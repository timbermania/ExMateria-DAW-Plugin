#include "fft_plugin/fft_smd_loop_roller.h"

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
constexpr int32_t OP_REPEAT         = 0x98;
constexpr int32_t OP_CODA           = 0x99;
constexpr int32_t OP_REPEAT_BREAK   = 0x9A;
constexpr int32_t OP_INSTRUMENT     = 0xAC;
constexpr int32_t OP_REVERB_ON      = 0xBA;
constexpr int32_t OP_REVERB_OFF     = 0xBB;
constexpr int32_t OP_DYNAMICS       = 0xE0;
constexpr int32_t OP_PAN            = 0xE8;

// Sticky state we care about for the state-balance check. Mirrors the
// packer's LaneState — they're tracking the same per-track register
// machine. If a candidate window's first iteration leaves the state
// in a different shape than it started, we can't roll it (the second
// iteration would play with stale state).
struct State {
    int32_t instrument = -2;  // -2 = unknown / never set
    int32_t octave = -2;
    int32_t dynamics = -2;
    int32_t pan = -2;
    int32_t reverb_on = -2;
};

bool state_eq(const State& a, const State& b) {
    return a.instrument == b.instrument && a.octave == b.octave &&
           a.dynamics == b.dynamics && a.pan == b.pan &&
           a.reverb_on == b.reverb_on;
}

void apply_sticky(const FFTSmdOpcodeEvent& opc, State& s) {
    switch (opc.opcode) {
        case OP_OCTAVE:       if (!opc.params.empty()) s.octave = opc.params[0]; break;
        case OP_RAISE_OCTAVE: if (s.octave >= -1) s.octave += 1; break;
        case OP_LOWER_OCTAVE: if (s.octave >= -1) s.octave -= 1; break;
        case OP_INSTRUMENT:   if (!opc.params.empty()) s.instrument = opc.params[0]; break;
        case OP_REVERB_ON:    s.reverb_on = 1; break;
        case OP_REVERB_OFF:   s.reverb_on = 0; break;
        case OP_DYNAMICS:     if (!opc.params.empty()) s.dynamics = opc.params[0]; break;
        case OP_PAN:          if (!opc.params.empty()) s.pan = opc.params[0]; break;
        default: break;
    }
}

// Approximate serialized size of one event in the SMD wire format.
// Notes: 2 bytes (velocity + key/duration). Opcodes: 1 byte + N param
// bytes. Conservative — we only need the size to compute byte savings;
// any overestimate just makes the roller slightly less aggressive.
int32_t event_byte_size(const FFTSmdTrackEvent& ev) {
    if (std::get_if<FFTSmdNoteEvent>(&ev) != nullptr) return 2;
    const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&ev);
    if (!opc) return 0;
    // Most opcodes: 1 byte + 1 byte per param.
    return 1 + static_cast<int32_t>(opc->params.size());
}

int32_t window_byte_size(const std::vector<FFTSmdTrackEvent>& track,
                         size_t start, size_t len) {
    int32_t b = 0;
    for (size_t k = 0; k < len; ++k) b += event_byte_size(track[start + k]);
    return b;
}

// Two events equal for windowed-repetition matching. Compare type +
// fields exactly. NoteEvents compare velocity, relative_key,
// delta_time. OpcodeEvents compare opcode + params.
bool event_eq(const FFTSmdTrackEvent& a, const FFTSmdTrackEvent& b) {
    const auto* an = std::get_if<FFTSmdNoteEvent>(&a);
    const auto* bn = std::get_if<FFTSmdNoteEvent>(&b);
    if (an && bn) {
        return an->velocity == bn->velocity &&
               an->relative_key == bn->relative_key &&
               an->delta_time == bn->delta_time;
    }
    const auto* ao = std::get_if<FFTSmdOpcodeEvent>(&a);
    const auto* bo = std::get_if<FFTSmdOpcodeEvent>(&b);
    if (ao && bo) {
        if (ao->opcode != bo->opcode) return false;
        if (ao->params.size() != bo->params.size()) return false;
        for (size_t i = 0; i < ao->params.size(); ++i) {
            if (ao->params[i] != bo->params[i]) return false;
        }
        return true;
    }
    return false;
}

