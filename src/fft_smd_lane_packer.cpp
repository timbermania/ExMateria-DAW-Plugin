#include "fft_plugin/fft_smd_lane_packer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "fft_plugin/fft_smd_opcodes.h"

namespace fftplugin {

namespace {

// Local int32_t aliases derived from the central enum so the dense switch
// blocks below stay readable. The enum in fft_smd_opcodes.h is the source
// of truth; these are pure projections, not new constants.
constexpr int32_t OP_REST           = static_cast<int32_t>(FFTSmdOpcode::REST);
constexpr int32_t OP_FERMATA        = static_cast<int32_t>(FFTSmdOpcode::FERMATA);
constexpr int32_t OP_NOP            = static_cast<int32_t>(FFTSmdOpcode::NOP);
constexpr int32_t OP_END_BAR        = static_cast<int32_t>(FFTSmdOpcode::END_BAR);
constexpr int32_t OP_LOOP           = static_cast<int32_t>(FFTSmdOpcode::LOOP);
constexpr int32_t OP_OCTAVE         = static_cast<int32_t>(FFTSmdOpcode::OCTAVE);
constexpr int32_t OP_RAISE_OCTAVE   = static_cast<int32_t>(FFTSmdOpcode::RAISE_OCTAVE);
constexpr int32_t OP_LOWER_OCTAVE   = static_cast<int32_t>(FFTSmdOpcode::LOWER_OCTAVE);
constexpr int32_t OP_INSTRUMENT     = static_cast<int32_t>(FFTSmdOpcode::INSTRUMENT);
constexpr int32_t OP_REVERB_ON      = static_cast<int32_t>(FFTSmdOpcode::REVERB_ON);
constexpr int32_t OP_REVERB_OFF     = static_cast<int32_t>(FFTSmdOpcode::REVERB_OFF);
constexpr int32_t OP_DYNAMICS       = static_cast<int32_t>(FFTSmdOpcode::DYNAMICS);
constexpr int32_t OP_PAN            = static_cast<int32_t>(FFTSmdOpcode::PAN);

// Lane-packer opcode-set predicate now lives in fft_smd_opcodes.h as
// is_packer_known_opcode. Local alias keeps call sites concise.
constexpr bool is_known_opcode(int32_t op) {
    return is_packer_known_opcode(op);
}

struct LaneState {
    int32_t instrument = -1;
    int32_t octave = -1;
    int32_t dynamics = -1;
    int32_t pan = -1;
    int32_t reverb_on = -1;  // -1 unset, 0 off, 1 on
};

bool state_eq(const LaneState& a, const LaneState& b) {
    return a.instrument == b.instrument &&
           a.octave == b.octave &&
           a.dynamics == b.dynamics &&
           a.pan == b.pan &&
           a.reverb_on == b.reverb_on;
}

// One event with its absolute tick interval. For state opcodes,
// tick_end == tick_start (zero duration). For notes / rests / ferma,
// tick_end - tick_start is the time advance. For END_BAR / LOOP / NOP
// also zero-duration.
struct TickedEvent {
    int32_t tick_start = 0;
    int32_t tick_end = 0;
    bool is_note_sounding = false;  // true if the lane is producing sound during [tick_start, tick_end)
    FFTSmdTrackEvent ev;
};

struct LaneTimeline {
    std::vector<TickedEvent> events;
    LaneState preamble_state;  // state in effect at the very start, after preamble
    int32_t total_ticks = 0;
    bool packable = false;     // false if we saw any opcode outside is_known_opcode
};

// Pre-pass: replace every cumulative octave delta (RAISE_OCTAVE 0x95 /
// LOWER_OCTAVE 0x96) with an equivalent absolute OCTAVE 0x94 N opcode.
// Same musical result, +1 byte per replaced opcode, but the packer's
// state-diff save/restore becomes correct without having to predict
// what the host's NEXT cumulative delta will compose with our restore.
//
// If a stream has a RAISE/LOWER opcode while no prior absolute octave
// has been set (state.octave still -1), it's left alone — we don't know
// what to substitute. In practice every well-formed compile output sets
// OCTAVE in its preamble, so this branch is dead.
std::vector<FFTSmdTrackEvent> normalize_octave_deltas(
    const std::vector<FFTSmdTrackEvent>& events) {
    std::vector<FFTSmdTrackEvent> out;
    out.reserve(events.size());
    int32_t cur_octave = -1;
    for (const auto& ev : events) {
        if (const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&ev)) {
            if (opc->opcode == OP_OCTAVE) {
                if (!opc->params.empty()) cur_octave = opc->params[0];
                out.push_back(ev);
                continue;
            }
            if (opc->opcode == OP_RAISE_OCTAVE && cur_octave >= 0) {
                cur_octave += 1;
                FFTSmdOpcodeEvent rep;
                rep.opcode = OP_OCTAVE;
                rep.params = {cur_octave};
                out.emplace_back(std::move(rep));
                continue;
            }
            if (opc->opcode == OP_LOWER_OCTAVE && cur_octave >= 0) {
                cur_octave -= 1;
                FFTSmdOpcodeEvent rep;
                rep.opcode = OP_OCTAVE;
                rep.params = {cur_octave};
                out.emplace_back(std::move(rep));
                continue;
            }
        }
        out.push_back(ev);
    }
    return out;
}

// Walk a track's event stream, produce the timeline + sticky-state
// progression. Returns packable=false if any unknown opcode is present.
// Caller has already normalized RAISE/LOWER octave deltas to absolute
// OCTAVE opcodes via normalize_octave_deltas, so we should never see
// OP_RAISE_OCTAVE or OP_LOWER_OCTAVE here in practice.
LaneTimeline build_timeline(const std::vector<FFTSmdTrackEvent>& raw_events) {
    LaneTimeline tl;
    tl.packable = true;
    int32_t cursor = 0;
    LaneState state;
    const auto events = normalize_octave_deltas(raw_events);
    for (const auto& ev : events) {
        TickedEvent te;
        te.ev = ev;
        te.tick_start = cursor;
        te.tick_end = cursor;
        te.is_note_sounding = false;

        if (const auto* note = std::get_if<FFTSmdNoteEvent>(&ev)) {
            te.tick_end = cursor + note->delta_time;
            // relative_key 13 == rest, 12 == tie; both don't sound new audio.
            te.is_note_sounding = (note->relative_key >= 0 && note->relative_key <= 11)
                                  || note->relative_key == 12;  // tie continues prior note's pitch
            cursor = te.tick_end;
        } else if (const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&ev)) {
            if (!is_known_opcode(opc->opcode)) {
                tl.packable = false;
            }
            switch (opc->opcode) {
                case OP_REST: {
                    const int32_t d = !opc->params.empty() ? opc->params[0] : 0;
                    te.tick_end = cursor + d;
                    cursor = te.tick_end;
                    break;
                }
                case OP_FERMATA: {
                    const int32_t d = !opc->params.empty() ? opc->params[0] : 0;
                    te.tick_end = cursor + d;
                    te.is_note_sounding = true;  // extends the previous note
                    cursor = te.tick_end;
                    break;
                }
                case OP_OCTAVE:
                    if (!opc->params.empty()) state.octave = opc->params[0];
                    break;
                case OP_RAISE_OCTAVE:
                    if (state.octave >= 0) state.octave += 1;
                    break;
                case OP_LOWER_OCTAVE:
                    if (state.octave >= 0) state.octave -= 1;
                    break;
                case OP_INSTRUMENT:
                    if (!opc->params.empty()) state.instrument = opc->params[0];
                    break;
                case OP_REVERB_ON:  state.reverb_on = 1; break;
                case OP_REVERB_OFF: state.reverb_on = 0; break;
                case OP_DYNAMICS:
                    if (!opc->params.empty()) state.dynamics = opc->params[0];
                    break;
                case OP_PAN:
                    if (!opc->params.empty()) state.pan = opc->params[0];
                    break;
                default:
                    break;
            }
        }
        tl.events.push_back(te);
    }
    tl.total_ticks = cursor;
    // The "preamble state" we track for the lane is the state right after
    // the leading run of zero-duration opcodes (i.e. the preamble). Every
    // event index has its own running state but we only need the post-
    // preamble snapshot for the host's pre-insertion baseline.
    LaneState running;
    for (const auto& te : tl.events) {
        if (const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&te.ev)) {
            switch (opc->opcode) {
                case OP_OCTAVE:       if (!opc->params.empty()) running.octave = opc->params[0]; break;
                case OP_RAISE_OCTAVE: if (running.octave >= 0) running.octave += 1; break;
                case OP_LOWER_OCTAVE: if (running.octave >= 0) running.octave -= 1; break;
                case OP_INSTRUMENT:   if (!opc->params.empty()) running.instrument = opc->params[0]; break;
                case OP_REVERB_ON:    running.reverb_on = 1; break;
                case OP_REVERB_OFF:   running.reverb_on = 0; break;
                case OP_DYNAMICS:     if (!opc->params.empty()) running.dynamics = opc->params[0]; break;
                case OP_PAN:          if (!opc->params.empty()) running.pan = opc->params[0]; break;
                default: break;
            }
        }
        if (te.tick_end > te.tick_start) {
            // First event that consumed time — preamble is over.
            tl.preamble_state = running;
            break;
        }
    }
    return tl;
}

