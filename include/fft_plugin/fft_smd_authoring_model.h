#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_smd_global_lane_assigner.h"
#include "fft_plugin/fft_smd_lane_packer.h"
#include "fft_plugin/fft_smd_loop_roller.h"

namespace fftplugin {

struct FFTSmdAuthoredSpan {
    int32_t start_tick = 0;
    int32_t total_ticks = 0;
    int32_t base_ticks = 0;
    int32_t velocity_hint = 100;
    int32_t relative_key = 13;
};

struct FFTSmdAuthoredOpcode {
    int32_t tick = 0;
    int32_t stack_order = 0;
    bool enabled = true;
    bool exact_timing = false;
    FFTSmdOpcodeEvent opcode;
};

struct FFTSmdAuthoredTrack {
    std::vector<FFTSmdAuthoredSpan> spans;
    std::vector<FFTSmdAuthoredOpcode> opcodes;
    int32_t total_ticks = 0;
    int32_t track_transposition = 0;
};

struct FFTSmdAuthoredPolyNote {
    int32_t start_tick = 0;
    int32_t total_ticks = 0;
    int32_t base_ticks = 0;
    int32_t velocity_hint = 100;
    int32_t relative_key = 0;
};

struct FFTSmdAuthoredPolyTrack {
    std::vector<FFTSmdAuthoredPolyNote> notes;
    std::vector<FFTSmdAuthoredOpcode> opcodes;
    int32_t total_ticks = 0;
    int32_t track_transposition = 0;
};

enum class FFTSmdAuthoringPartKind : int32_t {
    raw_track = 0,
    poly_track = 1,
};

struct FFTSmdAuthoringPart {
    FFTSmdAuthoringPartKind kind = FFTSmdAuthoringPartKind::raw_track;
    std::string name;
    FFTSmdAuthoredTrack raw_track;
    FFTSmdAuthoredPolyTrack poly_track;
};

struct FFTSmdAuthoringDocument {
    int32_t format_version = 4;
    int32_t track_count = 0;
    int32_t initial_tempo = 0;
    int32_t initial_volume = 0;
    int32_t assoc_wds_id = 0;
    std::string song_title;
    std::vector<FFTSmdAuthoringPart> parts;
    // Legacy raw-track mirror used by the existing monophonic edit paths while
    // PolyTrack support is brought up. Authoring truth lives in `parts`.
    std::vector<FFTSmdAuthoredTrack> tracks;

    bool empty() const {
        return parts.empty() && tracks.empty();
    }
};

struct FFTSmdCompiledDocument {
    FFTSmdFile smd;
    std::unordered_set<uint64_t> disabled_opcode_keys;
    std::vector<std::vector<int32_t>> authored_span_source_indices;
    std::vector<std::vector<int32_t>> authored_opcode_source_indices;
    std::vector<std::vector<uint64_t>> authored_span_source_keys;
    std::vector<std::vector<uint64_t>> authored_opcode_source_keys;
    std::vector<std::vector<int32_t>> authored_part_compiled_track_indices;
};

uint64_t smd_track_event_key(int32_t track_idx, int32_t source_event_index);

FFTSmdAuthoringDocument import_smd_authoring_document(
    const FFTSmdFile& smd,
    const std::function<bool(int32_t, int32_t)>& is_opcode_disabled = {});

FFTSmdCompiledDocument compile_smd_authoring_document(const FFTSmdAuthoringDocument& document);

// Empirically-confirmed FFT engine cap on SMD load size.
// Found 2026-05-03 via patcher binary search: a 10-sector / 20480-byte SMD
// (matching MUSIC_99's vanilla padded allocation) loads and plays cleanly
// from any music slot via the Shishi-style relocate path. A 13-sector /
// 26624-byte SMD silences the data screen. The engine likely has a fixed
// destination buffer in main RAM that's at least 10 sectors and less than
// 13. Treat 10 sectors as the firm ceiling; experiments to find the exact
// edge can override this constant locally.
constexpr size_t kEngineMaxSmdBytes = 20480;

struct FFTSmdGameCompileBudget {
    int32_t max_tracks = 24;                  // PSX SPU voice count
    size_t max_bytes = 65536;                 // SMD format hard wall (uint16 offsets)
    size_t shipped_max_bytes = 18992;         // largest stock music SMD (MUSIC_99)
    size_t engine_max_bytes = kEngineMaxSmdBytes;  // hard cap enforced by compile path
    size_t target_bytes = 0;                  // 0 = compile up to engine_max_bytes;
                                              // >0 = trim song to fit (clamped to engine_max_bytes)
};

struct FFTSmdGameCompileReport {
    struct PartReport {
        std::string part_name;
        int32_t original_lanes = 0;
        int32_t final_lanes = 0;
        int32_t notes_dropped = 0;
        int32_t notes_trimmed_by_length = 0;
        int32_t earliest_dropped_tick = -1;
        int32_t latest_dropped_tick = -1;
        bool dropped_entirely = false;
    };

    int32_t pre_reduction_track_count = 0;
    int32_t final_track_count = 0;
    size_t encoded_bytes = 0;
    bool fits_track_budget = true;
    bool fits_byte_budget = true;
    int32_t total_notes_dropped = 0;
    int32_t total_notes_trimmed_by_length = 0;
    int32_t parts_dropped = 0;
    int32_t trim_tick_cap = -1;          // -1 = no length trim applied
    std::vector<PartReport> parts;
    std::string summary;
    std::string error;
    FFTSmdLanePackerReport pack_report;  // populated by Stage A-prime
    FFTSmdLoopRollerReport roll_report;  // populated by Stage D
    FFTSmdGlobalLaneAssignerReport global_assign_report;  // Stage 0-prime
};

struct FFTSmdGameCompileResult {
    FFTSmdCompiledDocument compiled;
    FFTSmdGameCompileReport report;
    bool ok = false;
};

FFTSmdGameCompileResult compile_smd_authoring_document_for_game(
    const FFTSmdAuthoringDocument& document,
    const FFTSmdGameCompileBudget& budget = {});

}  // namespace fftplugin
