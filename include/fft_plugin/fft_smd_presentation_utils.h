#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {

struct FFTVisibleAuthoredTickAnchor {
    int32_t visible_tick = 0;
    int32_t authored_tick = -1;
};

std::string smd_note_name_for_relative_key(int32_t relative_key);
int32_t smd_instrument_opcode_param_to_played_sample_id(int32_t instrument_param);
int32_t smd_played_sample_id_to_instrument_opcode_param(int32_t played_sample_id);
std::string smd_format_instrument_label_from_opcode_param(int32_t instrument_param);
bool smd_is_structure_opcode(int32_t opcode);
bool smd_is_time_only_opcode(int32_t opcode);
bool smd_is_tempo_opcode(int32_t opcode);
bool smd_is_time_signature_opcode(int32_t opcode);
std::string smd_short_opcode_label(const FFTSmdOpcodeEvent& opcode);
std::vector<FFTVisibleAuthoredTickAnchor> smd_build_visible_authored_tick_anchors(
    const FFTSmdTrackLanePresentation& track);
const FFTSmdLaneTimeMapSegment* smd_find_visible_time_map_segment(
    const FFTSmdTrackLanePresentation& track,
    int32_t visible_tick);
int32_t smd_map_visible_tick_to_authored_tick(
    const FFTSmdTrackLanePresentation& track,
    int32_t visible_tick);

FFTSmdLaneCommandBlock smd_build_note_command(
    int32_t tick,
    int32_t sequence_index,
    int32_t source_event_index,
    const FFTSmdNoteEvent& note);

FFTSmdLaneCommandBlock smd_build_opcode_command(
    int32_t tick,
    int32_t sequence_index,
    int32_t source_event_index,
    int32_t authored_opcode_index,
    const FFTSmdOpcodeEvent& opcode,
    bool enabled);

}  // namespace fftplugin