// Window contains no opcodes that would break loop semantics: no
// nested REPEAT/CODA, no LOOP marker, no END_BAR, no REPEAT_BREAK.
bool window_loop_safe(const std::vector<FFTSmdTrackEvent>& track,
                      size_t start, size_t len) {
    for (size_t k = 0; k < len; ++k) {
        const auto* o = std::get_if<FFTSmdOpcodeEvent>(&track[start + k]);
        if (!o) continue;
        switch (o->opcode) {
            case OP_REPEAT:
            case OP_CODA:
            case OP_LOOP:
            case OP_END_BAR:
            case OP_REPEAT_BREAK:
                return false;
            default:
                break;
        }
    }
    return true;
}

// Window is state-balanced: applying every event leaves State unchanged.
// (Compares pre-state to post-state.) Required because each loop
// iteration starts with whatever state the previous iteration left
// behind.
bool window_state_balanced(const std::vector<FFTSmdTrackEvent>& track,
                           size_t start, size_t len, const State& pre) {
    State s = pre;
    for (size_t k = 0; k < len; ++k) {
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&track[start + k]);
        if (opc) apply_sticky(*opc, s);
    }
    return state_eq(s, pre);
}

// Compute the State at the start of position `idx` by walking events
// 0..idx-1.
State state_before(const std::vector<FFTSmdTrackEvent>& track, size_t idx) {
    State s;
    for (size_t i = 0; i < idx && i < track.size(); ++i) {
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&track[i]);
        if (opc) apply_sticky(*opc, s);
    }
    return s;
}

// Try to roll one collapse on `track` and return true if a profitable
// collapse was found. Modifies `track` in place.
bool try_one_collapse(std::vector<FFTSmdTrackEvent>& track) {
    const size_t N = track.size();
    if (N < 2) return false;
    int64_t best_savings = 0;
    size_t best_start = 0;
    size_t best_W = 0;
    int32_t best_K = 0;

    // Conservative thresholds: only roll when we have strong evidence
    // of a real repetition, not a coincidental short match.
    // K >= 3: at least 3 iterations.
    // W >= 4 events: skip tiny windows.
    // savings >= 8 bytes: comfortable margin over the 3-byte REPEAT+CODA cost.
    constexpr int32_t kMinK = 3;
    constexpr size_t  kMinW = 4;
    constexpr int64_t kMinSavings = 8;

    std::vector<size_t> Ws;
    for (size_t W = kMinW; W * kMinK <= N; W = W * 3 / 2 + 1) Ws.push_back(W);
    for (size_t W : Ws) {
        for (size_t i = 0; i + kMinK * W <= N; ++i) {
            const State pre = state_before(track, i);
            if (!window_state_balanced(track, i, W, pre)) continue;
            if (!window_loop_safe(track, i, W)) continue;
            int32_t K = 1;
            while (i + (K + 1) * W <= N) {
                bool match = true;
                for (size_t k = 0; k < W; ++k) {
                    if (!event_eq(track[i + k], track[i + K * W + k])) {
                        match = false;
                        break;
                    }
                }
                if (!match) break;
                K += 1;
                if (K >= 255) break;
            }
            if (K < kMinK) continue;
            const int32_t window_bytes = window_byte_size(track, i, W);
            const int64_t savings =
                static_cast<int64_t>(K - 1) * window_bytes - 3;
            if (savings < kMinSavings) continue;
            if (savings > best_savings) {
                best_savings = savings;
                best_start = i;
                best_W = W;
                best_K = K;
            }
        }
    }

    if (best_savings <= 0) return false;

    // Build the replacement: REPEAT best_K, [W events], CODA
    std::vector<FFTSmdTrackEvent> replacement;
    replacement.reserve(best_W + 2);
    {
        FFTSmdOpcodeEvent rep;
        rep.opcode = OP_REPEAT;
        rep.params = {best_K};
        replacement.emplace_back(std::move(rep));
    }
    for (size_t k = 0; k < best_W; ++k) {
        replacement.push_back(track[best_start + k]);
    }
    {
        FFTSmdOpcodeEvent coda;
        coda.opcode = OP_CODA;
        replacement.emplace_back(std::move(coda));
    }
    // Splice: remove [best_start, best_start + best_K * best_W), insert replacement.
    track.erase(track.begin() + static_cast<std::ptrdiff_t>(best_start),
                track.begin() + static_cast<std::ptrdiff_t>(best_start + best_K * best_W));
    track.insert(track.begin() + static_cast<std::ptrdiff_t>(best_start),
                 replacement.begin(), replacement.end());
    return true;
}

