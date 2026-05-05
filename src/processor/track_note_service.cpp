#include "fft_plugin/processor/track_note_service.h"

#include <algorithm>
#include <utility>

#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/processor/authoring_bridge.h"
#include "fft_plugin/processor/track_edit_helpers.h"
#include "fft_plugin/processor/track_grouping_helpers.h"
#include "fft_plugin/processor/track_note_helpers.h"

namespace fftplugin {

FFTTrackNoteService::FFTTrackNoteService(
    FFTPluginState& state, FFTAuthoringBridge& bridge, FinalizeCallback finalize
) : state_(&state), bridge_(&bridge), finalize_(std::move(finalize)) {}

bool FFTTrackNoteService::insert_track_time(
    int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
    if (authored_track == nullptr) {
        if (error_message != nullptr) *error_message = "Invalid track index";
        return false;
    }
    if (duration_ticks <= 0) {
        if (error_message != nullptr) *error_message = "Invalid time selection";
        return false;
    }

    const int32_t insert_tick = std::clamp(start_tick, 0, authored_track->total_ticks);
    authored_track->spans = insert_time_into_authored_spans(*authored_track, insert_tick, duration_ticks);
    authored_track->total_ticks += duration_ticks;
    for (auto& opcode : authored_track->opcodes) {
        if (opcode.tick >= insert_tick) {
            opcode.tick += duration_ticks;
        }
    }
    renumber_authored_opcode_stack_order(*authored_track);

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::delete_track_time(
    int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
    if (authored_track == nullptr) {
        if (error_message != nullptr) *error_message = "Invalid track index";
        return false;
    }
    if (duration_ticks <= 0) {
        if (error_message != nullptr) *error_message = "Invalid time selection";
        return false;
    }
    if (authored_track->total_ticks <= 0) {
        if (error_message != nullptr) *error_message = "Track has no time to delete";
        return false;
    }

    const int32_t delete_start_tick = std::clamp(start_tick, 0, authored_track->total_ticks);
    const int32_t delete_end_tick = std::clamp(delete_start_tick + duration_ticks, delete_start_tick, authored_track->total_ticks);
    const int32_t effective_duration = delete_end_tick - delete_start_tick;
    if (effective_duration <= 0) {
        if (error_message != nullptr) *error_message = "Invalid time selection";
        return false;
    }

    authored_track->spans = delete_time_from_authored_spans(*authored_track, delete_start_tick, delete_end_tick);
    authored_track->total_ticks = std::max(0, authored_track->total_ticks - effective_duration);
    auto& opcodes = authored_track->opcodes;
    opcodes.erase(
        std::remove_if(
            opcodes.begin(),
            opcodes.end(),
            [delete_start_tick, delete_end_tick](const FFTSmdAuthoredOpcode& opcode) {
                return opcode.tick >= delete_start_tick && opcode.tick < delete_end_tick;
            }),
        opcodes.end());
    for (auto& opcode : opcodes) {
        if (opcode.tick >= delete_end_tick) {
            opcode.tick -= effective_duration;
        }
    }
    renumber_authored_opcode_stack_order(*authored_track);

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::replace_note_with_rest_by_source_event(
    int32_t track_idx, int32_t source_event_index, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = authored_span_index_for_source_event(compiled, track_idx, source_event_index);
    if (!authored_index.has_value()) {
        if (error_message != nullptr) *error_message = "Failed to resolve authored note span";
        return false;
    }
    return replace_note_with_rest_by_authored_index(track_idx, static_cast<int32_t>(*authored_index), error_message);
}

bool FFTTrackNoteService::replace_note_with_rest_by_authored_index(
    int32_t track_idx, int32_t authored_span_index, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (auto* poly_track = authored_poly_track_ptr(&*state_->smd_authoring, track_idx)) {
        if (authored_span_index < 0 || static_cast<size_t>(authored_span_index) >= poly_track->notes.size()) {
            if (error_message != nullptr) *error_message = "Invalid authored poly note";
            return false;
        }
        poly_track->notes.erase(poly_track->notes.begin() + static_cast<std::ptrdiff_t>(authored_span_index));
    } else {
        auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
        if (authored_track == nullptr) {
            if (error_message != nullptr) *error_message = "Invalid track index";
            return false;
        }
        if (authored_span_index < 0 || static_cast<size_t>(authored_span_index) >= authored_track->spans.size()) {
            if (error_message != nullptr) *error_message = "Invalid authored note span";
            return false;
        }
        const FFTSmdAuthoredSpan span = authored_track->spans[static_cast<size_t>(authored_span_index)];
        if (span.relative_key < 0 || span.relative_key >= 12) {
            if (error_message != nullptr) *error_message = "Selected span is not a note";
            return false;
        }
        authored_track->spans = rewrite_authored_spans(
            *authored_track,
            span.start_tick,
            span.start_tick + span.total_ticks,
            FFTSmdAuthoredSpan {
                .start_tick = span.start_tick,
                .total_ticks = span.total_ticks,
                .base_ticks = span.total_ticks,
                .velocity_hint = span.velocity_hint,
                .relative_key = 13,
            });
    }

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::replace_rest_with_note_by_source_event(
    int32_t track_idx, int32_t source_event_index,
    int32_t note_relative_key, int32_t start_offset_ticks, int32_t duration_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (note_relative_key < 0 || note_relative_key > 13 || duration_ticks <= 0 || start_offset_ticks < 0) {
        if (error_message != nullptr) *error_message = "Invalid note placement";
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = authored_span_index_for_source_event(compiled, track_idx, source_event_index);
    if (!authored_index.has_value()) {
        if (error_message != nullptr) *error_message = "Failed to resolve authored time span";
        return false;
    }
    return replace_rest_with_note_by_authored_index(
        track_idx, static_cast<int32_t>(*authored_index),
        note_relative_key, start_offset_ticks, duration_ticks, error_message);
}

bool FFTTrackNoteService::replace_rest_with_note_by_authored_index(
    int32_t track_idx, int32_t authored_span_index,
    int32_t note_relative_key, int32_t start_offset_ticks, int32_t duration_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (note_relative_key < 0 || note_relative_key > 13 || duration_ticks <= 0 || start_offset_ticks < 0) {
        if (error_message != nullptr) *error_message = "Invalid note placement";
        return false;
    }
    auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
    if (authored_track == nullptr) {
        if (error_message != nullptr) *error_message = "Invalid track index";
        return false;
    }
    if (authored_span_index < 0 || static_cast<size_t>(authored_span_index) >= authored_track->spans.size()) {
        if (error_message != nullptr) *error_message = "Invalid authored time span";
        return false;
    }

    const FFTSmdAuthoredSpan anchor = authored_track->spans[static_cast<size_t>(authored_span_index)];
    const int32_t start_tick = anchor.start_tick + start_offset_ticks;
    const int32_t end_tick = start_tick + duration_ticks;
    if (start_offset_ticks >= anchor.total_ticks || end_tick > authored_track->total_ticks) {
        if (error_message != nullptr) *error_message = "Note placement exceeds covered span";
        return false;
    }

    authored_track->spans = rewrite_authored_spans(
        *authored_track,
        start_tick,
        end_tick,
        FFTSmdAuthoredSpan {
            .start_tick = start_tick,
            .total_ticks = duration_ticks,
            .base_ticks = duration_ticks,
            .velocity_hint = anchor.velocity_hint,
            .relative_key = note_relative_key,
        });
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::insert_authored_poly_note(
    int32_t track_idx, int32_t note_relative_key,
    int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (note_relative_key < 0 || note_relative_key >= 12 || start_tick < 0 || duration_ticks <= 0) {
        if (error_message != nullptr) *error_message = "Invalid poly note placement";
        return false;
    }
    auto* poly_track = authored_poly_track_ptr(&*state_->smd_authoring, track_idx);
    if (poly_track == nullptr) {
        if (error_message != nullptr) *error_message = "Selected track is not a PolyTrack";
        return false;
    }
    if (start_tick + duration_ticks > poly_track->total_ticks) {
        if (error_message != nullptr) *error_message = "Poly note placement exceeds covered span";
        return false;
    }
    if (poly_track_has_same_key_overlap(*poly_track, note_relative_key, start_tick, duration_ticks)) {
        if (error_message != nullptr) *error_message = "PolyTrack cannot overlap two notes of the same key";
        return false;
    }

    int32_t velocity_hint = 96;
    if (!poly_track->notes.empty()) {
        velocity_hint = poly_track->notes.front().velocity_hint;
    }
    poly_track->notes.push_back(FFTSmdAuthoredPolyNote {
        .start_tick = start_tick,
        .total_ticks = duration_ticks,
        .base_ticks = duration_ticks,
        .velocity_hint = velocity_hint,
        .relative_key = note_relative_key,
    });

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::insert_authored_poly_rest(
    int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (start_tick < 0 || duration_ticks <= 0) {
        if (error_message != nullptr) *error_message = "Invalid poly rest placement";
        return false;
    }
    auto* poly_track = authored_poly_track_ptr(&*state_->smd_authoring, track_idx);
    if (poly_track == nullptr) {
        if (error_message != nullptr) *error_message = "Selected track is not a PolyTrack";
        return false;
    }
    const int32_t end_tick = start_tick + duration_ticks;
    if (end_tick > poly_track->total_ticks) {
        if (error_message != nullptr) *error_message = "Poly rest placement exceeds covered span";
        return false;
    }

    std::vector<FFTSmdAuthoredPolyNote> rewritten;
    rewritten.reserve(poly_track->notes.size() * 2U);
    for (const auto& note : poly_track->notes) {
        const int32_t note_start = note.start_tick;
        const int32_t note_end = note.start_tick + note.total_ticks;
        if (note_end <= start_tick || note_start >= end_tick) {
            rewritten.push_back(note);
            continue;
        }
        if (note_start < start_tick) {
            const auto left = make_poly_note_fragment_like(note, note_start, start_tick);
            if (left.total_ticks > 0) rewritten.push_back(left);
        }
        if (note_end > end_tick) {
            const auto right = make_poly_note_fragment_like(note, end_tick, note_end);
            if (right.total_ticks > 0) rewritten.push_back(right);
        }
    }
    poly_track->notes = std::move(rewritten);
    std::stable_sort(
        poly_track->notes.begin(),
        poly_track->notes.end(),
        [](const FFTSmdAuthoredPolyNote& lhs, const FFTSmdAuthoredPolyNote& rhs) {
            if (lhs.start_tick != rhs.start_tick) return lhs.start_tick < rhs.start_tick;
            if (lhs.relative_key != rhs.relative_key) return lhs.relative_key < rhs.relative_key;
            return lhs.total_ticks < rhs.total_ticks;
        });

    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::set_fermata_extension_by_source_event(
    int32_t track_idx, int32_t source_event_index, int32_t extension_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (extension_ticks < 0) {
        if (error_message != nullptr) *error_message = "Invalid fermata duration";
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = authored_span_index_for_source_event(compiled, track_idx, source_event_index);
    if (!authored_index.has_value()) {
        if (error_message != nullptr) *error_message = "Failed to resolve authored note span";
        return false;
    }
    return set_fermata_extension_by_authored_index(
        track_idx, static_cast<int32_t>(*authored_index), extension_ticks, error_message);
}

bool FFTTrackNoteService::set_fermata_extension_by_authored_index(
    int32_t track_idx, int32_t authored_span_index, int32_t extension_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (extension_ticks < 0) {
        if (error_message != nullptr) *error_message = "Invalid fermata duration";
        return false;
    }
    if (auto* poly_track = authored_poly_track_ptr(&*state_->smd_authoring, track_idx)) {
        if (authored_span_index < 0 || static_cast<size_t>(authored_span_index) >= poly_track->notes.size()) {
            if (error_message != nullptr) *error_message = "Invalid authored poly note";
            return false;
        }
        auto& note = poly_track->notes[static_cast<size_t>(authored_span_index)];
        const int32_t new_total_ticks = note.base_ticks + extension_ticks;
        if (note.start_tick + new_total_ticks > poly_track->total_ticks) {
            if (error_message != nullptr) *error_message = "Note resize exceeds covered span";
            return false;
        }
        note.total_ticks = new_total_ticks;
    } else {
        auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
        if (authored_track == nullptr) {
            if (error_message != nullptr) *error_message = "Invalid track index";
            return false;
        }
        if (authored_span_index < 0 || static_cast<size_t>(authored_span_index) >= authored_track->spans.size()) {
            if (error_message != nullptr) *error_message = "Invalid authored note span";
            return false;
        }
        const FFTSmdAuthoredSpan span = authored_track->spans[static_cast<size_t>(authored_span_index)];
        if (span.relative_key < 0 || span.relative_key >= 12) {
            if (error_message != nullptr) *error_message = "Selected span is not a note";
            return false;
        }
        const int32_t new_total_ticks = span.base_ticks + extension_ticks;
        if (span.start_tick + new_total_ticks > authored_track->total_ticks) {
            if (error_message != nullptr) *error_message = "Note resize exceeds covered span";
            return false;
        }
        authored_track->spans = rewrite_authored_spans(
            *authored_track,
            span.start_tick,
            std::max(span.start_tick + span.total_ticks, span.start_tick + new_total_ticks),
            FFTSmdAuthoredSpan {
                .start_tick = span.start_tick,
                .total_ticks = new_total_ticks,
                .base_ticks = span.base_ticks,
                .velocity_hint = span.velocity_hint,
                .relative_key = span.relative_key,
            });
    }
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::set_note_geometry_by_source_event(
    int32_t track_idx, int32_t source_event_index,
    int32_t start_tick, int32_t base_duration_ticks, int32_t extension_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (source_event_index < 0 || start_tick < 0 || base_duration_ticks <= 0 || extension_ticks < 0) {
        if (error_message != nullptr) *error_message = "Invalid note geometry";
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = authored_span_index_for_source_event(compiled, track_idx, source_event_index);
    if (!authored_index.has_value()) {
        if (error_message != nullptr) *error_message = "Failed to resolve authored note span";
        return false;
    }
    return set_note_geometry_by_authored_index(
        track_idx, static_cast<int32_t>(*authored_index),
        start_tick, base_duration_ticks, extension_ticks, error_message);
}

bool FFTTrackNoteService::set_note_geometry_by_authored_index(
    int32_t track_idx, int32_t authored_span_index,
    int32_t start_tick, int32_t base_duration_ticks, int32_t extension_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (authored_span_index < 0 || start_tick < 0 || base_duration_ticks <= 0 || extension_ticks < 0) {
        if (error_message != nullptr) *error_message = "Invalid note geometry";
        return false;
    }
    if (auto* poly_track = authored_poly_track_ptr(&*state_->smd_authoring, track_idx)) {
        if (static_cast<size_t>(authored_span_index) >= poly_track->notes.size()) {
            if (error_message != nullptr) *error_message = "Invalid authored poly note";
            return false;
        }
        const int32_t new_total_ticks = base_duration_ticks + extension_ticks;
        if (start_tick + new_total_ticks > poly_track->total_ticks) {
            if (error_message != nullptr) *error_message = "Note resize exceeds covered span";
            return false;
        }
        auto& note = poly_track->notes[static_cast<size_t>(authored_span_index)];
        if (poly_track_has_same_key_overlap(
                *poly_track, note.relative_key, start_tick, new_total_ticks, authored_span_index)) {
            if (error_message != nullptr) *error_message = "PolyTrack cannot overlap two notes of the same key";
            return false;
        }
        note.start_tick = start_tick;
        note.base_ticks = base_duration_ticks;
        note.total_ticks = new_total_ticks;
    } else {
        auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
        if (authored_track == nullptr) {
            if (error_message != nullptr) *error_message = "Invalid track index";
            return false;
        }
        if (static_cast<size_t>(authored_span_index) >= authored_track->spans.size()) {
            if (error_message != nullptr) *error_message = "Invalid authored note span";
            return false;
        }
        const FFTSmdAuthoredSpan span = authored_track->spans[static_cast<size_t>(authored_span_index)];
        if (span.relative_key < 0 || span.relative_key >= 12) {
            if (error_message != nullptr) *error_message = "Selected span is not a note";
            return false;
        }
        const int32_t new_total_ticks = base_duration_ticks + extension_ticks;
        const int32_t old_start_tick = span.start_tick;
        const int32_t old_end_tick = span.start_tick + span.total_ticks;
        const int32_t new_end_tick = start_tick + new_total_ticks;
        if (new_end_tick > authored_track->total_ticks) {
            if (error_message != nullptr) *error_message = "Note resize exceeds covered span";
            return false;
        }
        authored_track->spans = rewrite_authored_spans(
            *authored_track,
            std::min(old_start_tick, start_tick),
            std::max(old_end_tick, new_end_tick),
            FFTSmdAuthoredSpan {
                .start_tick = start_tick,
                .total_ticks = new_total_ticks,
                .base_ticks = base_duration_ticks,
                .velocity_hint = span.velocity_hint,
                .relative_key = span.relative_key,
            });
    }
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

bool FFTTrackNoteService::resize_rest_duration_by_source_event(
    int32_t track_idx, int32_t source_event_index, int32_t delta_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (source_event_index < 0 || delta_ticks == 0) {
        if (error_message != nullptr) *error_message = "Invalid rest resize";
        return false;
    }
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    const auto authored_index = authored_span_index_for_source_event(compiled, track_idx, source_event_index);
    if (!authored_index.has_value()) {
        if (error_message != nullptr) *error_message = "Failed to resolve authored rest span";
        return false;
    }
    return resize_rest_duration_by_authored_index(
        track_idx, static_cast<int32_t>(*authored_index), delta_ticks, error_message);
}

bool FFTTrackNoteService::resize_rest_duration_by_authored_index(
    int32_t track_idx, int32_t authored_span_index, int32_t delta_ticks,
    std::string* error_message
) {
    if (!bridge_->ensure_loaded(error_message)) {
        return false;
    }
    if (authored_span_index < 0 || delta_ticks == 0) {
        if (error_message != nullptr) *error_message = "Invalid rest resize";
        return false;
    }
    auto* authored_track = authored_raw_track_ptr(&*state_->smd_authoring, track_idx);
    if (authored_track == nullptr) {
        if (error_message != nullptr) *error_message = "Invalid track index";
        return false;
    }
    if (static_cast<size_t>(authored_span_index) >= authored_track->spans.size()) {
        if (error_message != nullptr) *error_message = "Invalid authored rest span";
        return false;
    }

    auto span = authored_track->spans[static_cast<size_t>(authored_span_index)];
    if (span.relative_key != 13) {
        if (error_message != nullptr) *error_message = "Selected span is not a rest";
        return false;
    }

    const int32_t old_end_tick = span.start_tick + span.total_ticks;
    const int32_t new_total_ticks = span.total_ticks + delta_ticks;
    if (new_total_ticks <= 0) {
        if (error_message != nullptr) *error_message = "Rest resize exceeds span length";
        return false;
    }

    if (delta_ticks > 0) {
        authored_track->spans = insert_time_into_authored_spans(*authored_track, old_end_tick, delta_ticks);
        authored_track->total_ticks += delta_ticks;
        for (auto& opcode : authored_track->opcodes) {
            if (opcode.tick >= old_end_tick) {
                opcode.tick += delta_ticks;
            }
        }
    } else {
        span.total_ticks = new_total_ticks;
        span.base_ticks = new_total_ticks;
        authored_track->spans[static_cast<size_t>(authored_span_index)] = span;
        const int32_t new_end_tick = span.start_tick + span.total_ticks;
        for (size_t i = static_cast<size_t>(authored_span_index) + 1; i < authored_track->spans.size(); ++i) {
            authored_track->spans[i].start_tick += delta_ticks;
        }
        authored_track->total_ticks = std::max(0, authored_track->total_ticks + delta_ticks);
        for (auto& opcode : authored_track->opcodes) {
            if (opcode.tick > new_end_tick) {
                opcode.tick += delta_ticks;
            }
        }
        authored_track->spans = normalize_authored_spans_for_edit(
            authored_track->spans, authored_track->total_ticks);
    }

    renumber_authored_opcode_stack_order(*authored_track);
    sync_legacy_raw_tracks_from_parts(*state_->smd_authoring);
    FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return finalize_(compiled, error_message);
}

}  // namespace fftplugin
