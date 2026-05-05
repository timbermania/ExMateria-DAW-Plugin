#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "fft_plugin/fft_smd_inspector.h"  // for FFTSmdGridSegment, FFTSmdSecondMarker

namespace fftplugin {

// Builds the per-tick grid layout from a list of time-signature changes
// (changes[i] = {tick, {numerator, denominator}}). Output covers
// [0, total_ticks). When `changes` is empty or doesn't start at tick 0,
// a 4/4 segment is prepended.
std::vector<FFTSmdGridSegment> build_grid_segments_from_time_signatures(
    const std::vector<std::pair<int32_t, std::pair<int32_t, int32_t>>>& changes,
    int32_t total_ticks);

// Walks tempo changes and emits a second-marker every ~1 second of
// musical time. Used by the inspector's UI presentation and by the
// sequencer's playback presentation. `initial_tempo` falls back to 102
// when no tempo events appear.
std::vector<FFTSmdSecondMarker> build_second_markers_from_tempo_changes(
    const std::vector<std::pair<int32_t, int32_t>>& changes,
    int32_t total_ticks,
    int32_t initial_tempo);

}  // namespace fftplugin