// Reconstruct the running sticky state immediately BEFORE the given
// event index (i.e., the state the lane is in when this event begins).
LaneState state_before(const LaneTimeline& tl, size_t event_idx) {
    LaneState s;
    for (size_t i = 0; i < event_idx && i < tl.events.size(); ++i) {
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&tl.events[i].ev);
        if (!opc) continue;
        switch (opc->opcode) {
            case OP_OCTAVE:       if (!opc->params.empty()) s.octave = opc->params[0]; break;
            case OP_RAISE_OCTAVE: if (s.octave >= 0) s.octave += 1; break;
            case OP_LOWER_OCTAVE: if (s.octave >= 0) s.octave -= 1; break;
            case OP_INSTRUMENT:   if (!opc->params.empty()) s.instrument = opc->params[0]; break;
            case OP_REVERB_ON:    s.reverb_on = 1; break;
            case OP_REVERB_OFF:   s.reverb_on = 0; break;
            case OP_DYNAMICS:     if (!opc->params.empty()) s.dynamics = opc->params[0]; break;
            case OP_PAN:          if (!opc->params.empty()) s.pan = opc->params[0]; break;
            default: break;
        }
    }
    return s;
}

// Bytes contributed to the SMD stream by one opcode event. Note: this
// matches the FFT SMD wire format (1 opcode byte + N param bytes).
int32_t opcode_byte_cost(int32_t op) {
    switch (op) {
        case OP_REVERB_ON: case OP_REVERB_OFF: case OP_LOOP: case OP_END_BAR:
        case OP_RAISE_OCTAVE: case OP_LOWER_OCTAVE: case OP_NOP:
            return 1;
        case OP_OCTAVE: case OP_INSTRUMENT: case OP_DYNAMICS:
        case OP_PAN: case OP_REST: case OP_FERMATA:
            return 2;
        default:
            return 2;  // conservative
    }
}