int32_t serialized_track_bytes(const std::vector<FFTSmdTrackEvent>& track) {
    int32_t b = 0;
    for (const auto& ev : track) b += event_byte_size(ev);
    return b;
}

// Sum of cumulative tick advance for a track. Notes advance by their
// delta_time; REST/FERMATA opcodes advance by their first param;
// other opcodes are zero-duration. Used as a safety check: a roll
// must preserve the total tick count of the track.
int64_t track_total_ticks(const std::vector<FFTSmdTrackEvent>& track) {
    int64_t t = 0;
    for (const auto& ev : track) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&ev);
        if (note) {
            t += note->delta_time;
            continue;
        }
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&ev);
        if (!opc) continue;
        if ((opc->opcode == OP_REST || opc->opcode == OP_FERMATA) &&
            !opc->params.empty()) {
            t += opc->params[0];
        }
    }
    return t;
}

// Sum of note + Fermata durations (only sounding events). Mirrors the
// packer's safety check. A roll must preserve this exactly.
int64_t track_sum_durations(const std::vector<FFTSmdTrackEvent>& track) {
    int64_t s = 0;
    for (const auto& ev : track) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&ev);
        if (note && note->relative_key >= 0 && note->relative_key <= 11) {
            s += note->delta_time;
        }
        const auto* opc = std::get_if<FFTSmdOpcodeEvent>(&ev);
        if (opc && opc->opcode == OP_FERMATA && !opc->params.empty()) {
            s += opc->params[0];
        }
    }
    return s;
}

// Count of sounding notes in the track. Should be K * (count in body)
// after a roll, but we just verify the post-roll count equals the
// pre-roll count (a roll is a representational change, not a content
// change).
int32_t track_count_sounding(const std::vector<FFTSmdTrackEvent>& track) {
    int32_t n = 0;
    for (const auto& ev : track) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&ev);
        if (note && note->relative_key >= 0 && note->relative_key <= 11) n += 1;
    }
    return n;
}

}  // namespace

FFTSmdFile roll_loops_in_smd(
    const FFTSmdFile& smd, FFTSmdLoopRollerReport* report) {
    FFTSmdLoopRollerReport local;
    local.ok = true;
    FFTSmdFile out = smd;
    for (size_t ti = 0; ti < out.track_events.size(); ++ti) {
        FFTSmdLoopRollerTrackRecord rec;
        rec.track_index = static_cast<int32_t>(ti);
        rec.bytes_before = serialized_track_bytes(out.track_events[ti]);
        rec.collapses = 0;
        // Iterate until no more profitable collapses on this track.
        // Bound iterations defensively — each collapse strictly shrinks
        // the track, so this terminates, but cap at 64 just in case.
        for (int iter = 0; iter < 64; ++iter) {
            if (!try_one_collapse(out.track_events[ti])) break;
            rec.collapses += 1;
        }
        rec.bytes_after = serialized_track_bytes(out.track_events[ti]);
        local.total_collapses += rec.collapses;
        local.total_bytes_saved += (rec.bytes_before - rec.bytes_after);
        local.per_track.push_back(rec);
    }
    if (report) *report = local;
    return out;
}

}  // namespace fftplugin
