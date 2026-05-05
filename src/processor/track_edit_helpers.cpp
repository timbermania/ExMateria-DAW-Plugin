#include "fft_plugin/processor/track_edit_helpers.h"

#include <algorithm>

namespace fftplugin {

FFTSmdAuthoringPart* authored_part_ptr(FFTSmdAuthoringDocument* document, int32_t track_idx) {
    if (document == nullptr || track_idx < 0 ||
        static_cast<size_t>(track_idx) >= document->parts.size()) {
        return nullptr;
    }
    return &document->parts[static_cast<size_t>(track_idx)];
}

const FFTSmdAuthoringPart* authored_part_ptr(const FFTSmdAuthoringDocument* document, int32_t track_idx) {
    if (document == nullptr || track_idx < 0 ||
        static_cast<size_t>(track_idx) >= document->parts.size()) {
        return nullptr;
    }
    return &document->parts[static_cast<size_t>(track_idx)];
}

std::vector<FFTSmdAuthoredOpcode>* authored_part_opcodes(
    FFTSmdAuthoringDocument* document, int32_t track_idx
) {
    auto* part = authored_part_ptr(document, track_idx);
    if (part == nullptr) {
        return nullptr;
    }
    return part->kind == FFTSmdAuthoringPartKind::poly_track
        ? &part->poly_track.opcodes : &part->raw_track.opcodes;
}

const std::vector<FFTSmdAuthoredOpcode>* authored_part_opcodes(
    const FFTSmdAuthoringDocument* document, int32_t track_idx
) {
    const auto* part = authored_part_ptr(document, track_idx);
    if (part == nullptr) {
        return nullptr;
    }
    return part->kind == FFTSmdAuthoringPartKind::poly_track
        ? &part->poly_track.opcodes : &part->raw_track.opcodes;
}

FFTSmdAuthoredPolyTrack* authored_poly_track_ptr(
    FFTSmdAuthoringDocument* document, int32_t track_idx
) {
    auto* part = authored_part_ptr(document, track_idx);
    if (part == nullptr || part->kind != FFTSmdAuthoringPartKind::poly_track) {
        return nullptr;
    }
    return &part->poly_track;
}

const FFTSmdAuthoredPolyTrack* authored_poly_track_ptr(
    const FFTSmdAuthoringDocument* document, int32_t track_idx
) {
    const auto* part = authored_part_ptr(document, track_idx);
    if (part == nullptr || part->kind != FFTSmdAuthoringPartKind::poly_track) {
        return nullptr;
    }
    return &part->poly_track;
}

FFTSmdAuthoredTrack* authored_raw_track_ptr(
    FFTSmdAuthoringDocument* document, int32_t track_idx
) {
    auto* part = authored_part_ptr(document, track_idx);
    if (part == nullptr || part->kind != FFTSmdAuthoringPartKind::raw_track) {
        return nullptr;
    }
    return &part->raw_track;
}

const FFTSmdAuthoredTrack* authored_raw_track_ptr(
    const FFTSmdAuthoringDocument* document, int32_t track_idx
) {
    const auto* part = authored_part_ptr(document, track_idx);
    if (part == nullptr || part->kind != FFTSmdAuthoringPartKind::raw_track) {
        return nullptr;
    }
    return &part->raw_track;
}

bool authored_part_is_poly_track(const FFTSmdAuthoringDocument* document, int32_t track_idx) {
    const auto* part = authored_part_ptr(document, track_idx);
    return part != nullptr && part->kind == FFTSmdAuthoringPartKind::poly_track;
}

void sync_legacy_raw_tracks_from_parts(FFTSmdAuthoringDocument& document) {
    document.tracks.clear();
    for (const auto& part : document.parts) {
        if (part.kind == FFTSmdAuthoringPartKind::raw_track) {
            document.tracks.push_back(part.raw_track);
        }
    }
}

std::optional<size_t> authored_opcode_index_for_source_event(
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    int32_t source_event_index
) {
    if (track_idx < 0 ||
        static_cast<size_t>(track_idx) >= compiled.authored_opcode_source_indices.size() ||
        source_event_index < 0) {
        return std::nullopt;
    }
    const auto& indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
    for (size_t authored_index = 0; authored_index < indices.size(); ++authored_index) {
        if (indices[authored_index] == source_event_index) {
            return authored_index;
        }
    }
    return std::nullopt;
}

void renumber_authored_opcode_stack_order(FFTSmdAuthoredTrack& track) {
    for (size_t index = 0; index < track.opcodes.size(); ++index) {
        track.opcodes[index].stack_order = static_cast<int32_t>(index);
    }
}

void renumber_authored_opcode_stack_order(std::vector<FFTSmdAuthoredOpcode>& opcodes) {
    for (size_t index = 0; index < opcodes.size(); ++index) {
        opcodes[index].stack_order = static_cast<int32_t>(index);
    }
}

size_t reorder_authored_opcode_by_compiled_slot(
    std::vector<FFTSmdAuthoredOpcode>& opcodes,
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    size_t moving_authored_index,
    int32_t insertion_sequence_index
) {
    std::vector<size_t> compiled_order;
    compiled_order.reserve(opcodes.size());
    for (size_t authored_index = 0; authored_index < opcodes.size(); ++authored_index) {
        compiled_order.push_back(authored_index);
    }
    std::stable_sort(
        compiled_order.begin(),
        compiled_order.end(),
        [&compiled, track_idx](size_t lhs, size_t rhs) {
            const auto& indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
            return indices[lhs] < indices[rhs];
        });

    compiled_order.erase(
        std::remove(compiled_order.begin(), compiled_order.end(), moving_authored_index),
        compiled_order.end());

    size_t insertion_pos = compiled_order.size();
    if (insertion_sequence_index >= 0 &&
        track_idx >= 0 &&
        static_cast<size_t>(track_idx) < compiled.authored_opcode_source_indices.size()) {
        const auto& indices = compiled.authored_opcode_source_indices[static_cast<size_t>(track_idx)];
        for (size_t pos = 0; pos < compiled_order.size(); ++pos) {
            if (indices[compiled_order[pos]] >= insertion_sequence_index) {
                insertion_pos = pos;
                break;
            }
        }
    }
    compiled_order.insert(compiled_order.begin() + static_cast<std::ptrdiff_t>(insertion_pos), moving_authored_index);

    std::vector<FFTSmdAuthoredOpcode> reordered;
    reordered.reserve(opcodes.size());
    size_t new_authored_index = 0;
    for (size_t pos = 0; pos < compiled_order.size(); ++pos) {
        if (compiled_order[pos] == moving_authored_index) {
            new_authored_index = pos;
        }
        reordered.push_back(opcodes[compiled_order[pos]]);
    }
    opcodes = std::move(reordered);
    renumber_authored_opcode_stack_order(opcodes);
    return new_authored_index;
}

std::optional<size_t> authored_span_index_for_source_event(
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    int32_t source_event_index
) {
    if (track_idx < 0 ||
        static_cast<size_t>(track_idx) >= compiled.authored_span_source_indices.size() ||
        source_event_index < 0) {
        return std::nullopt;
    }
    const auto& indices = compiled.authored_span_source_indices[static_cast<size_t>(track_idx)];
    for (size_t authored_index = 0; authored_index < indices.size(); ++authored_index) {
        if (indices[authored_index] == source_event_index) {
            return authored_index;
        }
    }
    return std::nullopt;
}

std::optional<size_t> resolve_authored_opcode_index(
    const FFTSmdAuthoringDocument& document,
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    int32_t source_event_index
) {
    if (authored_part_is_poly_track(&document, track_idx)) {
        const auto* opcodes = authored_part_opcodes(&document, track_idx);
        if (opcodes == nullptr || source_event_index < 0 ||
            static_cast<size_t>(source_event_index) >= opcodes->size()) {
            return std::nullopt;
        }
        return static_cast<size_t>(source_event_index);
    }
    return authored_opcode_index_for_source_event(compiled, track_idx, source_event_index);
}

}  // namespace fftplugin