// Emit the minimum opcode sequence that drives `from` -> `to` for the
// state fields we track. Skips fields that match. Returns the opcodes
// + total byte cost.
struct StateDiffEmit {
    std::vector<FFTSmdTrackEvent> opcodes;
    int32_t bytes = 0;
};

StateDiffEmit emit_state_diff(const LaneState& from, const LaneState& to) {
    StateDiffEmit out;
    auto push = [&](int32_t op, std::vector<int32_t> params) {
        FFTSmdOpcodeEvent ev;
        ev.opcode = op;
        ev.params = std::move(params);
        out.bytes += opcode_byte_cost(op);
        out.opcodes.push_back(ev);
    };
    if (to.instrument >= 0 && to.instrument != from.instrument) {
        push(OP_INSTRUMENT, {to.instrument});
    }
    if (to.octave >= 0 && to.octave != from.octave) {
        push(OP_OCTAVE, {to.octave});
    }
    if (to.dynamics >= 0 && to.dynamics != from.dynamics) {
        push(OP_DYNAMICS, {to.dynamics});
    }
    if (to.pan >= 0 && to.pan != from.pan) {
        push(OP_PAN, {to.pan});
    }
    if (to.reverb_on >= 0 && to.reverb_on != from.reverb_on) {
        push(to.reverb_on ? OP_REVERB_ON : OP_REVERB_OFF, {});
    }
    return out;
}

// Find the smallest event-index range in `host` whose ticks span the
// closed-open interval [start, end). Used to locate REST opcodes that
// can be split for an insertion. Returns nullopt if [start, end)
// crosses an actual sounding note in host.
struct HostInsertionSlot {
    size_t event_index = 0;     // index of the REST opcode to split (or end-of-stream)
    int32_t rest_remaining = 0; // ticks left in the rest after our window starts
};

std::optional<HostInsertionSlot> find_insertion_slot(
    const LaneTimeline& host, int32_t start, int32_t end) {
    for (size_t i = 0; i < host.events.size(); ++i) {
        const auto& te = host.events[i];
        if (te.tick_end <= start) continue;
        if (te.tick_start >= end) {
            // We've passed the window without finding a covering rest.
            return std::nullopt;
        }
        if (te.is_note_sounding) {
            return std::nullopt;
        }
        // te is a non-sounding span (Rest opcode or zero-duration opcode)
        // overlapping [start, end). For simplicity require the span to
        // cover the entire window.
        if (te.tick_start <= start && te.tick_end >= end) {
            HostInsertionSlot slot;
            slot.event_index = i;
            slot.rest_remaining = te.tick_end - end;
            return slot;
        }
        // Partial cover — ignore (could be improved later).
        return std::nullopt;
    }
    return std::nullopt;  // ran off the end
}

