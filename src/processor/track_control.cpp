#include "fft_plugin/processor/track_control.h"

#include <algorithm>
#include <vector>

#include "fft_plugin/fft_file_playback_engine.h"
#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

namespace {

void set_track_id_enabled(std::vector<int32_t>& track_ids, int32_t track_idx, bool enabled) {
    track_ids.erase(
        std::remove(track_ids.begin(), track_ids.end(), track_idx),
        track_ids.end());
    if (enabled) {
        track_ids.push_back(track_idx);
        std::sort(track_ids.begin(), track_ids.end());
    }
}

bool contains_track_id(const std::vector<int32_t>& track_ids, int32_t track_idx) {
    return std::find(track_ids.begin(), track_ids.end(), track_idx) != track_ids.end();
}

// Map an editor-view track index to the engine-side compiled track indices
// it covers. PolyTracks expand to multiple compiled lanes; raw tracks are
// 1:1. Falls back to the identity mapping when no authored document is
// loaded (legacy raw-only state).
std::vector<int32_t> resolve_engine_track_indices_for_view_track(
    const FFTSmdAuthoringDocument* document,
    int32_t track_idx
) {
    if (track_idx < 0) {
        return {};
    }
    if (document == nullptr || document->parts.empty()) {
        return {track_idx};
    }
    if (static_cast<size_t>(track_idx) >= document->parts.size()) {
        return {track_idx};
    }

    const FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*document);
    if (static_cast<size_t>(track_idx) >= compiled.authored_part_compiled_track_indices.size()) {
        return {track_idx};
    }

    const auto& compiled_track_indices = compiled.authored_part_compiled_track_indices[static_cast<size_t>(track_idx)];
    if (!compiled_track_indices.empty()) {
        return compiled_track_indices;
    }
    return {track_idx};
}

}  // namespace

FFTTrackControlService::FFTTrackControlService(FFTPluginState& state, FFTFilePlaybackEngine& engine)
    : state_(&state), engine_(&engine) {}

void FFTTrackControlService::set_track_muted(int32_t track_idx, bool muted) {
    set_track_id_enabled(state_->editor_view.muted_track_ids, track_idx, muted);
    const auto engine_track_indices = resolve_engine_track_indices_for_view_track(
        state_->smd_authoring.has_value() ? &*state_->smd_authoring : nullptr,
        track_idx);
    for (const int32_t engine_track_idx : engine_track_indices) {
        engine_->set_track_muted(engine_track_idx, muted);
    }
}

void FFTTrackControlService::set_track_soloed(int32_t track_idx, bool soloed) {
    set_track_id_enabled(state_->editor_view.solo_track_ids, track_idx, soloed);
    const auto engine_track_indices = resolve_engine_track_indices_for_view_track(
        state_->smd_authoring.has_value() ? &*state_->smd_authoring : nullptr,
        track_idx);
    for (const int32_t engine_track_idx : engine_track_indices) {
        engine_->set_track_soloed(engine_track_idx, soloed);
    }
}

bool FFTTrackControlService::track_muted(int32_t track_idx) const {
    return contains_track_id(state_->editor_view.muted_track_ids, track_idx);
}

bool FFTTrackControlService::track_soloed(int32_t track_idx) const {
    return contains_track_id(state_->editor_view.solo_track_ids, track_idx);
}

int32_t FFTTrackControlService::selected_track_id() const {
    return state_->editor_view.selected_track_id;
}

void FFTTrackControlService::set_selected_track_id(int32_t track_idx) {
    state_->editor_view.selected_track_id = std::max(0, track_idx);
}

void FFTTrackControlService::apply_to_engine() {
    const FFTSmdAuthoringDocument* doc =
        state_->smd_authoring.has_value() ? &*state_->smd_authoring : nullptr;
    for (const int32_t track_idx : state_->editor_view.muted_track_ids) {
        for (const int32_t engine_track_idx :
             resolve_engine_track_indices_for_view_track(doc, track_idx)) {
            engine_->set_track_muted(engine_track_idx, true);
        }
    }
    for (const int32_t track_idx : state_->editor_view.solo_track_ids) {
        for (const int32_t engine_track_idx :
             resolve_engine_track_indices_for_view_track(doc, track_idx)) {
            engine_->set_track_soloed(engine_track_idx, true);
        }
    }
}

}  // namespace fftplugin
