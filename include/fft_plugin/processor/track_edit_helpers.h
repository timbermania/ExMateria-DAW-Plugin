#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"

// Helpers shared between FFTPluginProcessorCore and the extracted edit
// services (currently FFTTrackEditService). All operate on
// FFTSmdAuthoringDocument / FFTSmdCompiledDocument directly — no engine
// or service state — so they live as free functions rather than methods.
//
// Origin: previously file-private helpers in fft_plugin_processor_core.cpp.

namespace fftplugin {

FFTSmdAuthoringPart* authored_part_ptr(FFTSmdAuthoringDocument* document, int32_t track_idx);
const FFTSmdAuthoringPart* authored_part_ptr(const FFTSmdAuthoringDocument* document, int32_t track_idx);

std::vector<FFTSmdAuthoredOpcode>* authored_part_opcodes(
    FFTSmdAuthoringDocument* document, int32_t track_idx);
const std::vector<FFTSmdAuthoredOpcode>* authored_part_opcodes(
    const FFTSmdAuthoringDocument* document, int32_t track_idx);

FFTSmdAuthoredPolyTrack* authored_poly_track_ptr(
    FFTSmdAuthoringDocument* document, int32_t track_idx);
const FFTSmdAuthoredPolyTrack* authored_poly_track_ptr(
    const FFTSmdAuthoringDocument* document, int32_t track_idx);

FFTSmdAuthoredTrack* authored_raw_track_ptr(
    FFTSmdAuthoringDocument* document, int32_t track_idx);
const FFTSmdAuthoredTrack* authored_raw_track_ptr(
    const FFTSmdAuthoringDocument* document, int32_t track_idx);

bool authored_part_is_poly_track(const FFTSmdAuthoringDocument* document, int32_t track_idx);

void sync_legacy_raw_tracks_from_parts(FFTSmdAuthoringDocument& document);

// Maps a compiled-track source-event index back to the authored opcode
// index that produced it. For raw tracks this scans
// compiled.authored_opcode_source_indices; for poly tracks the authored
// index equals the source index (1:1 by construction).
std::optional<size_t> resolve_authored_opcode_index(
    const FFTSmdAuthoringDocument& document,
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    int32_t source_event_index);

std::optional<size_t> authored_opcode_index_for_source_event(
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    int32_t source_event_index);

std::optional<size_t> authored_span_index_for_source_event(
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    int32_t source_event_index);

// Renumber stack_order densely from 0..N-1 in current order. Call after
// inserting/removing/reordering opcodes to keep stack_order in sync.
void renumber_authored_opcode_stack_order(FFTSmdAuthoredTrack& track);
void renumber_authored_opcode_stack_order(std::vector<FFTSmdAuthoredOpcode>& opcodes);

// Move the authored opcode at `moving_authored_index` so that, in the
// compiled output ordering, it lands at insertion_sequence_index (or at
// the end when insertion_sequence_index < 0). Renumbers stack_order on
// completion. Returns the new authored index of the moved opcode.
size_t reorder_authored_opcode_by_compiled_slot(
    std::vector<FFTSmdAuthoredOpcode>& opcodes,
    const FFTSmdCompiledDocument& compiled,
    int32_t track_idx,
    size_t moving_authored_index,
    int32_t insertion_sequence_index);

}  // namespace fftplugin