// Estimate the byte savings from removing a lane entirely. The lane's
// preamble (DYNAMICS / PAN / INSTRUMENT / OCTAVE / REVERB_ON / LOOP), its
// trailing INSTRUMENT 0xFF mute + END_BAR, and its 2-byte offset-table
// entry all go away. Counted as the lane's zero-duration leading opcodes
// + 3 bytes (mute opcode + EndBar + offset entry).
int32_t lane_overhead_bytes(const LaneTimeline& tl) {
    int32_t bytes = 3;  // INSTRUMENT 0xFF (2) + END_BAR (1)... or close enough
    for (const auto& te : tl.events) {
        if (te.tick_end != te.tick_start) break;  // first time-consuming event ends preamble
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&te.ev);
        if (!opc) break;
        bytes += opcode_byte_cost(opc->opcode);
    }
    bytes += 2;  // offset table entry
    return bytes;
}

// Compute "weight" of a lane = number of non-rest, non-tie note events.
// Used to pick the lightest guest first.
int64_t lane_weight(const LaneTimeline& tl) {
    int64_t w = 0;
    for (const auto& te : tl.events) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&te.ev);
        if (note && note->relative_key >= 0 && note->relative_key <= 11) {
            w += note->delta_time + 1;
        }
    }
    return w;
}

// Render a timeline back to a flat event stream. The opcodes/notes are
// taken verbatim from te.ev; ticks are recomputed by the consumer if
// needed (the SMD format is delta-based, not absolute-tick, so the
// stream order is what matters).
std::vector<FFTSmdTrackEvent> render_to_stream(const LaneTimeline& tl) {
    std::vector<FFTSmdTrackEvent> out;
    out.reserve(tl.events.size());
    for (const auto& te : tl.events) out.push_back(te.ev);
    return out;
}

// Apply a single sticky-state opcode's effect to a LaneState.
void apply_sticky_opcode(const FFTSmdOpcodeEvent& opc, LaneState& state) {
    switch (opc.opcode) {
        case OP_OCTAVE:       if (!opc.params.empty()) state.octave = opc.params[0]; break;
        case OP_RAISE_OCTAVE: if (state.octave >= 0) state.octave += 1; break;
        case OP_LOWER_OCTAVE: if (state.octave >= 0) state.octave -= 1; break;
        case OP_INSTRUMENT:   if (!opc.params.empty()) state.instrument = opc.params[0]; break;
        case OP_REVERB_ON:    state.reverb_on = 1; break;
        case OP_REVERB_OFF:   state.reverb_on = 0; break;
        case OP_DYNAMICS:     if (!opc.params.empty()) state.dynamics = opc.params[0]; break;
        case OP_PAN:          if (!opc.params.empty()) state.pan = opc.params[0]; break;
        default: break;
    }
}

// One contiguous run of guest events that should be inserted into a
// single host free window as a unit. State at start = block_start_state;
// state at end = block_end_state. Bytes produced when serialized
// (without the surrounding state opcodes) are inferred from
// block_events by callers.
// One contiguous run of guest events — the unit of insertion. Built
// against an upper-bound window_end (= host's REST tick_end) so the
// block is the largest set of guest events that fits in that window.
struct Block {
    size_t guest_start_idx = 0;
    size_t guest_end_idx = 0;        // inclusive
    LaneState block_start_state;
    LaneState block_end_state;
    int32_t block_start_tick = 0;
    int32_t block_end_tick = 0;      // = last event's tick_end
};

// Walk guest from `gi` over leading non-sounding events (applying
// sticky-state to `guest_state`), then attempt to build a maximal
// block bounded by max_window_end. Returns the (optional) block,
// updated_gi (start of next attempt — if no block built, advances
// past leading opcodes), updated_state.
struct BuildBlockResult {
    std::optional<Block> block;
    size_t next_gi;
    LaneState updated_state;
};

