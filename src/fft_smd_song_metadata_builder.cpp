#include "fft_plugin/fft_smd_song_metadata_builder.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "fft_plugin/fft_smd_file.h"  // fft_tempo_to_bpm

namespace fftplugin {

namespace {

int32_t ticks_per_beat_for_denominator(int32_t denominator) {
    if (denominator <= 0) {
        denominator = 4;
    }
    return std::max(1, (48 * 4) / denominator);
}

}  // namespace

std::vector<FFTSmdGridSegment> build_grid_segments_from_time_signatures(
    const std::vector<std::pair<int32_t, std::pair<int32_t, int32_t>>>& changes,
    int32_t total_ticks
) {
    std::vector<FFTSmdGridSegment> segments;
    if (total_ticks <= 0) {
        return segments;
    }

    std::vector<std::pair<int32_t, std::pair<int32_t, int32_t>>> normalized = changes;
    if (normalized.empty() || normalized.front().first > 0) {
        normalized.insert(normalized.begin(), {0, {4, 4}});
    }

    for (size_t i = 0; i < normalized.size(); ++i) {
        const int32_t start_tick = std::max(0, normalized[i].first);
        const int32_t end_tick = (i + 1 < normalized.size()) ? normalized[i + 1].first : total_ticks;
        if (end_tick <= start_tick) {
            continue;
        }

        const int32_t numerator = std::max(1, normalized[i].second.first);
        const int32_t denominator = std::max(1, normalized[i].second.second);
        const int32_t ticks_per_beat = ticks_per_beat_for_denominator(denominator);
        segments.push_back(FFTSmdGridSegment {
            .start_tick = start_tick,
            .end_tick = end_tick,
            .numerator = numerator,
            .denominator = denominator,
            .ticks_per_beat = ticks_per_beat,
            .ticks_per_bar = std::max(1, numerator * ticks_per_beat),
        });
    }

    return segments;
}

std::vector<FFTSmdSecondMarker> build_second_markers_from_tempo_changes(
    const std::vector<std::pair<int32_t, int32_t>>& changes,
    int32_t total_ticks,
    int32_t initial_tempo
) {
    std::vector<FFTSmdSecondMarker> markers;
    if (total_ticks <= 0) {
        return markers;
    }

    std::vector<std::pair<int32_t, int32_t>> normalized = changes;
    if (normalized.empty()) {
        normalized.push_back({0, initial_tempo > 0 ? initial_tempo : 102});
    } else if (normalized.front().first > 0) {
        normalized.insert(normalized.begin(), {0, initial_tempo > 0 ? initial_tempo : normalized.front().second});
    }

    markers.push_back(FFTSmdSecondMarker {.tick = 0, .label = "0s"});
    double accumulated_seconds = 0.0;
    int next_second = 1;
    for (size_t i = 0; i < normalized.size(); ++i) {
        const int32_t start_tick = std::max(0, normalized[i].first);
        const int32_t end_tick = (i + 1 < normalized.size()) ? normalized[i + 1].first : total_ticks;
        if (end_tick <= start_tick) {
            continue;
        }
        const double bpm = fft_tempo_to_bpm(normalized[i].second);
        const double ticks_per_second = std::max(0.0001, (bpm / 60.0) * 48.0);
        const double segment_seconds = static_cast<double>(end_tick - start_tick) / ticks_per_second;
        while (static_cast<double>(next_second) <= accumulated_seconds + segment_seconds + 1e-9) {
            const double seconds_into_segment = static_cast<double>(next_second) - accumulated_seconds;
            const int32_t tick = start_tick + static_cast<int32_t>(std::round(seconds_into_segment * ticks_per_second));
            if (tick <= total_ticks) {
                markers.push_back(FFTSmdSecondMarker {
                    .tick = tick,
                    .label = std::to_string(next_second) + "s",
                });
            }
            next_second += 1;
        }
        accumulated_seconds += segment_seconds;
    }
    return markers;
}

}  // namespace fftplugin
