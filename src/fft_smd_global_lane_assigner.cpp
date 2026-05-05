#include "fft_plugin/fft_smd_global_lane_assigner.h"

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

namespace fftplugin {

namespace {

constexpr int32_t OP_REST           = 0x80;
constexpr int32_t OP_FERMATA        = 0x81;
constexpr int32_t OP_END_BAR        = 0x90;
constexpr int32_t OP_LOOP           = 0x91;
constexpr int32_t OP_OCTAVE         = 0x94;
constexpr int32_t OP_RAISE_OCTAVE   = 0x95;
constexpr int32_t OP_LOWER_OCTAVE   = 0x96;
constexpr int32_t OP_INSTRUMENT     = 0xAC;
constexpr int32_t OP_REVERB_ON      = 0xBA;
constexpr int32_t OP_REVERB_OFF     = 0xBB;
constexpr int32_t OP_DYNAMICS       = 0xE0;
constexpr int32_t OP_PAN            = 0xE8;

struct State {
    int32_t instrument = -1;
    int32_t octave = -1;
    int32_t dynamics = -1;
    int32_t pan = -1;
    int32_t reverb_on = -1;
};

bool state_eq(const State& a, const State& b) {
    return a.instrument == b.instrument && a.octave == b.octave &&
           a.dynamics == b.dynamics && a.pan == b.pan &&
           a.reverb_on == b.reverb_on;
}

void apply_sticky(const FFTSmdOpcodeEvent& opc, State& s) {
    switch (opc.opcode) {
        case OP_OCTAVE:       if (!opc.params.empty()) s.octave = opc.params[0]; break;
        case OP_RAISE_OCTAVE: if (s.octave >= 0) s.octave += 1; break;
        case OP_LOWER_OCTAVE: if (s.octave >= 0) s.octave -= 1; break;
        case OP_INSTRUMENT:   if (!opc.params.empty()) s.instrument = opc.params[0]; break;
        case OP_REVERB_ON:    s.reverb_on = 1; break;
        case OP_REVERB_OFF:   s.reverb_on = 0; break;
        case OP_DYNAMICS:     if (!opc.params.empty()) s.dynamics = opc.params[0]; break;
        case OP_PAN:          if (!opc.params.empty()) s.pan = opc.params[0]; break;
        default: break;
    }
}

// One sounding note + any trailing Fermatas, with the state required
// to play it correctly.
struct GlobalNote {
    int32_t start_tick = 0;
    int32_t end_tick = 0;            // start_tick + delta_time + sum(fermata.params)
    State required_state;
    FFTSmdNoteEvent note;
    std::vector<FFTSmdOpcodeEvent> trailing_fermatas;
    int32_t source_track_index = 0;
};

// Walk one track, emit GlobalNote per sounding event. Skips rests and
// non-sticky opcodes. Does NOT include the conductor track 0.
void collect_notes_from_track(
    const std::vector<FFTSmdTrackEvent>& events,
    int32_t track_index,
    std::vector<GlobalNote>& out) {
    State s;
    int32_t cursor = 0;
    size_t i = 0;
    while (i < events.size()) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&events[i]);
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&events[i]);
        if (opc) {
            apply_sticky(*opc, s);
            if (opc->opcode == OP_REST && !opc->params.empty()) {
                cursor += opc->params[0];
            }
            ++i;
            continue;
        }
        if (!note) { ++i; continue; }
        if (note->relative_key < 0 || note->relative_key > 11) {
            // Rest-encoded note (rel_key 13) or tie (12). Advance cursor.
            cursor += std::max(note->delta_time, 1);
            ++i;
            continue;
        }
        // Sounding note. Bundle trailing Fermatas + zero-duration
        // sticky opcodes (state changes ride along).
        GlobalNote g;
        g.start_tick = cursor;
        g.end_tick = cursor + note->delta_time;
        g.required_state = s;
        g.note = *note;
        g.source_track_index = track_index;
        cursor = g.end_tick;
        ++i;
        while (i < events.size()) {
            const auto* nopc = std::get_if<FFTSmdOpcodeEvent>(&events[i]);
            if (!nopc) break;
            if (nopc->opcode == OP_FERMATA && !nopc->params.empty()) {
                g.trailing_fermatas.push_back(*nopc);
                g.end_tick += nopc->params[0];
                cursor = g.end_tick;
                ++i;
                continue;
            }
            if (nopc->opcode == OP_REST) break;  // rest belongs to following events
            if (nopc->opcode == OP_END_BAR) break;
            // Other opcode (sticky state change). Apply and consume —
            // but mark it so subsequent notes pick up the new state.
            apply_sticky(*nopc, s);
            ++i;
        }
        out.push_back(std::move(g));
    }
}