BuildBlockResult build_next_block(
    const LaneTimeline& guest, size_t gi,
    LaneState guest_state, int32_t max_window_end) {
    BuildBlockResult out;
    while (gi < guest.events.size()) {
        const auto* gnote = std::get_if<FFTSmdNoteEvent>(&guest.events[gi].ev);
        const auto* gopc = std::get_if<FFTSmdOpcodeEvent>(&guest.events[gi].ev);
        if (gopc) {
            apply_sticky_opcode(*gopc, guest_state);
            ++gi;
            continue;
        }
        if (!gnote || gnote->relative_key == 13) {
            ++gi;
            continue;
        }
        break;
    }
    out.next_gi = gi;
    out.updated_state = guest_state;
    if (gi >= guest.events.size()) return out;

    const LaneState block_start_state = guest_state;
    const int32_t block_start_tick = guest.events[gi].tick_start;
    const int32_t first_note_end = guest.events[gi].tick_end;
    if (first_note_end > max_window_end) {
        // Doesn't fit; no block this round.
        return out;
    }

    size_t block_end_idx = gi;
    int32_t block_last_tick = first_note_end;
    LaneState walk_state = block_start_state;
    size_t j = gi;
    while (j < guest.events.size()) {
        // Atomic group: sounding event + trailing Fermatas + interleaved
        // zero-duration opcodes (state changes that we want to ride
        // along inside the block).
        size_t group_end = j;
        for (size_t k = j + 1; k < guest.events.size(); ++k) {
            const auto& kte = guest.events[k];
            const auto* kopc = std::get_if<FFTSmdOpcodeEvent>(&kte.ev);
            if (kopc && kopc->opcode == OP_FERMATA) { group_end = k; continue; }
            if (kopc && kte.tick_start == kte.tick_end) { group_end = k; continue; }
            break;
        }
        const int32_t group_tick_end = guest.events[group_end].tick_end;
        if (group_tick_end > max_window_end) break;
        for (size_t k = j; k <= group_end; ++k) {
            const auto* kopc = std::get_if<FFTSmdOpcodeEvent>(&guest.events[k].ev);
            if (kopc) apply_sticky_opcode(*kopc, walk_state);
        }
        block_end_idx = group_end;
        if (group_tick_end > block_last_tick) block_last_tick = group_tick_end;
        j = group_end + 1;
    }

    Block b;
    b.guest_start_idx = gi;
    b.guest_end_idx = block_end_idx;
    b.block_start_state = block_start_state;
    b.block_end_state = walk_state;
    b.block_start_tick = block_start_tick;
    b.block_end_tick = block_last_tick;
    out.block = std::move(b);
    out.next_gi = block_end_idx + 1;
    out.updated_state = walk_state;
    return out;
}

// Splice a built block into a host's free window. host is taken by
// value; on success returns the new host + bytes_added (forward +
// reverse state opcodes). On failure (no fitting REST in host or
// insertion would corrupt note counts) returns nullopt.
struct InsertResult {
    LaneTimeline packed_host;
    int32_t bytes_added = 0;
};

std::optional<InsertResult> try_insert_block_into_host(
    LaneTimeline host, const LaneTimeline& guest, const Block& b) {
    if (!host.packable) return std::nullopt;
    auto slot_opt = find_insertion_slot(host, b.block_start_tick, b.block_end_tick);
    if (!slot_opt) return std::nullopt;
    HostInsertionSlot slot = *slot_opt;
    const auto& orig_rest_te = host.events[slot.event_index];
    const int32_t window_end = orig_rest_te.tick_end;
    if (window_end < b.block_end_tick) return std::nullopt;  // defensive

    const LaneState host_state_at_slot = state_before(host, slot.event_index);
    StateDiffEmit forward = emit_state_diff(host_state_at_slot, b.block_start_state);
    StateDiffEmit reverse = emit_state_diff(b.block_end_state, host_state_at_slot);

    // Dead-store elimination on reverse: if no sounding events follow
    // in host, the restore is dead.
    bool host_has_later_sounding = false;
    for (size_t k = slot.event_index + 1; k < host.events.size(); ++k) {
        if (host.events[k].is_note_sounding) { host_has_later_sounding = true; break; }
    }
    if (!host_has_later_sounding) { reverse.opcodes.clear(); reverse.bytes = 0; }

    const int32_t prefix = b.block_start_tick - orig_rest_te.tick_start;
    const int32_t suffix = window_end - b.block_end_tick;

    std::vector<TickedEvent> replacement;
    auto emit_rest_chunks = [&](int32_t r, int32_t t) {
        while (r > 0) {
            int32_t c = std::min(r, 255);
            FFTSmdOpcodeEvent op; op.opcode = OP_REST; op.params = {c};
            TickedEvent te; te.tick_start = t; te.tick_end = t + c;
            te.is_note_sounding = false; te.ev = op;
            replacement.push_back(te);
            t += c; r -= c;
        }
    };
    if (prefix > 0) emit_rest_chunks(prefix, orig_rest_te.tick_start);
    for (auto& ev : forward.opcodes) {
        TickedEvent te; te.tick_start = b.block_start_tick; te.tick_end = b.block_start_tick;
        te.is_note_sounding = false; te.ev = ev;
        replacement.push_back(te);
    }
    for (size_t k = b.guest_start_idx; k <= b.guest_end_idx; ++k) {
        replacement.push_back(guest.events[k]);
    }
    for (auto& ev : reverse.opcodes) {
        TickedEvent te; te.tick_start = b.block_end_tick; te.tick_end = b.block_end_tick;
        te.is_note_sounding = false; te.ev = ev;
        replacement.push_back(te);
    }
    if (suffix > 0) emit_rest_chunks(suffix, b.block_end_tick);

    host.events.erase(host.events.begin() + static_cast<std::ptrdiff_t>(slot.event_index));
    host.events.insert(host.events.begin() + static_cast<std::ptrdiff_t>(slot.event_index),
                       replacement.begin(), replacement.end());
    InsertResult ir;
    ir.packed_host = std::move(host);
    ir.bytes_added = forward.bytes + reverse.bytes;
    return ir;
}

