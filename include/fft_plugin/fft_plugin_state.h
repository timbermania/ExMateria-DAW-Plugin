#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fft_plugin/fft_sequence_model.h"
#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

struct FFTEditorLaneState {
    OpcodeLaneType lane_type = OpcodeLaneType::instrument;
    bool visible = true;
};

struct FFTEditorViewState {
    double horizontal_zoom = 1.0;
    double vertical_zoom = 1.0;
    int32_t selected_track_id = 1;
    int16_t visible_note_low = 36;
    int16_t visible_note_high = 84;
    std::vector<int32_t> muted_track_ids;
    std::vector<int32_t> solo_track_ids;
    std::vector<FFTEditorLaneState> lanes;
};

struct FFTPluginState {
    int32_t state_version = 4;
    std::string waveset_path;
    std::string smd_path;
    std::string authoring_path;
    std::string selected_bank_name;
    FFTSequence sequence;
    std::optional<FFTSmdAuthoringDocument> smd_authoring;
    FFTEditorViewState editor_view;
};

}  // namespace fftplugin