// State-diff opcode emission, mirrors the lane packer's logic. Skips
// fields where from == to, returning empty if nothing changed.
struct StateDiffEmit {
    std::vector<FFTSmdOpcodeEvent> opcodes;
    int32_t bytes = 0;
};

int32_t opcode_byte_cost(int32_t op) {
    switch (op) {
        case OP_REVERB_ON: case OP_REVERB_OFF: return 1;
        case OP_OCTAVE: case OP_INSTRUMENT: case OP_DYNAMICS:
        case OP_PAN: case OP_REST: case OP_FERMATA: return 2;
        default: return 2;
    }
}

StateDiffEmit emit_state_diff(const State& from, const State& to) {
    StateDiffEmit out;
    auto push = [&](int32_t op, std::vector<int32_t> params) {
        FFTSmdOpcodeEvent ev;
        ev.opcode = op;
        ev.params = std::move(params);
        out.bytes += opcode_byte_cost(op);
        out.opcodes.push_back(std::move(ev));
    };
    if (to.instrument >= 0 && to.instrument != from.instrument) push(OP_INSTRUMENT, {to.instrument});
    if (to.octave >= 0 && to.octave != from.octave) push(OP_OCTAVE, {to.octave});
    if (to.dynamics >= 0 && to.dynamics != from.dynamics) push(OP_DYNAMICS, {to.dynamics});
    if (to.pan >= 0 && to.pan != from.pan) push(OP_PAN, {to.pan});
    if (to.reverb_on >= 0 && to.reverb_on != from.reverb_on) {
        push(to.reverb_on ? OP_REVERB_ON : OP_REVERB_OFF, {});
    }
    return out;
}

// One output lane being built incrementally.
struct OutLane {
    std::vector<FFTSmdTrackEvent> events;
    int32_t cursor_tick = 0;       // current tick position in the lane
    int32_t busy_until = 0;        // when the most-recent note ends
    State current_state;           // state in effect right now
    bool initialized = false;      // false until first event written
};

void emit_rest(std::vector<FFTSmdTrackEvent>& out, int32_t ticks) {
    while (ticks > 0) {
        int32_t c = std::min(ticks, 255);
        FFTSmdOpcodeEvent op;
        op.opcode = OP_REST;
        op.params = {c};
        out.emplace_back(std::move(op));
        ticks -= c;
    }
}

}  // namespace

