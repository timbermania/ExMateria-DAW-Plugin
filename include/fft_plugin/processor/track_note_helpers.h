#pragma once

#include <cstdint>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"

// Span / poly-note manipulation primitives shared by the note-editing
// service and the still-on-processor edit paths. All operate on authored
// types (no engine state); they're free functions for call-site brevity.

namespace fftplugin {

// Construct a fragment of `source` covering [start_tick, end_tick).
// base_ticks is preserved when start matches the source's start (the
// fragment keeps the source's "exact" leading portion); otherwise the
// fragment becomes a fresh exact-encoded slice.
FFTSmdAuthoredPolyNote make_poly_note_fragment_like(
    const FFTSmdAuthoredPolyNote& source,
    int32_t start_tick,
    int32_t end_tick);

FFTSmdAuthoredSpan make_authored_fragment_like(
    const FFTSmdAuthoredSpan& source,
    int32_t start_tick,
    int32_t end_tick);

// Sort, dedupe overlaps, and gap-fill with rests so the returned span
// list densely covers [0, total_ticks). Adjacent rests are merged.
std::vector<FFTSmdAuthoredSpan> normalize_authored_spans_for_edit(
    std::vector<FFTSmdAuthoredSpan> spans,
    int32_t total_ticks);

// Replace `[replace_start_tick, replace_end_tick)` in `track.spans` with
// `inserted_span`. Pre-existing spans that straddle the boundary are
// trimmed.
std::vector<FFTSmdAuthoredSpan> rewrite_authored_spans(
    const FFTSmdAuthoredTrack& track,
    int32_t replace_start_tick,
    int32_t replace_end_tick,
    const FFTSmdAuthoredSpan& inserted_span);

// Insert `duration_ticks` of empty time at `insert_tick`, shifting
// later spans rightward. Mid-span splits create two fragments.
std::vector<FFTSmdAuthoredSpan> insert_time_into_authored_spans(
    const FFTSmdAuthoredTrack& track,
    int32_t insert_tick,
    int32_t duration_ticks);

// Remove `[delete_start_tick, delete_end_tick)`, shifting later spans
// leftward. Spans crossing the deleted region are split + rejoined.
std::vector<FFTSmdAuthoredSpan> delete_time_from_authored_spans(
    const FFTSmdAuthoredTrack& track,
    int32_t delete_start_tick,
    int32_t delete_end_tick);

}  // namespace fftplugin
