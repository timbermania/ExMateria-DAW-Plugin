#include "fft_plugin/processor/track_edit_service.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/processor/authoring_bridge.h"
#include "fft_plugin/processor/track_edit_helpers.h"

namespace fftplugin {

FFTTrackEditService::FFTTrackEditService(
    FFTPluginState& state,
    FFTAuthoringBridge& bridge,
    FinalizeCallback finalize)
    : state_(&state), bridge_(&bridge), finalize_(std::move(finalize)) {}

bool FFTTrackEditService::set_param(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    int32_t value,
    int32_t min_value,
    int32_t max_value,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (value < min_value || value > max_value) {
        if (error_message != nullptr) {
            *error_message = "Opcode parameter value is out of range";
        }
        return false;
    }

    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = resolve_authored_opcode_index(
        *state_->smd_authoring, compiled, track_idx, source_event_index);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (!authored_index.has_value() || opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }

    auto& authored_opcode = (*opcodes)[*authored_index];
    if (authored_opcode.opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }

    if (authored_opcode.opcode.params.empty()) {
        authored_opcode.opcode.params.resize(1U, 0);
    }
    authored_opcode.opcode.params[0] = value;
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::set_param_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    const std::vector<int32_t>& values,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }

    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = resolve_authored_opcode_index(
        *state_->smd_authoring, compiled, track_idx, source_event_index);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (!authored_index.has_value() || opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }

    auto& authored_opcode = (*opcodes)[*authored_index];
    if (authored_opcode.opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }

