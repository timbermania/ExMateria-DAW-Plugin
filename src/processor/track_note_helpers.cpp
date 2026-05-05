#include "fft_plugin/processor/track_note_helpers.h"

#include <algorithm>
#include <utility>

namespace fftplugin {

FFTSmdAuthoredPolyNote make_poly_note_fragment_like(
    const FFTSmdAuthoredPolyNote& source,
    int32_t start_tick,
    int32_t end_tick
) {
    FFTSmdAuthoredPolyNote fragment = source;
    fragment.start_tick = start_tick;
    fragment.total_ticks = std::max(0, end_tick - start_tick);
    if (start_tick == source.start_tick) {
        fragment.base_ticks = std::min(source.base_ticks, fragment.total_ticks);
    } else {
        fragment.base_ticks = fragment.total_ticks;
    }
    return fragment;
}

FFTSmdAuthoredSpan make_authored_fragment_like(
    const FFTSmdAuthoredSpan& source,
    int32_t start_tick,
    int32_t end_tick
) {
    FFTSmdAuthoredSpan fragment = source;
    fragment.start_tick = start_tick;
    fragment.total_ticks = std::max(0, end_tick - start_tick);
    fragment.base_ticks = fragment.total_ticks;
    return fragment;
}

std::vector<FFTSmdAuthoredSpan> normalize_authored_spans_for_edit(
    std::vector<FFTSmdAuthoredSpan> spans,
    int32_t total_ticks
) {
    std::sort(
        spans.begin(),
        spans.end(),
        [](const FFTSmdAuthoredSpan& lhs, const FFTSmdAuthoredSpan& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            return lhs.relative_key < rhs.relative_key;
        });

    std::vector<FFTSmdAuthoredSpan> normalized;
    int32_t cursor = 0;
    for (auto span : spans) {
        if (span.total_ticks <= 0) {
            continue;
        }

        int32_t span_start = span.start_tick;
        int32_t span_end = span.start_tick + span.total_ticks;
        if (span_end <= cursor) {
            continue;
        }
        if (span_start < cursor) {
            span_start = cursor;
            span.start_tick = cursor;
            span.total_ticks = span_end - cursor;
            span.base_ticks = std::min(span.base_ticks, span.total_ticks);
        }

        if (span_start > cursor) {
            normalized.push_back(FFTSmdAuthoredSpan {
                .start_tick = cursor,
                .total_ticks = span_start - cursor,
                .base_ticks = span_start - cursor,
                .velocity_hint = 100,
                .relative_key = 13,
            });
        }

        if (!normalized.empty() &&
            normalized.back().relative_key == 13 &&
            span.relative_key == 13 &&
            normalized.back().start_tick + normalized.back().total_ticks == span.start_tick) {
            normalized.back().total_ticks += span.total_ticks;
            normalized.back().base_ticks = normalized.back().total_ticks;
        } else {
            normalized.push_back(span);
        }
        cursor = span.start_tick + span.total_ticks;
    }

    if (cursor < total_ticks) {
        if (!normalized.empty() &&
            normalized.back().relative_key == 13 &&
            normalized.back().start_tick + normalized.back().total_ticks == cursor) {
            normalized.back().total_ticks += total_ticks - cursor;
            normalized.back().base_ticks = normalized.back().total_ticks;
        } else {
            normalized.push_back(FFTSmdAuthoredSpan {
                .start_tick = cursor,
                .total_ticks = total_ticks - cursor,
                .base_ticks = total_ticks - cursor,
                .velocity_hint = 100,
                .relative_key = 13,
            });
        }
    }

    return normalized;
}

std::vector<FFTSmdAuthoredSpan> rewrite_authored_spans(
    const FFTSmdAuthoredTrack& track,
    int32_t replace_start_tick,
    int32_t replace_end_tick,
    const FFTSmdAuthoredSpan& inserted_span
) {
    std::vector<FFTSmdAuthoredSpan> spans;
    spans.reserve(track.spans.size() + 2U);
    for (const auto& span : track.spans) {
        const int32_t span_start = span.start_tick;
        const int32_t span_end = span.start_tick + span.total_ticks;
        if (span_end <= replace_start_tick || span_start >= replace_end_tick) {
            spans.push_back(span);
            continue;
        }
        if (span_start < replace_start_tick) {
            spans.push_back(make_authored_fragment_like(span, span_start, replace_start_tick));
        }
        if (span_end > replace_end_tick) {
            spans.push_back(make_authored_fragment_like(span, replace_end_tick, span_end));
        }
    }
    spans.push_back(inserted_span);
    return normalize_authored_spans_for_edit(std::move(spans), track.total_ticks);
}

std::vector<FFTSmdAuthoredSpan> insert_time_into_authored_spans(
    const FFTSmdAuthoredTrack& track,
    int32_t insert_tick,
    int32_t duration_ticks
) {
    std::vector<FFTSmdAuthoredSpan> spans;
    spans.reserve(track.spans.size() + 3U);
    bool inserted_rest = false;
    for (const auto& span : track.spans) {
        const int32_t span_start = span.start_tick;
        const int32_t span_end = span.start_tick + span.total_ticks;
        if (span_end <= insert_tick) {
            spans.push_back(span);
            continue;
        }
        if (!inserted_rest) {
            spans.push_back(FFTSmdAuthoredSpan {
                .start_tick = insert_tick,
                .total_ticks = duration_ticks,
                .base_ticks = duration_ticks,
                .velocity_hint = span.velocity_hint,
                .relative_key = 13,
            });
            inserted_rest = true;
        }
        if (span_start >= insert_tick) {
            auto shifted = span;
            shifted.start_tick += duration_ticks;
            spans.push_back(shifted);
            continue;
        }
        spans.push_back(make_authored_fragment_like(span, span_start, insert_tick));
        if (span_end > insert_tick) {
            auto shifted = make_authored_fragment_like(
                span,
                insert_tick + duration_ticks,
                span_end + duration_ticks);
            spans.push_back(shifted);
        }
    }
    if (!inserted_rest) {
        spans.push_back(FFTSmdAuthoredSpan {
            .start_tick = insert_tick,
            .total_ticks = duration_ticks,
            .base_ticks = duration_ticks,
            .velocity_hint = 100,
            .relative_key = 13,
        });
    }
    return normalize_authored_spans_for_edit(std::move(spans), track.total_ticks + duration_ticks);
}

std::vector<FFTSmdAuthoredSpan> delete_time_from_authored_spans(
    const FFTSmdAuthoredTrack& track,
    int32_t delete_start_tick,
    int32_t delete_end_tick
) {
    const int32_t duration_ticks = std::max(0, delete_end_tick - delete_start_tick);
    std::vector<FFTSmdAuthoredSpan> spans;
    spans.reserve(track.spans.size());
    for (const auto& span : track.spans) {
        const int32_t span_start = span.start_tick;
        const int32_t span_end = span.start_tick + span.total_ticks;
        if (span_end <= delete_start_tick) {
            spans.push_back(span);
            continue;
        }
        if (span_start >= delete_end_tick) {
            auto shifted = span;
            shifted.start_tick -= duration_ticks;
            spans.push_back(shifted);
            continue;
        }

        const bool keep_left = span_start < delete_start_tick;
        const bool keep_right = span_end > delete_end_tick;
        if (keep_left && keep_right) {
            spans.push_back(make_authored_fragment_like(span, span_start, span_end - duration_ticks));
        } else if (keep_left) {
            spans.push_back(make_authored_fragment_like(span, span_start, delete_start_tick));
        } else if (keep_right) {
            spans.push_back(make_authored_fragment_like(span, delete_start_tick, span_end - duration_ticks));
        }
    }
    return normalize_authored_spans_for_edit(
        std::move(spans),
        std::max(0, track.total_ticks - duration_ticks));
}

}  // namespace fftplugin