// Single-host packing: attempt to absorb every guest block into the
// same host. Used by callers that already have a chosen host.
std::optional<LaneTimeline> try_pack_guest_into_host(
    LaneTimeline host, const LaneTimeline& guest,
    int32_t* bytes_added_out, int32_t* blocks_out) {
    if (!host.packable || !guest.packable) return std::nullopt;
    int32_t bytes_added = 0;
    int32_t blocks_used = 0;
    LaneState guest_state;
    size_t gi = 0;
    while (gi < guest.events.size()) {
        // Find the host's free window starting at or covering gi's
        // first sounding tick. Build a block bounded by that window.
        // First locate the first sounding event in guest at/after gi.
        size_t scan = gi;
        LaneState scan_state = guest_state;
        while (scan < guest.events.size()) {
            const auto* gnote = std::get_if<FFTSmdNoteEvent>(&guest.events[scan].ev);
            const auto* gopc = std::get_if<FFTSmdOpcodeEvent>(&guest.events[scan].ev);
            if (gopc) { apply_sticky_opcode(*gopc, scan_state); ++scan; continue; }
            if (!gnote || gnote->relative_key == 13) { ++scan; continue; }
            break;
        }
        if (scan >= guest.events.size()) { gi = scan; break; }
        const int32_t bst = guest.events[scan].tick_start;
        const int32_t bne = guest.events[scan].tick_end;
        auto slot_opt = find_insertion_slot(host, bst, bne);
        if (!slot_opt) return std::nullopt;
        const int32_t window_end = host.events[slot_opt->event_index].tick_end;
        auto bb = build_next_block(guest, gi, guest_state, window_end);
        if (!bb.block) return std::nullopt;
        auto ir = try_insert_block_into_host(host, guest, *bb.block);
        if (!ir) return std::nullopt;
        host = std::move(ir->packed_host);
        bytes_added += ir->bytes_added;
        blocks_used += 1;
        guest_state = bb.updated_state;
        gi = bb.next_gi;
    }
    if (bytes_added_out) *bytes_added_out += bytes_added;
    if (blocks_out) *blocks_out += blocks_used;
    return host;
}

// Scatter packing: each guest block can land in a DIFFERENT host. We
// keep a working copy of every host, walk guest blocks, and for each
// block try every host (excluding `guest_idx`), picking the lowest-
// bytes-added host that fits. If any block can't find a home in any
// host, the scatter fails.
//
// Returns a vector of new host LaneTimelines (full size, including
// unmodified hosts) on success, total bytes added, and number of blocks.
struct ScatterResult {
    std::vector<LaneTimeline> updated_hosts;  // full-size: copies of all hosts, with affected ones updated
    int32_t bytes_added = 0;
    int32_t blocks_used = 0;
};

std::optional<ScatterResult> try_pack_guest_scatter(
    const std::vector<LaneTimeline>& hosts,
    const LaneTimeline& guest,
    size_t guest_idx,
    size_t skip_first_n_hosts) {
    if (!guest.packable) return std::nullopt;
    ScatterResult sr;
    sr.updated_hosts = hosts;  // copy
    LaneState guest_state;
    size_t gi = 0;
    while (gi < guest.events.size()) {
        // Find guest's next sounding event to determine block_start_tick.
        size_t scan = gi;
        LaneState scan_state = guest_state;
        while (scan < guest.events.size()) {
            const auto* gnote = std::get_if<FFTSmdNoteEvent>(&guest.events[scan].ev);
            const auto* gopc = std::get_if<FFTSmdOpcodeEvent>(&guest.events[scan].ev);
            if (gopc) { apply_sticky_opcode(*gopc, scan_state); ++scan; continue; }
            if (!gnote || gnote->relative_key == 13) { ++scan; continue; }
            break;
        }
        if (scan >= guest.events.size()) { gi = scan; break; }
        const int32_t bst = guest.events[scan].tick_start;
        const int32_t bne = guest.events[scan].tick_end;

        // Try each candidate host. Pick the one with lowest bytes_added.
        std::optional<size_t> best_host;
        std::optional<InsertResult> best_ir;
        std::optional<BuildBlockResult> best_bb;
        for (size_t hi = skip_first_n_hosts; hi < sr.updated_hosts.size(); ++hi) {
            if (hi == guest_idx) continue;
            if (!sr.updated_hosts[hi].packable) continue;
            auto slot_opt = find_insertion_slot(sr.updated_hosts[hi], bst, bne);
            if (!slot_opt) continue;
            const int32_t window_end =
                sr.updated_hosts[hi].events[slot_opt->event_index].tick_end;
            auto bb = build_next_block(guest, gi, guest_state, window_end);
            if (!bb.block) continue;
            auto ir = try_insert_block_into_host(
                sr.updated_hosts[hi], guest, *bb.block);
            if (!ir) continue;
            if (!best_ir || ir->bytes_added < best_ir->bytes_added) {
                best_host = hi;
                best_ir = std::move(ir);
                best_bb = std::move(bb);
            }
        }
        if (!best_host) return std::nullopt;  // no host fits this block
        sr.updated_hosts[*best_host] = std::move(best_ir->packed_host);
        sr.bytes_added += best_ir->bytes_added;
        sr.blocks_used += 1;
        guest_state = best_bb->updated_state;
        gi = best_bb->next_gi;
    }
    return sr;
}

