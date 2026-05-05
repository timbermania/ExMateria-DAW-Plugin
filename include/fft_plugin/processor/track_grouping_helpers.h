#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"

// Helpers for raw-track <-> poly-track grouping. Used by the grouping
// service inside the plugin and by fft_polytrack_roundtrip_driver to keep
// one source of truth.

namespace fftplugin {

// Subset of is_structure_opcode (excludes END_BAR — 0x90 — which marks
// track end and shouldn't move during poly-grouping).
bool is_structure_opcode_for_grouping(int32_t opcode);

bool authored_opcode_matches_for_grouping(
    const FFTSmdAuthoredOpcode& lhs,
    const FFTSmdAuthoredOpcode& rhs);

// Returns the subset of `opcodes` matching the structure_only flag,
// stable-sorted by (tick, stack_order).
std::vector<FFTSmdAuthoredOpcode> sorted_authored_opcodes_for_grouping(
    const std::vector<FFTSmdAuthoredOpcode>& opcodes,
    bool structure_only);

// Two raw tracks share a compatible structural timeline if their
// total_ticks match and their structure-opcode sequences match in
// content + ordering.
bool authored_structure_timelines_match(
    const FFTSmdAuthoredTrack& lhs,
    const FFTSmdAuthoredTrack& rhs);

// Builds a poly-track from one or more raw-track parts: collects all
// non-rest spans, merges shared structural + non-structural opcodes.
FFTSmdAuthoredPolyTrack poly_track_from_raw_parts(const std::vector<FFTSmdAuthoringPart>& parts);

// Returns true if any existing poly note with the same relative_key
// overlaps the [start_tick, start_tick + total_ticks) interval.
bool poly_track_has_same_key_overlap(
    const FFTSmdAuthoredPolyTrack& poly_track,
    int32_t relative_key,
    int32_t start_tick,
    int32_t total_ticks,
    std::optional<int32_t> exclude_note_index = std::nullopt);

}  // namespace fftplugin