    authored_opcode.opcode.params = values;
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

int32_t FFTTrackEditService::track_transposition(int32_t track_idx) const {
    if (!state_->smd_authoring.has_value()) {
        return 0;
    }
    const auto* part = authored_part_ptr(&*state_->smd_authoring, track_idx);
    if (part == nullptr) {
        return 0;
    }
    return part->kind == FFTSmdAuthoringPartKind::poly_track
        ? part->poly_track.track_transposition
        : part->raw_track.track_transposition;
}

bool FFTTrackEditService::set_track_transposition(
    int32_t track_idx, int32_t semitones, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    const int32_t clamped = std::clamp(semitones, -36, 36);
    auto* part = authored_part_ptr(&*state_->smd_authoring, track_idx);
    if (part == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Invalid track index";
        }
        return false;
    }
    if (part->kind == FFTSmdAuthoringPartKind::poly_track) {
        part->poly_track.track_transposition = clamped;
    } else {
        part->raw_track.track_transposition = clamped;
    }
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::set_opcode_code(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    int32_t new_opcode,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = resolve_authored_opcode_index(
        *state_->smd_authoring, compiled, track_idx, source_event_index);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (!authored_index.has_value() || opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    auto& authored_opcode = (*opcodes)[*authored_index];
    if (authored_opcode.opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }
    authored_opcode.opcode.opcode = new_opcode;
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::delete_opcode_by_source_event(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = resolve_authored_opcode_index(
        *state_->smd_authoring, compiled, track_idx, source_event_index);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (!authored_index.has_value() || opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    if ((*opcodes)[*authored_index].opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }
    opcodes->erase(opcodes->begin() + static_cast<std::ptrdiff_t>(*authored_index));
    renumber_authored_opcode_stack_order(*opcodes);
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::delete_opcode_by_authored_index(
    int32_t track_idx,
    int32_t authored_opcode_index,
    int32_t expected_opcode,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    if (authored_opcode_index < 0 || static_cast<size_t>(authored_opcode_index) >= opcodes->size()) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    if ((*opcodes)[static_cast<size_t>(authored_opcode_index)].opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }
    opcodes->erase(opcodes->begin() + static_cast<std::ptrdiff_t>(authored_opcode_index));
    renumber_authored_opcode_stack_order(*opcodes);
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::insert_opcode(
    int32_t track_idx,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    int32_t opcode,
    const std::vector<int32_t>& params,
    int32_t* inserted_source_event_index,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Invalid authored track for opcode insert";
        }
        return false;
    }

    const size_t authored_index = opcodes->size();
    opcodes->push_back(FFTSmdAuthoredOpcode {
        .tick = std::max(0, target_tick),
        .stack_order = insertion_sequence_index,
        .enabled = true,
        .opcode = FFTSmdOpcodeEvent {
            .opcode = opcode,
            .params = params,
        },
    });
    const size_t reordered_authored_index = reorder_authored_opcode_by_compiled_slot(
        *opcodes,
        compile_smd_authoring_document(*state_->smd_authoring),
        track_idx,
        authored_index,
        insertion_sequence_index);

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    if (inserted_source_event_index != nullptr) {
        if (authored_part_is_poly_track(&*state_->smd_authoring, track_idx)) {
            *inserted_source_event_index = static_cast<int32_t>(reordered_authored_index);
        } else if (static_cast<size_t>(track_idx) < compiled.authored_opcode_source_indices.size() &&
                   reordered_authored_index < compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)].size()) {
            *inserted_source_event_index =
                compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)][reordered_authored_index];
        }
    }
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::move_opcode_by_source_event(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    int32_t* moved_source_event_index,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = resolve_authored_opcode_index(
        *state_->smd_authoring, compiled, track_idx, source_event_index);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (!authored_index.has_value() || opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    auto& authored_opcode = (*opcodes)[*authored_index];
    if (authored_opcode.opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }
    authored_opcode.tick = std::max(0, target_tick);
    const size_t reordered_authored_index = reorder_authored_opcode_by_compiled_slot(
        *opcodes, compiled, track_idx, *authored_index, insertion_sequence_index);
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    if (moved_source_event_index != nullptr &&
        track_idx >= 0 &&
        static_cast<size_t>(track_idx) < compiled.authored_opcode_source_indices.size() &&
        reordered_authored_index < compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)].size()) {
        *moved_source_event_index =
            compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)][reordered_authored_index];
    }
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::move_opcode_by_authored_index(
    int32_t track_idx,
    int32_t authored_opcode_index,
    int32_t expected_opcode,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    int32_t* moved_authored_opcode_index,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    if (authored_opcode_index < 0 || static_cast<size_t>(authored_opcode_index) >= opcodes->size()) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    auto& authored_opcode = (*opcodes)[static_cast<size_t>(authored_opcode_index)];
    if (authored_opcode.opcode.opcode != expected_opcode) {
        if (error_message != nullptr) {
            *error_message = "Selected opcode no longer matches authored state";
        }
        return false;
    }
    authored_opcode.tick = std::max(0, target_tick);
    const size_t reordered_authored_index = reorder_authored_opcode_by_compiled_slot(
        *opcodes, compiled, track_idx, static_cast<size_t>(authored_opcode_index), insertion_sequence_index);
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    if (moved_authored_opcode_index != nullptr) {
        *moved_authored_opcode_index = static_cast<int32_t>(reordered_authored_index);
    }
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::move_opcodes_by_source_events(
    int32_t track_idx,
    const std::vector<int32_t>& source_event_indices,
    int32_t dragged_source_event_index,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    std::vector<int32_t>* moved_source_event_indices,
    int32_t* moved_dragged_source_event_index,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (track_idx < 0 || static_cast<size_t>(track_idx) >= state_->smd_authoring->tracks.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid track index";
        }
        return false;
    }

    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    auto& authored_track = state_->smd_authoring->tracks[static_cast<size_t>(track_idx)];
    const auto& compiled_source_indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];

    std::vector<size_t> selected_authored_indices;
    selected_authored_indices.reserve(source_event_indices.size());
    for (const int32_t source_event_index : source_event_indices) {
        const auto authored_index = authored_opcode_index_for_source_event(compiled, track_idx, source_event_index);
        if (!authored_index.has_value()) {
            continue;
        }
        if (std::find(selected_authored_indices.begin(), selected_authored_indices.end(), *authored_index) ==
            selected_authored_indices.end()) {
            selected_authored_indices.push_back(*authored_index);
        }
    }

    const auto dragged_authored_index = authored_opcode_index_for_source_event(
        compiled, track_idx, dragged_source_event_index);
    if (!dragged_authored_index.has_value()) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve dragged authored opcode";
        }
        return false;
    }
    if (std::find(selected_authored_indices.begin(), selected_authored_indices.end(), *dragged_authored_index) ==
        selected_authored_indices.end()) {
        selected_authored_indices.push_back(*dragged_authored_index);
    }

    std::sort(
        selected_authored_indices.begin(),
        selected_authored_indices.end(),
        [&compiled_source_indices](size_t lhs, size_t rhs) {
            return compiled_source_indices[lhs] < compiled_source_indices[rhs];
        });

    std::vector<FFTSmdAuthoredOpcode> moved_block;
    moved_block.reserve(selected_authored_indices.size());
    size_t dragged_block_offset = 0;
    int32_t min_selected_tick = std::numeric_limits<int32_t>::max();
    int32_t dragged_tick = 0;
    for (size_t block_index = 0; block_index < selected_authored_indices.size(); ++block_index) {
        const size_t authored_index = selected_authored_indices[block_index];
        if (authored_index >= authored_track.opcodes.size()) {
            if (error_message != nullptr) {
                *error_message = "Selected opcode no longer matches authored state";
            }
            return false;
        }
        const auto& authored_opcode = authored_track.opcodes[authored_index];
        min_selected_tick = std::min(min_selected_tick, authored_opcode.tick);
        if (authored_index == *dragged_authored_index) {
            dragged_block_offset = block_index;
            dragged_tick = authored_opcode.tick;
        }
        moved_block.push_back(authored_opcode);
    }

    int32_t tick_delta = std::max(0, target_tick) - dragged_tick;
    if (min_selected_tick + tick_delta < 0) {
        tick_delta = -min_selected_tick;
    }
    for (auto& authored_opcode : moved_block) {
        authored_opcode.tick = std::max(0, authored_opcode.tick + tick_delta);
    }

    std::vector<size_t> descending_indices = selected_authored_indices;
    std::sort(descending_indices.begin(), descending_indices.end(), std::greater<size_t>());
    for (const size_t authored_index : descending_indices) {
        authored_track.opcodes.erase(authored_track.opcodes.begin() + static_cast<std::ptrdiff_t>(authored_index));
    }
    renumber_authored_opcode_stack_order(authored_track);

    FFTSmdCompiledDocument destination_compiled = compile_smd_authoring_document(*state_->smd_authoring);
    std::vector<size_t> destination_compiled_order;
    destination_compiled_order.reserve(authored_track.opcodes.size());
    for (size_t authored_index = 0; authored_index < authored_track.opcodes.size(); ++authored_index) {
        destination_compiled_order.push_back(authored_index);
    }
    if (track_idx >= 0 &&
        static_cast<size_t>(track_idx) < destination_compiled.authored_opcode_source_indices.size()) {
        const auto& destination_source_indices =
            destination_compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
        std::stable_sort(
            destination_compiled_order.begin(),
            destination_compiled_order.end(),
            [&destination_source_indices](size_t lhs, size_t rhs) {
                return destination_source_indices[lhs] < destination_source_indices[rhs];
            });
    }

    size_t insertion_pos = destination_compiled_order.size();
    if (insertion_sequence_index >= 0 &&
        track_idx >= 0 &&
        static_cast<size_t>(track_idx) < destination_compiled.authored_opcode_source_indices.size()) {
        const auto& destination_source_indices =
            destination_compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
        for (size_t pos = 0; pos < destination_compiled_order.size(); ++pos) {
            if (destination_source_indices[destination_compiled_order[pos]] >= insertion_sequence_index) {
                insertion_pos = pos;
                break;
            }
        }
    }

    std::vector<FFTSmdAuthoredOpcode> reordered;
    reordered.reserve(authored_track.opcodes.size() + moved_block.size());
    for (size_t pos = 0; pos < destination_compiled_order.size(); ++pos) {
        if (pos == insertion_pos) {
            reordered.insert(reordered.end(), moved_block.begin(), moved_block.end());
        }
        reordered.push_back(authored_track.opcodes[destination_compiled_order[pos]]);
    }
    if (insertion_pos >= destination_compiled_order.size()) {
        reordered.insert(reordered.end(), moved_block.begin(), moved_block.end());
    }
    authored_track.opcodes = std::move(reordered);
    renumber_authored_opcode_stack_order(authored_track);

    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    if (moved_source_event_indices != nullptr) {
        moved_source_event_indices->clear();
        if (track_idx >= 0 &&
            static_cast<size_t>(track_idx) < compiled.authored_opcode_source_indices.size()) {
            const auto& moved_source_indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
            for (size_t offset = 0; offset < moved_block.size(); ++offset) {
                const size_t authored_index = insertion_pos + offset;
                if (authored_index < moved_source_indices.size()) {
                    moved_source_event_indices->push_back(moved_source_indices[authored_index]);
                }
            }
        }
    }
    if (moved_dragged_source_event_index != nullptr) {
        *moved_dragged_source_event_index = -1;
        if (track_idx >= 0 &&
            static_cast<size_t>(track_idx) < compiled.authored_opcode_source_indices.size()) {
            const auto& moved_source_indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
            const size_t authored_index = insertion_pos + dragged_block_offset;
            if (authored_index < moved_source_indices.size()) {
                *moved_dragged_source_event_index = moved_source_indices[authored_index];
            }
        }
    }
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::move_opcodes_by_authored_indices(
    int32_t track_idx,
    const std::vector<int32_t>& authored_opcode_indices,
    int32_t dragged_authored_opcode_index,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    std::vector<int32_t>* moved_authored_opcode_indices,
    int32_t* moved_dragged_authored_opcode_index,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Invalid track index";
        }
        return false;
    }
    const auto& compiled_source_indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];

    std::vector<size_t> selected_authored_indices;
    selected_authored_indices.reserve(authored_opcode_indices.size());
    for (const int32_t authored_opcode_index : authored_opcode_indices) {
        if (authored_opcode_index < 0 ||
            static_cast<size_t>(authored_opcode_index) >= opcodes->size()) {
            continue;
        }
        const size_t authored_index = static_cast<size_t>(authored_opcode_index);
        if (std::find(selected_authored_indices.begin(), selected_authored_indices.end(), authored_index) ==
            selected_authored_indices.end()) {
            selected_authored_indices.push_back(authored_index);
        }
    }

    if (dragged_authored_opcode_index < 0 ||
        static_cast<size_t>(dragged_authored_opcode_index) >= opcodes->size()) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve dragged authored opcode";
        }
        return false;
    }

    const size_t dragged_authored_index = static_cast<size_t>(dragged_authored_opcode_index);
    if (std::find(selected_authored_indices.begin(), selected_authored_indices.end(), dragged_authored_index) ==
        selected_authored_indices.end()) {
        selected_authored_indices.push_back(dragged_authored_index);
    }

    std::sort(
        selected_authored_indices.begin(),
        selected_authored_indices.end(),
        [&compiled_source_indices](size_t lhs, size_t rhs) {
            return compiled_source_indices[lhs] < compiled_source_indices[rhs];
        });

    std::vector<FFTSmdAuthoredOpcode> moved_block;
    moved_block.reserve(selected_authored_indices.size());
    size_t dragged_block_offset = 0;
    int32_t min_selected_tick = std::numeric_limits<int32_t>::max();
    int32_t dragged_tick = 0;
    for (size_t block_index = 0; block_index < selected_authored_indices.size(); ++block_index) {
        const size_t authored_index = selected_authored_indices[block_index];
        if (authored_index >= opcodes->size()) {
            if (error_message != nullptr) {
                *error_message = "Selected opcode no longer matches authored state";
            }
            return false;
        }
        const auto& authored_opcode = (*opcodes)[authored_index];
        min_selected_tick = std::min(min_selected_tick, authored_opcode.tick);
        if (authored_index == dragged_authored_index) {
            dragged_block_offset = block_index;
            dragged_tick = authored_opcode.tick;
        }
        moved_block.push_back(authored_opcode);
    }

    int32_t tick_delta = std::max(0, target_tick) - dragged_tick;
    if (min_selected_tick + tick_delta < 0) {
        tick_delta = -min_selected_tick;
    }
    for (auto& authored_opcode : moved_block) {
        authored_opcode.tick = std::max(0, authored_opcode.tick + tick_delta);
    }

    std::vector<size_t> descending_indices = selected_authored_indices;
    std::sort(descending_indices.begin(), descending_indices.end(), std::greater<size_t>());
    for (const size_t authored_index : descending_indices) {
        opcodes->erase(opcodes->begin() + static_cast<std::ptrdiff_t>(authored_index));
    }
    renumber_authored_opcode_stack_order(*opcodes);

    FFTSmdCompiledDocument destination_compiled = compile_smd_authoring_document(*state_->smd_authoring);
    std::vector<size_t> destination_compiled_order;
    destination_compiled_order.reserve(opcodes->size());
    for (size_t authored_index = 0; authored_index < opcodes->size(); ++authored_index) {
        destination_compiled_order.push_back(authored_index);
    }
    if (track_idx >= 0 &&
        static_cast<size_t>(track_idx) < destination_compiled.authored_opcode_source_indices.size()) {
        const auto& destination_source_indices =
            destination_compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
        std::stable_sort(
            destination_compiled_order.begin(),
            destination_compiled_order.end(),
            [&destination_source_indices](size_t lhs, size_t rhs) {
                return destination_source_indices[lhs] < destination_source_indices[rhs];
            });
    }

    size_t insertion_pos = destination_compiled_order.size();
    if (insertion_sequence_index >= 0 &&
        track_idx >= 0 &&
        static_cast<size_t>(track_idx) < destination_compiled.authored_opcode_source_indices.size()) {
        const auto& destination_source_indices =
            destination_compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
        for (size_t pos = 0; pos < destination_compiled_order.size(); ++pos) {
            if (destination_source_indices[destination_compiled_order[pos]] >= insertion_sequence_index) {
                insertion_pos = pos;
                break;
            }
        }
    }

    std::vector<FFTSmdAuthoredOpcode> reordered;
    reordered.reserve(opcodes->size() + moved_block.size());
    for (size_t pos = 0; pos < destination_compiled_order.size(); ++pos) {
        if (pos == insertion_pos) {
            reordered.insert(reordered.end(), moved_block.begin(), moved_block.end());
        }
        reordered.push_back((*opcodes)[destination_compiled_order[pos]]);
    }
    if (insertion_pos >= destination_compiled_order.size()) {
        reordered.insert(reordered.end(), moved_block.begin(), moved_block.end());
    }
    *opcodes = std::move(reordered);
    renumber_authored_opcode_stack_order(*opcodes);

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    if (moved_authored_opcode_indices != nullptr) {
        moved_authored_opcode_indices->clear();
        for (size_t offset = 0; offset < moved_block.size(); ++offset) {
            moved_authored_opcode_indices->push_back(static_cast<int32_t>(insertion_pos + offset));
        }
    }
    if (moved_dragged_authored_opcode_index != nullptr) {
        *moved_dragged_authored_opcode_index = static_cast<int32_t>(insertion_pos + dragged_block_offset);
    }
    return finalize_(compiled, error_message);
}

bool FFTTrackEditService::set_source_opcode_disabled(
    int32_t track_idx,
    int32_t source_event_index,
    bool disabled,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = resolve_authored_opcode_index(
        *state_->smd_authoring, compiled, track_idx, source_event_index);
    auto* opcodes = authored_part_opcodes(&*state_->smd_authoring, track_idx);
    if (!authored_index.has_value() || opcodes == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Failed to resolve authored opcode";
        }
        return false;
    }
    (*opcodes)[*authored_index].enabled = !disabled;
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

}  // namespace fftplugin
