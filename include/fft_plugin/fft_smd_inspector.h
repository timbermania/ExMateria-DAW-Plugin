#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_file.h"

namespace fftplugin {

enum class FFTSmdPresentationMode {
    source,
    playback,
};

struct FFTSmdTrackSummary {
    int32_t track_index = 0;
    int32_t event_count = 0;
    int32_t note_count = 0;
    int32_t opcode_count = 0;
};

struct FFTSmdLaneNoteBlock {
    int32_t start_tick = 0;
    int32_t authored_start_tick = -1;
    int32_t authored_span_index = -1;
    int32_t loop_root_id = -1;
    int32_t loop_instance_id = -1;
    int32_t duration_ticks = 0;
    int32_t relative_key = 0;
    bool has_fermata = false;
    int32_t fermata_extension_ticks = 0;
    int32_t loop_depth = 0;
    int32_t source_event_index = -1;
};

enum class FFTSmdLaneCommandKind {
    note,
    rest,
    hold,
    opcode,
    structure,
    tempo,
};

struct FFTSmdLaneCommandBlock {
    int32_t tick = 0;
    int32_t authored_tick = -1;
    int32_t authored_opcode_index = -1;
    int32_t loop_root_id = -1;
    int32_t loop_instance_id = -1;
    int32_t duration_ticks = 0;
    std::vector<int32_t> opcode_params;
    int32_t sequence_index = 0;
    int32_t loop_depth = 0;
    int32_t source_event_index = -1;
    int32_t opcode = -1;
    FFTSmdLaneCommandKind kind = FFTSmdLaneCommandKind::opcode;
    std::string label;
    bool enabled = true;
};

enum class FFTSmdLaneInsertAnchorKind {
    track_start,
    note_start,
    rest_start,
    measure,
    command,
};

struct FFTSmdLaneInsertAnchor {
    int32_t tick = 0;
    int32_t insertion_sequence_index = 0;
    int32_t authored_opcode_index = -1;
    int32_t authored_span_index = -1;
    int32_t source_event_index = -1;
    int32_t opcode = -1;
    FFTSmdLaneInsertAnchorKind kind = FFTSmdLaneInsertAnchorKind::measure;
    std::string label;
};

enum class FFTSmdLaneMarkerKind {
    opcode,
    structure,
    tempo,
};

enum class FFTSmdLaneDiagnosticSeverity {
    warning,
    error,
};

struct FFTSmdLaneDiagnostic {
    int32_t tick = 0;
    FFTSmdLaneDiagnosticSeverity severity = FFTSmdLaneDiagnosticSeverity::error;
    std::string short_label;
    std::string message;
};

struct FFTSmdLaneMarker {
    int32_t tick = 0;
    int32_t authored_tick = -1;
    FFTSmdLaneMarkerKind kind = FFTSmdLaneMarkerKind::opcode;
    int32_t loop_depth = 0;
    std::string label;
};

struct FFTSmdLaneLoopBoundary {
    int32_t tick = 0;
    int32_t authored_tick = -1;
    int32_t loop_depth = 0;
    std::string label;
};

struct FFTSmdLaneTimeMapSegment {
    int32_t start_tick = 0;
    int32_t authored_start_tick = -1;
    int32_t duration_ticks = 0;
    int32_t root_group_id = -1;
    int32_t loop_instance_id = -1;
    int32_t occurrence_index = -1;
    int32_t occurrence_count = 0;
    bool repeated = false;
};

struct FFTSmdGridSegment {
    int32_t start_tick = 0;
    int32_t end_tick = 0;
    int32_t numerator = 4;
    int32_t denominator = 4;
    int32_t ticks_per_beat = 48;
    int32_t ticks_per_bar = 192;
};

struct FFTSmdSecondMarker {
    int32_t tick = 0;
    std::string label;
};

struct FFTSmdTrackLanePresentation {
    int32_t track_index = 0;
    int32_t total_ticks = 0;
    bool polyphonic_notes = false;
    bool muted = false;
    bool soloed = false;
    FFTSmdTrackSummary summary;
    std::vector<FFTSmdLaneNoteBlock> notes;
    std::vector<FFTSmdLaneCommandBlock> commands;
    std::vector<FFTSmdLaneInsertAnchor> insert_anchors;
    std::vector<FFTSmdLaneMarker> markers;
    std::vector<FFTSmdLaneLoopBoundary> loop_boundaries;
    std::vector<FFTSmdLaneTimeMapSegment> time_map_segments;
    std::vector<FFTSmdLaneDiagnostic> diagnostics;
};

struct FFTSmdSongPresentation {
    int32_t total_ticks = 0;
    std::vector<FFTSmdLaneMarker> conductor_markers;
    std::vector<FFTSmdGridSegment> grid_segments;
    std::vector<FFTSmdSecondMarker> second_markers;
    std::vector<FFTSmdTrackLanePresentation> tracks;
};

std::vector<FFTSmdTrackSummary> build_smd_track_summaries(const FFTSmdFile& smd);
std::string build_smd_metadata_text(const FFTSmdFile& smd);
std::string build_smd_track_summary_text(const FFTSmdFile& smd);
std::string build_smd_track_events_text(const FFTSmdFile& smd, int32_t track_index);
FFTSmdSongPresentation build_smd_song_presentation(
    const FFTSmdFile& smd,
    FFTSmdPresentationMode mode = FFTSmdPresentationMode::source,
    std::function<bool(int32_t, int32_t)> is_source_event_disabled = {}
);

}  // namespace fftplugin
