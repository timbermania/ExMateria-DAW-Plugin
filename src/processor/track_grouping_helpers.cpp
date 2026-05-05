#include "fft_plugin/processor/track_grouping_helpers.h"

#include <algorithm>

#include "fft_plugin/fft_smd_opcodes.h"
#include "fft_plugin/processor/track_edit_helpers.h"

namespace fftplugin {

namespace {

std::vector<FFTSmdAuthoredOpcode> merge_nonstructural_authored_opcodes(
    const std::vector<FFTSmdAuthoringPart>& parts
) {
    std::vector<FFTSmdAuthoredOpcode> merged;
    for (const auto& part : parts) {
        const auto sorted = sorted_authored_opcodes_for_grouping(part.raw_track.opcodes, false);
        for (const auto& opcode : sorted) {
            const bool already_present = std::any_of(
                merged.begin(),
                merged.end(),
                [&opcode](const FFTSmdAuthoredOpcode& existing) {
                    return authored_opcode_matches_for_grouping(existing, opcode);
                });
            if (!already_present) {
                merged.push_back(opcode);
            }
        }
    }

    std::stable_sort(
        merged.begin(),
        merged.end(),
        [](const FFTSmdAuthoredOpcode& lhs, const FFTSmdAuthoredOpcode& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            if (lhs.opcode.opcode != rhs.opcode.opcode) {
                return lhs.opcode.opcode < rhs.opcode.opcode;
            }
            if (lhs.opcode.params != rhs.opcode.params) {
                return lhs.opcode.params < rhs.opcode.params;
            }
            if (lhs.enabled != rhs.enabled) {
                return lhs.enabled < rhs.enabled;
            }
            return lhs.exact_timing < rhs.exact_timing;
        });
    renumber_authored_opcode_stack_order(merged);
    return merged;
}

// Preserve the full opcode timeline from the first selected raw track so
// structural control flow survives grouping. Then merge in any additional
// non-structural opcodes from the other selected tracks.
std::vector<FFTSmdAuthoredOpcode> merge_shared_authored_opcodes(
    const std::vector<FFTSmdAuthoringPart>& parts
) {
    if (parts.empty()) {
        return {};
    }
    if (parts.size() == 1) {
        return parts.front().raw_track.opcodes;
    }

    std::vector<FFTSmdAuthoredOpcode> merged = parts.front().raw_track.opcodes;
    std::stable_sort(
        merged.begin(),
        merged.end(),
        [](const FFTSmdAuthoredOpcode& lhs, const FFTSmdAuthoredOpcode& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.stack_order < rhs.stack_order;
        });

    const auto additional_nonstructural = merge_nonstructural_authored_opcodes(parts);
    for (const auto& opcode : additional_nonstructural) {
        const bool already_present = std::any_of(
            merged.begin(),
            merged.end(),
            [&opcode](const FFTSmdAuthoredOpcode& existing) {
                return authored_opcode_matches_for_grouping(existing, opcode);
            });
        if (!already_present) {
            merged.push_back(opcode);
        }
    }

    std::stable_sort(
        merged.begin(),
        merged.end(),
        [](const FFTSmdAuthoredOpcode& lhs, const FFTSmdAuthoredOpcode& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.stack_order < rhs.stack_order;
        });
    renumber_authored_opcode_stack_order(merged);
    return merged;
}

}  // namespace

bool is_structure_opcode_for_grouping(int32_t opcode) {
    switch (static_cast<FFTSmdOpcode>(opcode & 0xFF)) {
    case FFTSmdOpcode::LOOP:
    case FFTSmdOpcode::REPEAT:
    case FFTSmdOpcode::CODA:
    case FFTSmdOpcode::REPEAT_BREAK:
        return true;
    default:
        return false;
    }
}

bool authored_opcode_matches_for_grouping(
    const FFTSmdAuthoredOpcode& lhs,
    const FFTSmdAuthoredOpcode& rhs
) {
    return lhs.tick == rhs.tick &&
        lhs.enabled == rhs.enabled &&
        lhs.exact_timing == rhs.exact_timing &&
        lhs.opcode.opcode == rhs.opcode.opcode &&
        lhs.opcode.params == rhs.opcode.params;
}

std::vector<FFTSmdAuthoredOpcode> sorted_authored_opcodes_for_grouping(
    const std::vector<FFTSmdAuthoredOpcode>& opcodes,
    bool structure_only
) {
    std::vector<FFTSmdAuthoredOpcode> sorted;
    sorted.reserve(opcodes.size());
    for (const auto& opcode : opcodes) {
        if (is_structure_opcode_for_grouping(opcode.opcode.opcode) == structure_only) {
            sorted.push_back(opcode);
        }
    }
    std::stable_sort(
        sorted.begin(),
        sorted.end(),
        [](const FFTSmdAuthoredOpcode& lhs, const FFTSmdAuthoredOpcode& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.stack_order < rhs.stack_order;
        });
    return sorted;
}

bool authored_structure_timelines_match(
    const FFTSmdAuthoredTrack& lhs,
    const FFTSmdAuthoredTrack& rhs
) {
    if (lhs.total_ticks != rhs.total_ticks) {
        return false;
    }
    const auto lhs_structure = sorted_authored_opcodes_for_grouping(lhs.opcodes, true);
    const auto rhs_structure = sorted_authored_opcodes_for_grouping(rhs.opcodes, true);
    if (lhs_structure.size() != rhs_structure.size()) {
        return false;
    }
    for (size_t opcode_index = 0; opcode_index < lhs_structure.size(); ++opcode_index) {
        if (!authored_opcode_matches_for_grouping(lhs_structure[opcode_index], rhs_structure[opcode_index])) {
            return false;
        }
    }
    return true;
}

FFTSmdAuthoredPolyTrack poly_track_from_raw_parts(const std::vector<FFTSmdAuthoringPart>& parts) {
    FFTSmdAuthoredPolyTrack poly_track;
    for (const auto& part : parts) {
        poly_track.total_ticks = std::max(poly_track.total_ticks, part.raw_track.total_ticks);
        for (const auto& span : part.raw_track.spans) {
            if (span.relative_key < 0 || span.relative_key >= 12) {
                continue;
            }
            poly_track.notes.push_back(FFTSmdAuthoredPolyNote {
                .start_tick = span.start_tick,
                .total_ticks = span.total_ticks,
                .base_ticks = span.base_ticks,
                .velocity_hint = span.velocity_hint,
                .relative_key = span.relative_key,
            });
        }
    }
    poly_track.opcodes = merge_shared_authored_opcodes(parts);
    return poly_track;
}

bool poly_track_has_same_key_overlap(
    const FFTSmdAuthoredPolyTrack& poly_track,
    int32_t relative_key,
    int32_t start_tick,
    int32_t total_ticks,
    std::optional<int32_t> exclude_note_index
) {
    const int32_t end_tick = start_tick + total_ticks;
    for (size_t note_index = 0; note_index < poly_track.notes.size(); ++note_index) {
        if (exclude_note_index.has_value() &&
            static_cast<int32_t>(note_index) == *exclude_note_index) {
            continue;
        }
        const auto& existing = poly_track.notes[note_index];
        if (existing.relative_key != relative_key) {
            continue;
        }
        const int32_t existing_start = existing.start_tick;
        const int32_t existing_end = existing.start_tick + existing.total_ticks;
        if (std::max(start_tick, existing_start) < std::min(end_tick, existing_end)) {
            return true;
        }
    }
    return false;
}

}  // namespace fftplugin