// Sum the delta_times of every sounding note (rel_key 0..11). Used as
// a packer sanity check: a merge should preserve the per-note duration
// signature exactly — guest's notes go in unchanged plus host's notes
// stay unchanged. Any drift means a Fermata or duration was clipped.
int64_t sum_note_durations(const LaneTimeline& tl) {
    int64_t s = 0;
    for (const auto& te : tl.events) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&te.ev);
        if (note && note->relative_key >= 0 && note->relative_key <= 11) {
            s += note->delta_time;
        }
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&te.ev);
        if (opc && opc->opcode == OP_FERMATA && !opc->params.empty()) {
            s += opc->params[0];
        }
    }
    return s;
}

}  // namespace

FFTSmdFile pack_lanes_to_track_budget(
    const FFTSmdFile& smd, int32_t target_track_count,
    int32_t first_packable_track, FFTSmdLanePackerReport* report) {
    FFTSmdLanePackerReport local_report;
    local_report.lanes_in = smd.track_count;

    if (smd.track_count <= target_track_count) {
        local_report.ok = true;
        local_report.lanes_out = smd.track_count;
        if (report) *report = local_report;
        return smd;
    }

    const size_t skip = std::max<size_t>(0, static_cast<size_t>(first_packable_track));
    std::vector<LaneTimeline> lanes;
    lanes.reserve(smd.track_events.size());
    for (size_t ti = 0; ti < skip && ti < smd.track_events.size(); ++ti) {
        lanes.push_back(LaneTimeline{});  // placeholder, won't be packed
    }
    for (size_t ti = skip; ti < smd.track_events.size(); ++ti) {
        lanes.push_back(build_timeline(smd.track_events[ti]));
    }

    int32_t total_packed_notes = 0;
    int32_t total_state_bytes = 0;
    const bool dbg = std::getenv("FFT_PACKER_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr, "[packer] start: lanes=%d target=%d\n",
                     (int)lanes.size(), (int)target_track_count);
    }

    // Score-based copy-coalescing (Briggs/George 1996, scored by
    // music-domain payoff). Each iteration: enumerate every (guest,
    // host) pair that can pack, score by `notes_moved -
    // state_flip_byte_cost`, commit the best, repeat. Stop the moment
    // we hit `target_track_count` — never over-reduce.
    auto count_sounding_notes = [](const LaneTimeline& tl) {
        int32_t n = 0;
        for (const auto& te : tl.events) {
            const auto* note = std::get_if<FFTSmdNoteEvent>(&te.ev);
            if (note && note->relative_key >= 0 && note->relative_key <= 11) n += 1;
        }
        return n;
    };
    auto guest_tick_range = [](const LaneTimeline& tl) -> std::pair<int32_t, int32_t> {
        int32_t lo = -1, hi_t = -1;
        for (const auto& te : tl.events) {
            const auto* note = std::get_if<FFTSmdNoteEvent>(&te.ev);
            if (note && note->relative_key >= 0 && note->relative_key <= 11) {
                if (lo < 0 || te.tick_start < lo) lo = te.tick_start;
                if (te.tick_end > hi_t) hi_t = te.tick_end;
            }
        }
        return {lo < 0 ? 0 : lo, hi_t < 0 ? 0 : hi_t};
    };

    // Scatter candidate: a guest absorbed across (potentially) multiple
    // hosts. We don't track per-block (host_idx) anymore — `host_idx`
    // is set to the FIRST modified host for compat with the report
    // shape, and `updated_hosts` carries the full new state.
    struct Candidate {
        size_t guest_idx;
        size_t host_idx;                      // first modified host (for report)
        std::vector<LaneTimeline> updated_hosts;
        int32_t bytes_added;
        int32_t notes_moved;
        int32_t blocks;
        int64_t score;
    };

    while (static_cast<int32_t>(lanes.size()) > target_track_count) {
        std::vector<Candidate> candidates;
        for (size_t gi = skip; gi < lanes.size(); ++gi) {
            if (!lanes[gi].packable) continue;
            const int32_t guest_notes = count_sounding_notes(lanes[gi]);

            // Try scatter packing this guest across all OTHER hosts.
            auto sr_opt = try_pack_guest_scatter(lanes, lanes[gi], gi, skip);
            if (!sr_opt) continue;

            // Correctness check: total note duration across ALL host
            // lanes (excluding the guest itself) before vs after must
            // match guest's contribution.
            int64_t pre_total = sum_note_durations(lanes[gi]);
            for (size_t hi = skip; hi < lanes.size(); ++hi) {
                if (hi == gi) continue;
                pre_total += sum_note_durations(lanes[hi]);
            }
            int64_t post_total = 0;
            for (size_t hi = skip; hi < sr_opt->updated_hosts.size(); ++hi) {
                if (hi == gi) continue;
                post_total += sum_note_durations(sr_opt->updated_hosts[hi]);
            }
            if (post_total != pre_total) {
                if (dbg) std::fprintf(stderr,
                    "[packer]   reject guest=%zu scatter: duration sum changed (%lld -> %lld)\n",
                    gi, (long long)pre_total, (long long)post_total);
                continue;
            }

            // Find first modified host for the report.
            size_t first_modified = gi;
            for (size_t hi = skip; hi < sr_opt->updated_hosts.size(); ++hi) {
                if (hi == gi) continue;
                if (sr_opt->updated_hosts[hi].events.size() != lanes[hi].events.size()) {
                    first_modified = hi;
                    break;
                }
            }

            Candidate c;
            c.guest_idx = gi;
            c.host_idx = first_modified;
            c.updated_hosts = std::move(sr_opt->updated_hosts);
            c.bytes_added = sr_opt->bytes_added;
            c.notes_moved = guest_notes;
            c.blocks = sr_opt->blocks_used;
            c.score = static_cast<int64_t>(guest_notes) - static_cast<int64_t>(sr_opt->bytes_added);
            candidates.push_back(std::move(c));
        }
        if (candidates.empty()) break;

        auto best_it = std::max_element(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
        Candidate best = std::move(*best_it);

        if (dbg) std::fprintf(stderr,
            "[packer] commit best: guest=%zu host=%zu notes=%d ba=%d blocks=%d score=%lld\n",
            best.guest_idx, best.host_idx, (int)best.notes_moved,
            (int)best.bytes_added, (int)best.blocks, (long long)best.score);

        const auto [gmin, gmax] = guest_tick_range(lanes[best.guest_idx]);
        FFTSmdLanePackerMergeRecord rec;
        rec.guest_lane_index = static_cast<int32_t>(best.guest_idx);
        rec.host_lane_index = static_cast<int32_t>(best.host_idx);
        rec.notes_moved = best.notes_moved;
        rec.state_bytes_added = best.bytes_added;
        rec.guest_min_tick = gmin;
        rec.guest_max_tick = gmax;
        rec.blocks = best.blocks;
        local_report.merges.push_back(rec);

        // Commit: replace ALL hosts (some may have been modified by
        // scatter, others identical), then drop the guest. Indices in
        // best.updated_hosts are aligned with lanes pre-erase.
        for (size_t hi = 0; hi < best.updated_hosts.size() && hi < lanes.size(); ++hi) {
            if (hi == best.guest_idx) continue;
            lanes[hi] = std::move(best.updated_hosts[hi]);
        }
        lanes.erase(lanes.begin() + best.guest_idx);
        local_report.lanes_fully_absorbed += 1;
        total_packed_notes += best.notes_moved;
        total_state_bytes += best.bytes_added;
    }

    local_report.lanes_out = static_cast<int32_t>(lanes.size());
    local_report.notes_packed = total_packed_notes;
    local_report.state_change_bytes_added = total_state_bytes;
    local_report.ok = (static_cast<int32_t>(lanes.size()) <= target_track_count);

    // Rebuild FFTSmdFile.
    FFTSmdFile out = smd;
    out.track_events.clear();
    for (size_t i = 0; i < skip && i < smd.track_events.size(); ++i) {
        out.track_events.push_back(smd.track_events[i]);
    }
    for (size_t i = skip; i < lanes.size(); ++i) {
        out.track_events.push_back(render_to_stream(lanes[i]));
    }
    out.track_count = static_cast<int32_t>(out.track_events.size());

    if (report) *report = local_report;
    return out;
}

}  // namespace fftplugin
