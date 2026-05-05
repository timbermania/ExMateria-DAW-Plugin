#include "fft_plugin/processor/track_grouping_service.h"

#include <algorithm>
#include <utility>

#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/processor/authoring_bridge.h"
#include "fft_plugin/processor/track_edit_helpers.h"
#include "fft_plugin/processor/track_grouping_helpers.h"

namespace fftplugin {

FFTTrackGroupingService::FFTTrackGroupingService(
    FFTPluginState& state,
    FFTAuthoringBridge& bridge,
    FinalizeCallback finalize
) : state_(&state), bridge_(&bridge), finalize_(std::move(finalize)) {}

bool FFTTrackGroupingService::convert_to_poly(
    const std::vector<int32_t>& track_indices,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (track_indices.empty()) {
        if (error_message != nullptr) {
            *error_message = "No tracks selected for grouping";
        }
        return false;
    }

    std::vector<int32_t> unique_indices = track_indices;
    std::sort(unique_indices.begin(), unique_indices.end());
    unique_indices.erase(std::unique(unique_indices.begin(), unique_indices.end()), unique_indices.end());

    std::vector<FFTSmdAuthoringPart> selected_parts;
    selected_parts.reserve(unique_indices.size());
    for (const int32_t track_idx : unique_indices) {
        const auto* part = authored_part_ptr(&*state_->smd_authoring, track_idx);
        if (part == nullptr || part->kind != FFTSmdAuthoringPartKind::raw_track) {
            if (error_message != nullptr) {
                *error_message = "Only raw source tracks can be converted to PolyTrack";
            }
            return false;
        }
        selected_parts.push_back(*part);
    }

    for (size_t part_index = 1; part_index < selected_parts.size(); ++part_index) {
        if (!authored_structure_timelines_match(
                selected_parts.front().raw_track,
                selected_parts[part_index].raw_track)) {
            if (error_message != nullptr) {
                *error_message = "Selected tracks do not share a compatible structural timeline";
            }
            return false;
        }
    }

    FFTSmdAuthoringPart poly_part;
    poly_part.kind = FFTSmdAuthoringPartKind::poly_track;
    poly_part.name = unique_indices.size() == 1
        ? ("Poly " + selected_parts.front().name)
        : ("Poly " + selected_parts.front().name + " +" + std::to_string(unique_indices.size() - 1));
    poly_part.poly_track = poly_track_from_raw_parts(selected_parts);

    auto& parts = state_->smd_authoring->parts;
    const size_t insertion_index = static_cast<size_t>(unique_indices.front());
    for (auto it = unique_indices.rbegin(); it != unique_indices.rend(); ++it) {
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    parts.insert(parts.begin() + static_cast<std::ptrdiff_t>(insertion_index), std::move(poly_part));
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    state_->editor_view.selected_track_id = static_cast<int32_t>(insertion_index);

    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackGroupingService::ungroup(int32_t track_idx, std::string* error_message) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }

    const auto* part = authored_part_ptr(&*state_->smd_authoring, track_idx);
    if (part == nullptr || part->kind != FFTSmdAuthoringPartKind::poly_track) {
        if (error_message != nullptr) {
            *error_message = "Selected part is not a PolyTrack";
        }
        return false;
    }

    FFTSmdAuthoringDocument temporary;
    temporary.initial_tempo = state_->smd_authoring->initial_tempo;
    temporary.initial_volume = state_->smd_authoring->initial_volume;
    temporary.assoc_wds_id = state_->smd_authoring->assoc_wds_id;
    temporary.song_title = state_->smd_authoring->song_title;
    temporary.parts.push_back(*part);
    const FFTSmdCompiledDocument compiled_poly = compile_smd_authoring_document(temporary);
    FFTSmdAuthoringDocument ungrouped = import_smd_authoring_document(
        compiled_poly.smd,
        [&compiled_poly](int32_t compiled_track_idx, int32_t source_event_index) {
            return compiled_poly.disabled_opcode_keys.contains(
                smd_track_event_key(compiled_track_idx, source_event_index));
        });

    auto& parts = state_->smd_authoring->parts;
    parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(track_idx));
    parts.insert(
        parts.begin() + static_cast<std::ptrdiff_t>(track_idx),
        ungrouped.parts.begin(),
        ungrouped.parts.end());
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    state_->editor_view.selected_track_id = std::max(0, track_idx);

    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackGroupingService::is_poly(int32_t track_idx) const {
    const auto* doc = state_->smd_authoring.has_value() ? &*state_->smd_authoring : nullptr;
    const auto* part = authored_part_ptr(doc, track_idx);
    return part != nullptr && part->kind == FFTSmdAuthoringPartKind::poly_track;
}

}  // namespace fftplugin