FFTSmdFile global_lane_reassign(
    const FFTSmdFile& smd, int32_t max_lanes,
    int32_t first_packable_track,
    FFTSmdGlobalLaneAssignerReport* report) {
    FFTSmdGlobalLaneAssignerReport rpt;
    rpt.ok = true;
    rpt.lanes_in = smd.track_count;

    FFTSmdFile out;
    out.initial_tempo = smd.initial_tempo;
    out.initial_volume = smd.initial_volume;
    out.assoc_wds_id = smd.assoc_wds_id;
    out.song_title = smd.song_title;

    const size_t skip = std::max<size_t>(0, static_cast<size_t>(first_packable_track));
    // Preserve any pre-skip tracks as-is (e.g., conductor track 0
    // when first_packable_track == 1).
    for (size_t i = 0; i < skip && i < smd.track_events.size(); ++i) {
        out.track_events.push_back(smd.track_events[i]);
    }

    // Collect sounding notes from every reassignable track.
    std::vector<GlobalNote> notes;
    notes.reserve(2048);
    for (size_t ti = skip; ti < smd.track_events.size(); ++ti) {
        collect_notes_from_track(smd.track_events[ti],
                                 static_cast<int32_t>(ti), notes);
    }
    rpt.notes_assigned = static_cast<int32_t>(notes.size());

    // Sort by start_tick. Tie-break by source track index for stability.
    std::sort(notes.begin(), notes.end(),
        [](const GlobalNote& a, const GlobalNote& b) {
            if (a.start_tick != b.start_tick) return a.start_tick < b.start_tick;
            return a.source_track_index < b.source_track_index;
        });

    // Linear-scan assignment with state-affinity tiebreak.
    std::vector<OutLane> lanes;
    int32_t total_state_bytes = 0;
    int32_t free_assignments = 0;

    for (const auto& g : notes) {
        // Find candidate lanes: those whose busy_until <= g.start_tick.
        // Among them, prefer:
        //  (1) lane whose current_state == g.required_state (zero state cost)
        //  (2) lane with lowest state-flip cost
        // If none free and lanes.size() < max_lanes, open a new lane.
        int32_t best_lane = -1;
        int32_t best_cost = INT32_MAX;
        for (size_t li = 0; li < lanes.size(); ++li) {
            if (lanes[li].busy_until > g.start_tick) continue;
            const auto diff = emit_state_diff(lanes[li].current_state, g.required_state);
            const int32_t cost = diff.bytes;
            if (cost < best_cost) {
                best_cost = cost;
                best_lane = static_cast<int32_t>(li);
                if (cost == 0) break;  // can't improve
            }
        }
        // If no free lane found AND we can open a new one, do so.
        // Cap on lanes we can open: the engine budget minus tracks we
        // preserved (conductor etc.).
        const int32_t lane_cap = max_lanes - static_cast<int32_t>(skip);
        if (best_lane < 0 && static_cast<int32_t>(lanes.size()) < lane_cap) {
            // skip-aware budget.
            lanes.emplace_back();
            best_lane = static_cast<int32_t>(lanes.size()) - 1;
            best_cost = 0;  // new lane has unknown state; first emit_state_diff applied below treats it as fresh
        }
        if (best_lane < 0) {
            // No lane available — would have to drop this note. For now,
            // bail out and let the packer/Stage A pipeline handle it.
            rpt.ok = false;
            rpt.error = "global lane assigner: no free lane and at max_lanes; not yet implemented spill heuristic";
            // Return the input unchanged so callers fall back to per-part compile.
            if (report) *report = rpt;
            return smd;
        }

        OutLane& L = lanes[static_cast<size_t>(best_lane)];

        // Emit any rest needed to advance cursor to g.start_tick.
        if (g.start_tick > L.cursor_tick) {
            emit_rest(L.events, g.start_tick - L.cursor_tick);
            L.cursor_tick = g.start_tick;
        }

        // Emit state-flip opcodes as needed.
        const auto diff = emit_state_diff(L.current_state, g.required_state);
        for (auto& op : diff.opcodes) {
            L.events.emplace_back(op);
        }
        total_state_bytes += diff.bytes;
        if (diff.bytes == 0) free_assignments += 1;
        // Apply the state changes to L.current_state.
        for (const auto& op : diff.opcodes) apply_sticky(op, L.current_state);

        // Emit the note + Fermatas.
        L.events.emplace_back(g.note);
        L.cursor_tick += g.note.delta_time;
        for (const auto& f : g.trailing_fermatas) {
            L.events.emplace_back(f);
            L.cursor_tick += !f.params.empty() ? f.params[0] : 0;
        }
        L.busy_until = g.end_tick;
        L.initialized = true;
    }

    // Finalize each lane: append INSTRUMENT 0xFF mute (if any notes
    // played) then EndBar.
    for (auto& L : lanes) {
        if (L.initialized) {
            FFTSmdOpcodeEvent mute;
            mute.opcode = OP_INSTRUMENT;
            mute.params = {0xFF};
            L.events.emplace_back(std::move(mute));
        }
        FFTSmdOpcodeEvent endbar;
        endbar.opcode = OP_END_BAR;
        L.events.emplace_back(std::move(endbar));
    }

    // Push lanes into output SMD.
    for (auto& L : lanes) {
        out.track_events.push_back(std::move(L.events));
    }
    out.track_count = static_cast<int32_t>(out.track_events.size());

    rpt.lanes_out = out.track_count;
    rpt.state_flip_bytes = total_state_bytes;
    rpt.free_assignments = free_assignments;

    if (report) *report = rpt;
    return out;
}

}  // namespace fftplugin
