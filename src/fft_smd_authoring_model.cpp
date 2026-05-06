#include "fft_plugin/fft_smd_authoring_model.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <unordered_map>

#include "fft_plugin/fft_smd_lane_packer.h"
#include "fft_plugin/fft_smd_opcodes.h"

namespace fftplugin {

namespace {

struct IndexedAuthoredSpan {
    FFTSmdAuthoredSpan span;
    int32_t authored_index = -1;
};

struct IndexedPolyNote {
    FFTSmdAuthoredPolyNote note;
    int32_t authored_index = -1;
};

struct IndexedOpcode {
    FFTSmdAuthoredOpcode opcode;
    size_t authored_index = 0;
};

bool is_time_only_opcode(const FFTSmdOpcodeEvent& opcode) {
    return fftplugin::is_time_only_opcode(opcode.opcode);
}

int32_t time_only_opcode_duration(const FFTSmdOpcodeEvent& opcode) {
    if (!is_time_only_opcode(opcode) || opcode.params.empty()) {
        return 0;
    }
    return std::max(opcode.params[0], 0);
}

bool has_following_time_only_opcode(
    const std::vector<FFTSmdTrackEvent>& track,
    size_t start_index,
    bool note_continuation
) {
    for (size_t index = start_index; index < track.size(); ++index) {
        const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&track[index]);
        if (opcode == nullptr) {
            return false;
        }
        if (note_continuation) {
            if (opcode->opcode == FFTSmdOpcode::FERMATA) {
                return true;
            }
        } else if (fftplugin::is_time_only_opcode(opcode->opcode)) {
            return true;
        }
    }
    return false;
}

std::vector<FFTSmdTrackEvent> build_rest_events_for_duration(int32_t total_ticks, int32_t velocity_hint) {
    (void) velocity_hint;
    std::vector<FFTSmdTrackEvent> events;
    int32_t remaining = std::max(total_ticks, 1);
    while (remaining > 0) {
        const int32_t chunk = std::min(remaining, 255);
        events.emplace_back(FFTSmdOpcodeEvent {
            .opcode = op_byte(FFTSmdOpcode::REST),
            .params = {chunk},
        });
        remaining -= chunk;
    }
    return events;
}

int32_t largest_exact_note_duration_at_or_below(int32_t ticks) {
    int32_t best = -1;
    for (size_t index = 1; index < kFftSmdDeltaTimeTable.size(); ++index) {
        const int32_t candidate = kFftSmdDeltaTimeTable[index];
        if (candidate <= ticks && candidate > best) {
            best = candidate;
        }
    }
    return best;
}

std::vector<FFTSmdTrackEvent> build_note_events_for_duration(
    int32_t note_relative_key,
    int32_t total_ticks,
    int32_t velocity_hint
) {
    if (note_relative_key == 13) {
        return build_rest_events_for_duration(total_ticks, velocity_hint);
    }

    std::vector<FFTSmdTrackEvent> events;
    const int32_t clamped_relative_key = std::clamp(note_relative_key, 0, 11);
    int32_t remaining = std::max(total_ticks, 1);
    const int32_t exact_base_ticks = largest_exact_note_duration_at_or_below(remaining);
    const int32_t base_ticks = exact_base_ticks > 0 ? exact_base_ticks : remaining;
    events.emplace_back(FFTSmdNoteEvent {
        .velocity = velocity_hint,
        .relative_key = clamped_relative_key,
        .delta_time = base_ticks,
    });
    remaining -= base_ticks;
    while (remaining > 0) {
        const int32_t chunk = std::min(remaining, 255);
        events.emplace_back(FFTSmdOpcodeEvent {
            .opcode = op_byte(FFTSmdOpcode::FERMATA),
            .params = {chunk},
        });
        remaining -= chunk;
    }
    return events;
}

std::vector<FFTSmdTrackEvent> build_wait_opcode_events(int32_t total_ticks, int32_t opcode) {
    std::vector<FFTSmdTrackEvent> events;
    int32_t remaining = std::max(total_ticks, 0);
    while (remaining > 0) {
        const int32_t chunk = std::min(remaining, 255);
        events.emplace_back(FFTSmdOpcodeEvent {
            .opcode = opcode,
            .params = {chunk},
        });
        remaining -= chunk;
    }
    return events;
}

void append_segment_events(
    std::vector<FFTSmdTrackEvent>& out,
    const FFTSmdAuthoredSpan& span,
    int32_t duration_ticks,
    bool first_segment
) {
    if (duration_ticks <= 0) {
        return;
    }

    if (span.relative_key == 13) {
        const auto events = first_segment
            ? build_rest_events_for_duration(duration_ticks, span.velocity_hint)
            : build_wait_opcode_events(duration_ticks, op_byte(FFTSmdOpcode::REST));
        out.insert(out.end(), events.begin(), events.end());
        return;
    }

    const auto events = first_segment
        ? build_note_events_for_duration(span.relative_key, duration_ticks, span.velocity_hint)
        : build_wait_opcode_events(duration_ticks, op_byte(FFTSmdOpcode::FERMATA));
    out.insert(out.end(), events.begin(), events.end());
}

std::vector<IndexedAuthoredSpan> normalize_indexed_spans(
    std::vector<IndexedAuthoredSpan> spans,
    int32_t total_ticks
) {
    std::sort(
        spans.begin(),
        spans.end(),
        [](const IndexedAuthoredSpan& lhs, const IndexedAuthoredSpan& rhs) {
            if (lhs.span.start_tick != rhs.span.start_tick) {
                return lhs.span.start_tick < rhs.span.start_tick;
            }
            return lhs.span.relative_key < rhs.span.relative_key;
        });

    std::vector<IndexedAuthoredSpan> normalized;
    int32_t cursor = 0;
    for (auto indexed_span : spans) {
        auto& span = indexed_span.span;
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
            normalized.push_back(IndexedAuthoredSpan {
                .span = FFTSmdAuthoredSpan {
                    .start_tick = cursor,
                    .total_ticks = span_start - cursor,
                    .base_ticks = span_start - cursor,
                    .velocity_hint = 100,
                    .relative_key = 13,
                },
                .authored_index = -1,
            });
        }

        if (!normalized.empty() &&
            normalized.back().authored_index < 0 &&
            indexed_span.authored_index < 0 &&
            normalized.back().span.relative_key == 13 &&
            span.relative_key == 13 &&
            normalized.back().span.start_tick + normalized.back().span.total_ticks == span.start_tick) {
            normalized.back().span.total_ticks += span.total_ticks;
            normalized.back().span.base_ticks = normalized.back().span.total_ticks;
        } else {
            normalized.push_back(indexed_span);
        }
        cursor = span.start_tick + span.total_ticks;
    }

    if (cursor < total_ticks) {
        if (!normalized.empty() &&
            normalized.back().authored_index < 0 &&
            normalized.back().span.relative_key == 13 &&
            normalized.back().span.start_tick + normalized.back().span.total_ticks == cursor) {
            normalized.back().span.total_ticks += total_ticks - cursor;
            normalized.back().span.base_ticks = normalized.back().span.total_ticks;
        } else {
            normalized.push_back(IndexedAuthoredSpan {
                .span = FFTSmdAuthoredSpan {
                    .start_tick = cursor,
                    .total_ticks = total_ticks - cursor,
                    .base_ticks = total_ticks - cursor,
                    .velocity_hint = 100,
                    .relative_key = 13,
                },
                .authored_index = -1,
            });
        }
    }

    return normalized;
}

std::vector<IndexedAuthoredSpan> indexed_spans_from_raw_track(const FFTSmdAuthoredTrack& track) {
    std::vector<IndexedAuthoredSpan> indexed;
    indexed.reserve(track.spans.size());
    for (size_t authored_index = 0; authored_index < track.spans.size(); ++authored_index) {
        indexed.push_back(IndexedAuthoredSpan {
            .span = track.spans[authored_index],
            .authored_index = static_cast<int32_t>(authored_index),
        });
    }
    return normalize_indexed_spans(std::move(indexed), track.total_ticks);
}

std::vector<IndexedPolyNote> normalize_indexed_poly_notes(
    std::vector<IndexedPolyNote> notes,
    int32_t total_ticks
) {
    std::vector<IndexedPolyNote> normalized;
    normalized.reserve(notes.size());
    std::sort(
        notes.begin(),
        notes.end(),
        [](const IndexedPolyNote& lhs, const IndexedPolyNote& rhs) {
            if (lhs.note.start_tick != rhs.note.start_tick) {
                return lhs.note.start_tick < rhs.note.start_tick;
            }
            if (lhs.note.relative_key != rhs.note.relative_key) {
                return lhs.note.relative_key < rhs.note.relative_key;
            }
            if (lhs.note.total_ticks != rhs.note.total_ticks) {
                return lhs.note.total_ticks < rhs.note.total_ticks;
            }
            return lhs.authored_index < rhs.authored_index;
        });
    for (auto indexed_note : notes) {
        auto& note = indexed_note.note;
        note.start_tick = std::max(0, note.start_tick);
        note.total_ticks = std::max(1, note.total_ticks);
        note.base_ticks = std::clamp(note.base_ticks, 1, note.total_ticks);
        note.relative_key = std::clamp(note.relative_key, 0, 11);
        if (note.start_tick >= total_ticks) {
            continue;
        }
        if (note.start_tick + note.total_ticks > total_ticks) {
            note.total_ticks = std::max(1, total_ticks - note.start_tick);
            note.base_ticks = std::min(note.base_ticks, note.total_ticks);
        }
        normalized.push_back(indexed_note);
    }
    return normalized;
}

FFTSmdAuthoringPart make_raw_track_part(FFTSmdAuthoredTrack track, std::string name) {
    FFTSmdAuthoringPart part;
    part.kind = FFTSmdAuthoringPartKind::raw_track;
    part.name = std::move(name);
    part.raw_track = std::move(track);
    return part;
}

int32_t part_total_ticks(const FFTSmdAuthoringPart& part) {
    return part.kind == FFTSmdAuthoringPartKind::poly_track
        ? part.poly_track.total_ticks
        : part.raw_track.total_ticks;
}

void compile_indexed_track_to_compiled_lane(
    const std::vector<IndexedAuthoredSpan>& spans,
    const std::vector<FFTSmdAuthoredOpcode>& authored_opcodes,
    int32_t compiled_track_index,
    std::vector<FFTSmdTrackEvent>& out,
    std::vector<int32_t>& part_span_source_indices,
    std::vector<int32_t>& part_opcode_source_indices,
    std::vector<uint64_t>& part_span_source_keys,
    std::vector<uint64_t>& part_opcode_source_keys,
    std::unordered_set<uint64_t>& disabled_opcode_keys,
    bool emit_generated_loop_markers
) {
    std::vector<IndexedOpcode> opcodes;
    opcodes.reserve(authored_opcodes.size());
    for (size_t authored_index = 0; authored_index < authored_opcodes.size(); ++authored_index) {
        opcodes.push_back(IndexedOpcode {
            .opcode = authored_opcodes[authored_index],
            .authored_index = authored_index,
        });
    }

    std::stable_sort(
        opcodes.begin(),
        opcodes.end(),
        [](const IndexedOpcode& lhs, const IndexedOpcode& rhs) {
            if (lhs.opcode.tick != rhs.opcode.tick) {
                return lhs.opcode.tick < rhs.opcode.tick;
            }
            return lhs.opcode.stack_order < rhs.opcode.stack_order;
        });

    size_t opcode_index = 0;
    bool loop_marker_emitted = false;
    const auto emit_loop_marker = [&out, &loop_marker_emitted, emit_generated_loop_markers]() {
        if (!emit_generated_loop_markers) return;
        if (loop_marker_emitted) return;
        FFTSmdOpcodeEvent loop_start;
        loop_start.opcode = op_byte(FFTSmdOpcode::LOOP);  // per-loop-iteration replay anchor
        out.emplace_back(loop_start);
        loop_marker_emitted = true;
    };
    const auto emit_opcode = [&out, &part_opcode_source_indices, &part_opcode_source_keys,
                              &disabled_opcode_keys, compiled_track_index](
                                  const IndexedOpcode& indexed_opcode) {
        const int32_t new_index = static_cast<int32_t>(out.size());
        out.emplace_back(indexed_opcode.opcode.opcode);
        if (indexed_opcode.authored_index < part_opcode_source_indices.size() &&
            part_opcode_source_indices[indexed_opcode.authored_index] < 0) {
            part_opcode_source_indices[indexed_opcode.authored_index] = new_index;
            part_opcode_source_keys[indexed_opcode.authored_index] =
                smd_track_event_key(compiled_track_index, new_index);
        }
        if (!indexed_opcode.opcode.enabled) {
            disabled_opcode_keys.insert(smd_track_event_key(compiled_track_index, new_index));
        }
    };

    for (const auto& indexed_span : spans) {
        const auto& span = indexed_span.span;
        const int32_t span_start = span.start_tick;
        const int32_t span_end = span.start_tick + span.total_ticks;

        while (opcode_index < opcodes.size() && opcodes[opcode_index].opcode.tick < span_start) {
            emit_opcode(opcodes[opcode_index]);
            opcode_index += 1;
        }
        while (opcode_index < opcodes.size() && opcodes[opcode_index].opcode.tick == span_start) {
            emit_opcode(opcodes[opcode_index]);
            opcode_index += 1;
        }

        // Emit the per-track loop-start marker right before the first span,
        // after all initial setup opcodes (volume, pan, instrument, etc.).
        // The engine's loop-back lands here, so init runs once but per-loop
        // body replays from this marker.
        emit_loop_marker();

        const int32_t span_source_index = static_cast<int32_t>(out.size());
        std::vector<int32_t> breakpoints;
        if (span.relative_key >= 0 && span.relative_key < 12 && span.base_ticks < span.total_ticks) {
            breakpoints.push_back(span.start_tick + span.base_ticks);
        }
        // Tick-critical opcodes (TEMPO, TEMPO_SLIDE, TIME_SIGNATURE) must
        // fire at their authored tick — they affect global timing or bar
        // structure, not just whatever instrument owns the current span.
        // Without this, on the conductor (one rest span covering the whole
        // song) every tempo opcode would queue to span_end below.
        const auto opcode_is_tick_critical = [](int32_t op) {
            return op == op_byte(FFTSmdOpcode::TEMPO)
                || op == op_byte(FFTSmdOpcode::TEMPO_SLIDE)
                || op == op_byte(FFTSmdOpcode::TIME_SIGNATURE);
        };
        for (const auto& indexed_opcode : opcodes) {
            const bool tick_critical = opcode_is_tick_critical(indexed_opcode.opcode.opcode.opcode);
            if (!indexed_opcode.opcode.exact_timing && !tick_critical) {
                continue;
            }
            if (indexed_opcode.opcode.tick > span_start && indexed_opcode.opcode.tick < span_end) {
                breakpoints.push_back(indexed_opcode.opcode.tick);
            }
        }
        std::sort(breakpoints.begin(), breakpoints.end());
        breakpoints.erase(std::unique(breakpoints.begin(), breakpoints.end()), breakpoints.end());

        int32_t cursor = span_start;
        bool first_segment = true;
        for (const int32_t breakpoint : breakpoints) {
            append_segment_events(out, span, breakpoint - cursor, first_segment);
            first_segment = false;
            cursor = breakpoint;
            while (opcode_index < opcodes.size() && opcodes[opcode_index].opcode.tick == breakpoint) {
                emit_opcode(opcodes[opcode_index]);
                opcode_index += 1;
            }
        }

        append_segment_events(out, span, span_end - cursor, first_segment);
        if (indexed_span.authored_index >= 0 &&
            static_cast<size_t>(indexed_span.authored_index) < part_span_source_indices.size()) {
            part_span_source_indices[static_cast<size_t>(indexed_span.authored_index)] = span_source_index;
            part_span_source_keys[static_cast<size_t>(indexed_span.authored_index)] =
                smd_track_event_key(compiled_track_index, span_source_index);
        }

        // Authored opcode ticks are a display/order proxy. Only exact_timing
        // (or tick-critical opcode kinds, which we promote to exact_timing
        // above) keeps an opcode on an internal breakpoint; everything else
        // queues to the next real boundary.
        while (opcode_index < opcodes.size() &&
               opcodes[opcode_index].opcode.tick > span_start &&
               opcodes[opcode_index].opcode.tick < span_end &&
               !opcodes[opcode_index].opcode.exact_timing &&
               !opcode_is_tick_critical(opcodes[opcode_index].opcode.opcode.opcode)) {
            emit_opcode(opcodes[opcode_index]);
            opcode_index += 1;
        }
    }

    while (opcode_index < opcodes.size()) {
        emit_opcode(opcodes[opcode_index]);
        opcode_index += 1;
    }
    // Defensive: if a track had no spans at all, still emit the loop marker
    // so the engine has a valid loop-back point.
    emit_loop_marker();

    // Standard track-end pattern from vanilla SMDs: AC FF 90.
    //   AC FF: set instrument to 0xFF (silence/release the SPU voice)
    //   90   : EndBar terminator
    bool needs_silence = true;
    bool needs_terminator = true;
    if (!out.empty()) {
        if (const auto* last_op = std::get_if<FFTSmdOpcodeEvent>(&out.back())) {
            if (last_op->opcode == FFTSmdOpcode::END_BAR) needs_terminator = false;
        }
    }
    if (needs_silence) {
        FFTSmdOpcodeEvent silence;
        silence.opcode = op_byte(FFTSmdOpcode::INSTRUMENT);
        silence.params.push_back(static_cast<int32_t>(0xFF));
        out.emplace_back(silence);
    }
    if (needs_terminator) {
        FFTSmdOpcodeEvent terminator;
        terminator.opcode = op_byte(FFTSmdOpcode::END_BAR);
        out.emplace_back(terminator);
    }
}

std::vector<IndexedAuthoredSpan> build_poly_voice_lane(
    const std::vector<IndexedPolyNote>& notes,
    int32_t total_ticks,
    std::vector<int32_t>* lane_assignment_out,
    int32_t lane_index
) {
    std::vector<IndexedAuthoredSpan> lane_spans;
    for (size_t note_index = 0; note_index < notes.size(); ++note_index) {
        if (lane_assignment_out != nullptr &&
            note_index < lane_assignment_out->size() &&
            (*lane_assignment_out)[note_index] == lane_index) {
            const auto& indexed_note = notes[note_index];
            const auto& note = indexed_note.note;
            lane_spans.push_back(IndexedAuthoredSpan {
                .span = FFTSmdAuthoredSpan {
                    .start_tick = note.start_tick,
                    .total_ticks = note.total_ticks,
                    .base_ticks = note.base_ticks,
                    .velocity_hint = note.velocity_hint,
                    .relative_key = note.relative_key,
                },
                .authored_index = indexed_note.authored_index,
            });
        }
    }
    return normalize_indexed_spans(std::move(lane_spans), total_ticks);
}

std::vector<int32_t> assign_poly_notes_to_voice_lanes(
    const std::vector<IndexedPolyNote>& notes,
    int32_t* lane_count_out
) {
    std::vector<IndexedPolyNote> sorted_notes = notes;
    std::unordered_map<int32_t, size_t> normalized_index_by_authored_index;
    normalized_index_by_authored_index.reserve(notes.size());
    for (size_t note_index = 0; note_index < notes.size(); ++note_index) {
        normalized_index_by_authored_index[notes[note_index].authored_index] = note_index;
    }
    std::stable_sort(
        sorted_notes.begin(),
        sorted_notes.end(),
        [](const IndexedPolyNote& lhs, const IndexedPolyNote& rhs) {
            if (lhs.note.start_tick != rhs.note.start_tick) {
                return lhs.note.start_tick < rhs.note.start_tick;
            }
            if (lhs.note.relative_key != rhs.note.relative_key) {
                return lhs.note.relative_key < rhs.note.relative_key;
            }
            return lhs.authored_index < rhs.authored_index;
        });

    std::vector<int32_t> lane_ends;
    std::vector<int32_t> assignments(notes.size(), -1);
    for (const auto& indexed_note : sorted_notes) {
        int32_t lane_index = -1;
        for (size_t existing_lane = 0; existing_lane < lane_ends.size(); ++existing_lane) {
            if (lane_ends[existing_lane] <= indexed_note.note.start_tick) {
                lane_index = static_cast<int32_t>(existing_lane);
                break;
            }
        }
        if (lane_index < 0) {
            lane_index = static_cast<int32_t>(lane_ends.size());
            lane_ends.push_back(0);
        }
        lane_ends[static_cast<size_t>(lane_index)] = indexed_note.note.start_tick + indexed_note.note.total_ticks;
        const auto normalized_it = normalized_index_by_authored_index.find(indexed_note.authored_index);
        if (normalized_it != normalized_index_by_authored_index.end()) {
            assignments[normalized_it->second] = lane_index;
        }
    }

    if (lane_count_out != nullptr) {
        *lane_count_out = std::max<int32_t>(1, static_cast<int32_t>(lane_ends.size()));
    }
    return assignments;
}

}  // namespace

uint64_t smd_track_event_key(int32_t track_idx, int32_t source_event_index) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(track_idx)) << 32U) |
        static_cast<uint32_t>(source_event_index);
}

FFTSmdAuthoringDocument import_smd_authoring_document(
    const FFTSmdFile& smd,
    const std::function<bool(int32_t, int32_t)>& is_opcode_disabled
) {
    FFTSmdAuthoringDocument document;
    document.track_count = smd.track_count;
    document.initial_tempo = smd.initial_tempo;
    document.initial_volume = smd.initial_volume;
    document.assoc_wds_id = smd.assoc_wds_id;
    document.song_title = smd.song_title;
    document.parts.reserve(smd.track_events.size());

    for (size_t track_idx = 0; track_idx < smd.track_events.size(); ++track_idx) {
        const auto& track = smd.track_events[track_idx];
        FFTSmdAuthoredTrack authored_track;
        int32_t tick = 0;
        size_t event_index = 0;
        while (event_index < track.size()) {
            const auto& event = track[event_index];

            if (const auto* note = std::get_if<FFTSmdNoteEvent>(&event)) {
                const bool is_note = note->relative_key >= 0 && note->relative_key < 12;
                const bool is_rest = note->relative_key == 13;
                if (is_note || is_rest) {
                    FFTSmdAuthoredSpan span;
                    span.start_tick = tick;
                    span.total_ticks = std::max(note->delta_time, 1);
                    span.base_ticks = std::max(note->delta_time, 1);
                    span.velocity_hint = note->velocity;
                    span.relative_key = is_note ? note->relative_key : 13;

                    tick += span.total_ticks;
                    event_index += 1;

                    while (event_index < track.size()) {
                        const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&track[event_index]);
                        if (opcode == nullptr) {
                            break;
                        }
                        if (is_note && opcode->opcode == FFTSmdOpcode::FERMATA) {
                            const int32_t duration = std::max(time_only_opcode_duration(*opcode), 1);
                            span.total_ticks += duration;
                            tick += duration;
                            event_index += 1;
                            continue;
                        }
                        if (is_rest && fftplugin::is_time_only_opcode(opcode->opcode)) {
                            const int32_t duration = std::max(time_only_opcode_duration(*opcode), 1);
                            span.total_ticks += duration;
                            tick += duration;
                            event_index += 1;
                            continue;
                        }
                        if (!is_time_only_opcode(*opcode)) {
                            authored_track.opcodes.push_back(FFTSmdAuthoredOpcode {
                                .tick = tick,
                                .stack_order = static_cast<int32_t>(event_index),
                                .enabled = !(is_opcode_disabled &&
                                    is_opcode_disabled(static_cast<int32_t>(track_idx), static_cast<int32_t>(event_index))),
                                .exact_timing = has_following_time_only_opcode(track, event_index + 1, is_note),
                                .opcode = *opcode,
                            });
                            event_index += 1;
                            continue;
                        }
                        break;
                    }

                    authored_track.spans.push_back(span);
                    continue;
                }
            }

            if (const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&event)) {
                if (fftplugin::is_time_only_opcode(opcode->opcode)) {
                    FFTSmdAuthoredSpan span;
                    span.start_tick = tick;
                    span.total_ticks = std::max(time_only_opcode_duration(*opcode), 1);
                    span.base_ticks = span.total_ticks;
                    span.velocity_hint = 100;
                    span.relative_key = 13;

                    tick += span.total_ticks;
                    event_index += 1;

                    while (event_index < track.size()) {
                        const auto* next_opcode = std::get_if<FFTSmdOpcodeEvent>(&track[event_index]);
                        if (next_opcode == nullptr) {
                            break;
                        }
                        if (fftplugin::is_time_only_opcode(next_opcode->opcode)) {
                            const int32_t duration = std::max(time_only_opcode_duration(*next_opcode), 1);
                            span.total_ticks += duration;
                            tick += duration;
                            event_index += 1;
                            continue;
                        }
                        authored_track.opcodes.push_back(FFTSmdAuthoredOpcode {
                            .tick = tick,
                            .stack_order = static_cast<int32_t>(event_index),
                            .enabled = !(is_opcode_disabled &&
                                is_opcode_disabled(static_cast<int32_t>(track_idx), static_cast<int32_t>(event_index))),
                            .exact_timing = has_following_time_only_opcode(track, event_index + 1, false),
                            .opcode = *next_opcode,
                        });
                        event_index += 1;
                    }

                    authored_track.spans.push_back(span);
                    continue;
                }

                authored_track.opcodes.push_back(FFTSmdAuthoredOpcode {
                    .tick = tick,
                    .stack_order = static_cast<int32_t>(event_index),
                    .enabled = !(is_opcode_disabled &&
                        is_opcode_disabled(static_cast<int32_t>(track_idx), static_cast<int32_t>(event_index))),
                    .opcode = *opcode,
                });
            }

            event_index += 1;
        }

        authored_track.total_ticks = tick;
        auto normalized_spans = indexed_spans_from_raw_track(authored_track);
        authored_track.spans.clear();
        authored_track.spans.reserve(normalized_spans.size());
        for (const auto& indexed_span : normalized_spans) {
            authored_track.spans.push_back(indexed_span.span);
        }

        document.tracks.push_back(authored_track);
        document.parts.push_back(make_raw_track_part(
            std::move(authored_track),
            "Track " + std::to_string(track_idx)));
    }

    document.track_count = static_cast<int32_t>(smd.track_events.size());
    return document;
}

namespace {

int32_t floor_div_i32(int32_t a, int32_t b) {
    int32_t q = a / b;
    int32_t r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

int32_t floor_mod_i32(int32_t a, int32_t b) {
    int32_t r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) {
        r += b;
    }
    return r;
}

void apply_track_transposition_to_events(
    std::vector<FFTSmdTrackEvent>& events,
    int32_t transposition_semitones
) {
    if (transposition_semitones == 0) {
        return;
    }
    constexpr int32_t kDefaultOctave = 4;
    int32_t authored_octave = kDefaultOctave;
    int32_t emitted_octave = -1;
    std::vector<FFTSmdTrackEvent> rewritten;
    rewritten.reserve(events.size() + 8);
    for (const auto& event : events) {
        if (const auto* op = std::get_if<FFTSmdOpcodeEvent>(&event)) {
            if (op->opcode == FFTSmdOpcode::OCTAVE && !op->params.empty()) {
                authored_octave = op->params[0];
                const int32_t base_shift = floor_div_i32(transposition_semitones, 12);
                const int32_t new_emitted = std::clamp(authored_octave + base_shift, 0, 8);
                FFTSmdOpcodeEvent updated = *op;
                updated.params[0] = new_emitted;
                rewritten.emplace_back(updated);
                emitted_octave = new_emitted;
                continue;
            }
            rewritten.push_back(event);
            continue;
        }
        const auto& note = std::get<FFTSmdNoteEvent>(event);
        if (note.relative_key < 0 || note.relative_key > 11) {
            rewritten.push_back(event);
            continue;
        }
        const int32_t absolute = authored_octave * 12 + note.relative_key + transposition_semitones;
        const int32_t new_oct = std::clamp(floor_div_i32(absolute, 12), 0, 8);
        const int32_t new_rel = std::clamp(floor_mod_i32(absolute, 12), 0, 11);
        if (new_oct != emitted_octave) {
            FFTSmdOpcodeEvent octave_set;
            octave_set.opcode = op_byte(FFTSmdOpcode::OCTAVE);
            octave_set.params = {new_oct};
            rewritten.emplace_back(octave_set);
            emitted_octave = new_oct;
        }
        FFTSmdNoteEvent shifted = note;
        shifted.relative_key = new_rel;
        rewritten.emplace_back(shifted);
    }
    events = std::move(rewritten);
}

}  // namespace

FFTSmdCompiledDocument compile_smd_authoring_document_impl(
    const FFTSmdAuthoringDocument& document,
    bool emit_generated_loop_markers
) {
    FFTSmdCompiledDocument compiled;
    compiled.smd.initial_tempo = document.initial_tempo;
    compiled.smd.initial_volume = document.initial_volume;
    compiled.smd.assoc_wds_id = document.assoc_wds_id;
    compiled.smd.song_title = document.song_title;
    compiled.authored_span_source_indices.resize(document.parts.size());
    compiled.authored_opcode_source_indices.resize(document.parts.size());
    compiled.authored_span_source_keys.resize(document.parts.size());
    compiled.authored_opcode_source_keys.resize(document.parts.size());
    compiled.authored_part_compiled_track_indices.resize(document.parts.size());

    for (size_t part_index = 0; part_index < document.parts.size(); ++part_index) {
        const auto& part = document.parts[part_index];
        if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
            std::vector<IndexedPolyNote> indexed_notes;
            indexed_notes.reserve(part.poly_track.notes.size());
            for (size_t note_index = 0; note_index < part.poly_track.notes.size(); ++note_index) {
                indexed_notes.push_back(IndexedPolyNote {
                    .note = part.poly_track.notes[note_index],
                    .authored_index = static_cast<int32_t>(note_index),
                });
            }
            const auto normalized_notes = normalize_indexed_poly_notes(
                std::move(indexed_notes),
                part.poly_track.total_ticks);
            compiled.authored_span_source_indices[part_index].assign(part.poly_track.notes.size(), -1);
            compiled.authored_opcode_source_indices[part_index].assign(part.poly_track.opcodes.size(), -1);
            compiled.authored_span_source_keys[part_index].assign(part.poly_track.notes.size(), 0);
            compiled.authored_opcode_source_keys[part_index].assign(part.poly_track.opcodes.size(), 0);

            FFTSmdAuthoredPolyTrack normalized_poly = part.poly_track;
            normalized_poly.notes.clear();
            normalized_poly.notes.reserve(normalized_notes.size());
            for (const auto& indexed_note : normalized_notes) {
                normalized_poly.notes.push_back(indexed_note.note);
            }

            int32_t lane_count = 1;
            const std::vector<int32_t> lane_assignments =
                assign_poly_notes_to_voice_lanes(normalized_notes, &lane_count);
            for (int32_t lane_index = 0; lane_index < lane_count; ++lane_index) {
                const int32_t compiled_track_index = static_cast<int32_t>(compiled.smd.track_events.size());
                compiled.smd.track_events.emplace_back();
                compiled.authored_part_compiled_track_indices[part_index].push_back(compiled_track_index);
                auto indexed_spans = build_poly_voice_lane(
                    normalized_notes,
                    normalized_poly.total_ticks,
                    const_cast<std::vector<int32_t>*>(&lane_assignments),
                    lane_index);
                compile_indexed_track_to_compiled_lane(
                    indexed_spans,
                    normalized_poly.opcodes,
                    compiled_track_index,
                    compiled.smd.track_events.back(),
                    compiled.authored_span_source_indices[part_index],
                    compiled.authored_opcode_source_indices[part_index],
                    compiled.authored_span_source_keys[part_index],
                    compiled.authored_opcode_source_keys[part_index],
                    compiled.disabled_opcode_keys,
                    emit_generated_loop_markers);
                apply_track_transposition_to_events(
                    compiled.smd.track_events.back(),
                    part.poly_track.track_transposition);
            }
            continue;
        }

        compiled.authored_span_source_indices[part_index].assign(part.raw_track.spans.size(), -1);
        compiled.authored_opcode_source_indices[part_index].assign(part.raw_track.opcodes.size(), -1);
        compiled.authored_span_source_keys[part_index].assign(part.raw_track.spans.size(), 0);
        compiled.authored_opcode_source_keys[part_index].assign(part.raw_track.opcodes.size(), 0);
        const int32_t compiled_track_index = static_cast<int32_t>(compiled.smd.track_events.size());
        compiled.smd.track_events.emplace_back();
        compiled.authored_part_compiled_track_indices[part_index].push_back(compiled_track_index);
        auto indexed_spans = indexed_spans_from_raw_track(part.raw_track);
        compile_indexed_track_to_compiled_lane(
            indexed_spans,
            part.raw_track.opcodes,
            compiled_track_index,
            compiled.smd.track_events.back(),
            compiled.authored_span_source_indices[part_index],
            compiled.authored_opcode_source_indices[part_index],
            compiled.authored_span_source_keys[part_index],
            compiled.authored_opcode_source_keys[part_index],
            compiled.disabled_opcode_keys,
            emit_generated_loop_markers);
        apply_track_transposition_to_events(
            compiled.smd.track_events.back(),
            part.raw_track.track_transposition);
    }

    compiled.smd.track_count = static_cast<int32_t>(compiled.smd.track_events.size());
    return compiled;
}

FFTSmdCompiledDocument compile_smd_authoring_document(const FFTSmdAuthoringDocument& document) {
    return compile_smd_authoring_document_impl(document, true);
}

namespace {

int32_t compute_part_lane_count(const FFTSmdAuthoringPart& part) {
    if (part.kind != FFTSmdAuthoringPartKind::poly_track) {
        return 1;
    }
    std::vector<IndexedPolyNote> indexed;
    indexed.reserve(part.poly_track.notes.size());
    for (size_t i = 0; i < part.poly_track.notes.size(); ++i) {
        indexed.push_back(IndexedPolyNote {
            .note = part.poly_track.notes[i],
            .authored_index = static_cast<int32_t>(i),
        });
    }
    auto normalized = normalize_indexed_poly_notes(std::move(indexed), part.poly_track.total_ticks);
    int32_t lane_count = 1;
    (void) assign_poly_notes_to_voice_lanes(normalized, &lane_count);
    return lane_count;
}

// Returns indices (into part.poly_track.notes) of notes assigned to the highest
// lane, sorted in priority-ascending order (lowest velocity, shortest duration,
// latest start_tick first → these would be "least missed" if dropped).
std::vector<size_t> top_lane_note_indices_by_priority(const FFTSmdAuthoringPart& part) {
    std::vector<size_t> result;
    if (part.kind != FFTSmdAuthoringPartKind::poly_track) {
        return result;
    }
    std::vector<IndexedPolyNote> indexed;
    indexed.reserve(part.poly_track.notes.size());
    for (size_t i = 0; i < part.poly_track.notes.size(); ++i) {
        indexed.push_back(IndexedPolyNote {
            .note = part.poly_track.notes[i],
            .authored_index = static_cast<int32_t>(i),
        });
    }
    auto normalized = normalize_indexed_poly_notes(std::move(indexed), part.poly_track.total_ticks);
    int32_t lane_count = 1;
    auto assignments = assign_poly_notes_to_voice_lanes(normalized, &lane_count);
    if (lane_count <= 1) {
        return result;
    }
    const int32_t top_lane = lane_count - 1;
    struct Cand {
        size_t authored_index;
        int32_t velocity;
        int32_t total_ticks;
        int32_t start_tick;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (assignments[i] != top_lane) continue;
        const auto& n = normalized[i].note;
        cands.push_back(Cand {
            .authored_index = static_cast<size_t>(normalized[i].authored_index),
            .velocity = n.velocity_hint,
            .total_ticks = n.total_ticks,
            .start_tick = n.start_tick,
        });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.velocity != b.velocity) return a.velocity < b.velocity;
        if (a.total_ticks != b.total_ticks) return a.total_ticks < b.total_ticks;
        return a.start_tick > b.start_tick;
    });
    result.reserve(cands.size());
    for (const auto& c : cands) result.push_back(c.authored_index);
    return result;
}

}  // namespace

// Synthesized control track 0 modeled after vanilla SMDs. Drives global
// tempo + time signature + song-loop structure. Without it, the engine has
// no playback rate and the data screen freezes.
//   BA              ReverbOn
//   A0 <tempo>      Tempo (BPM-derived, 0..255)
//   97 04 04        TimeSig 4/4
//   98 10           Repeat 16 times (loop count)
//   80 FF           Long rest (255 ticks; just a placeholder duration token)
//   99              Coda
//   90              EndBar
static std::vector<FFTSmdTrackEvent> build_synthesized_control_track(
    int32_t initial_tempo,
    int32_t finite_ticks
) {
    const uint8_t tempo_byte = static_cast<uint8_t>(
        std::clamp<int32_t>(initial_tempo > 0 ? initial_tempo : 120, 1, 255));
    std::vector<FFTSmdTrackEvent> events;
    auto add_op = [&](int32_t op, std::initializer_list<int32_t> params) {
        FFTSmdOpcodeEvent ev;
        ev.opcode = op;
        for (auto p : params) ev.params.push_back(p);
        events.emplace_back(ev);
    };
    add_op(op_byte(FFTSmdOpcode::REVERB_ON), {});
    add_op(op_byte(FFTSmdOpcode::TEMPO), {static_cast<int32_t>(tempo_byte)});
    add_op(op_byte(FFTSmdOpcode::TIME_SIGNATURE), {4, 4});
    if (finite_ticks >= 0) {
        const auto rests = build_wait_opcode_events(finite_ticks, op_byte(FFTSmdOpcode::REST));
        events.insert(events.end(), rests.begin(), rests.end());
        add_op(op_byte(FFTSmdOpcode::END_BAR), {});
        return events;
    }
    add_op(op_byte(FFTSmdOpcode::REPEAT), {16});
    add_op(op_byte(FFTSmdOpcode::REST), {0xFF});
    add_op(op_byte(FFTSmdOpcode::CODA), {});
    add_op(op_byte(FFTSmdOpcode::END_BAR), {});
    return events;
}

FFTSmdGameCompileResult compile_smd_authoring_document_for_game(
    const FFTSmdAuthoringDocument& document,
    const FFTSmdGameCompileBudget& budget
) {
    FFTSmdGameCompileResult result;
    FFTSmdAuthoringDocument working = document;

    // target_bytes == 0 historically meant "untrimmed legacy path that emits
    // generated loop markers." That path produces SMDs the FFT engine doesn't
    // play (silent on the data screen). Treat 0 as "trim to fit the engine
    // cap (kEngineMaxSmdBytes / budget.engine_max_bytes)". Any explicit
    // target_bytes above the engine cap is clamped down to it: the engine
    // refuses to load larger SMDs (binary-searched 2026-05-03; 10-sector /
    // 20480-byte plays, 13-sector / 26624-byte silences), so emitting one
    // would just produce a silent ISO.
    FFTSmdGameCompileBudget normalized_budget = budget;
    if (normalized_budget.target_bytes == 0) {
        normalized_budget.target_bytes = normalized_budget.engine_max_bytes;
    }
    if (normalized_budget.target_bytes > normalized_budget.engine_max_bytes) {
        normalized_budget.target_bytes = normalized_budget.engine_max_bytes;
    }

    // Reserve one track for the control header that we prepend at the end,
    // and a few bytes for its serialized body + extra offset-table entry.
    FFTSmdGameCompileBudget effective_budget = normalized_budget;
    if (effective_budget.max_tracks > 1) {
        effective_budget.max_tracks -= 1;
    }
    constexpr size_t kControlTrackReserveBytes = 32;
    if (effective_budget.target_bytes > kControlTrackReserveBytes) {
        effective_budget.target_bytes -= kControlTrackReserveBytes;
    }

    // Always run the loop-stripping flat-trim path. The legacy loop-emitting
    // branch is unreachable now that target_bytes is always > 0.
    const auto compile_for_game = [](const FFTSmdAuthoringDocument& doc) {
        return compile_smd_authoring_document_impl(doc, /*emit_generated_loop_markers=*/false);
    };

    auto initial = compile_for_game(working);
    result.report.pre_reduction_track_count = initial.smd.track_count;

    result.report.parts.resize(working.parts.size());
    for (size_t pi = 0; pi < working.parts.size(); ++pi) {
        result.report.parts[pi].part_name = working.parts[pi].name;
        result.report.parts[pi].original_lanes = compute_part_lane_count(working.parts[pi]);
        result.report.parts[pi].final_lanes = result.report.parts[pi].original_lanes;
    }

    auto part_weight = [](const FFTSmdAuthoringPart& p) -> int64_t {
        int64_t w = 0;
        if (p.kind == FFTSmdAuthoringPartKind::poly_track) {
            for (const auto& n : p.poly_track.notes) {
                w += static_cast<int64_t>(std::max(0, n.velocity_hint)) *
                     static_cast<int64_t>(std::max(1, n.total_ticks));
            }
        } else {
            for (const auto& s : p.raw_track.spans) {
                if (s.relative_key < 0 || s.relative_key >= 12) continue;  // skip rests
                w += static_cast<int64_t>(std::max(0, s.velocity_hint)) *
                     static_cast<int64_t>(std::max(1, s.total_ticks));
            }
        }
        return w;
    };

    const auto reduce_doc_to_track_budget = [&](FFTSmdAuthoringDocument doc) {
        auto comp = compile_for_game(doc);
        while (comp.smd.track_count > effective_budget.max_tracks) {
            int32_t target_pi = -1;
            int32_t target_lanes = 1;
            for (size_t pi = 0; pi < doc.parts.size(); ++pi) {
                if (doc.parts[pi].kind != FFTSmdAuthoringPartKind::poly_track) continue;
                const int32_t lanes = static_cast<int32_t>(
                    comp.authored_part_compiled_track_indices[pi].size());
                if (lanes > target_lanes) {
                    target_lanes = lanes;
                    target_pi = static_cast<int32_t>(pi);
                }
            }
            if (target_pi < 0) {
                break;
            }

            auto& part = doc.parts[static_cast<size_t>(target_pi)];
            const auto drop_indices = top_lane_note_indices_by_priority(part);
            if (drop_indices.empty()) {
                break;
            }
            std::vector<size_t> sorted_desc = drop_indices;
            std::sort(sorted_desc.begin(), sorted_desc.end(), std::greater<size_t>());
            for (size_t idx : sorted_desc) {
                if (idx < part.poly_track.notes.size()) {
                    part.poly_track.notes.erase(part.poly_track.notes.begin() + static_cast<std::ptrdiff_t>(idx));
                }
            }
            comp = compile_for_game(doc);
        }

        while (comp.smd.track_count > effective_budget.max_tracks && !doc.parts.empty()) {
            int32_t lightest_pi = -1;
            int64_t lightest_w = std::numeric_limits<int64_t>::max();
            for (size_t pi = 0; pi < doc.parts.size(); ++pi) {
                const int64_t w = part_weight(doc.parts[pi]);
                if (w < lightest_w) {
                    lightest_w = w;
                    lightest_pi = static_cast<int32_t>(pi);
                }
            }
            if (lightest_pi < 0) break;
            doc.parts.erase(doc.parts.begin() + static_cast<std::ptrdiff_t>(lightest_pi));
            comp = compile_for_game(doc);
        }

        return doc;
    };

    // Stage C runs first for targeted game exports: find the largest shared
    // tick cap that fits the requested byte budget, then let lane reduction
    // operate only on the surviving opening material.
    if (normalized_budget.target_bytes > 0) {
        auto trimmed_copy_at = [](const FFTSmdAuthoringDocument& source, int32_t cap) {
            FFTSmdAuthoringDocument trimmed = source;
            for (auto& part : trimmed.parts) {
                if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
                    auto& poly = part.poly_track;
                    std::vector<FFTSmdAuthoredPolyNote> kept;
                    for (auto n : poly.notes) {
                        if (n.start_tick >= cap) continue;
                        const int32_t end_tick = n.start_tick + n.total_ticks;
                        if (end_tick > cap) {
                            n.total_ticks = cap - n.start_tick;
                            n.base_ticks = std::min(n.base_ticks, n.total_ticks);
                        }
                        if (n.total_ticks > 0) {
                            kept.push_back(n);
                        }
                    }
                    poly.notes = std::move(kept);
                    std::vector<FFTSmdAuthoredOpcode> kept_op;
                    for (const auto& op : poly.opcodes) {
                        if (op.tick < cap) kept_op.push_back(op);
                    }
                    poly.opcodes = std::move(kept_op);
                    poly.total_ticks = std::min(poly.total_ticks, cap);
                } else {
                    auto& raw = part.raw_track;
                    std::vector<FFTSmdAuthoredSpan> kept_spans;
                    for (auto s : raw.spans) {
                        if (s.start_tick >= cap) continue;
                        const int32_t end_tick = s.start_tick + s.total_ticks;
                        if (end_tick > cap) {
                            s.total_ticks = cap - s.start_tick;
                            s.base_ticks = std::min(s.base_ticks, s.total_ticks);
                        }
                        if (s.total_ticks > 0) {
                            kept_spans.push_back(s);
                        }
                    }
                    raw.spans = std::move(kept_spans);
                    std::vector<FFTSmdAuthoredOpcode> kept_op;
                    for (const auto& op : raw.opcodes) {
                        if (op.tick < cap) kept_op.push_back(op);
                    }
                    raw.opcodes = std::move(kept_op);
                    raw.total_ticks = std::min(raw.total_ticks, cap);
                }
            }
            return trimmed;
        };

        // packer_strict=true: only accept tick caps where the cross-track
        // packer can fit every lane into the engine track budget without
        // dropping content. Stage C's binary search will converge on the
        // longest cap where this is possible — i.e. shrink song-time to
        // preserve all musical voices, per the project policy "track
        // fidelity > byte size > song length." If even tick_cap=1 won't
        // fit (extreme density), we fall back to the lossy path that
        // drops top lanes per part.
        bool packer_strict = true;
        const bool stage_c_dbg = std::getenv("FFT_STAGE_C_DEBUG") != nullptr;
        auto serialized_target_size = [&](const FFTSmdAuthoringDocument& doc, int32_t cap) -> size_t {
            auto comp = compile_for_game(doc);
            const int32_t pre_lanes = comp.smd.track_count;
            // Stage 0-prime: global lane reassignment first.
            FFTSmdGlobalLaneAssignerReport gar;
            FFTSmdFile reassigned = global_lane_reassign(
                comp.smd, /*max_lanes=*/normalized_budget.max_tracks - 1,
                /*first_packable_track=*/0, &gar);
            if (gar.ok) comp.smd = std::move(reassigned);
            // Stage A-prime: lane packer (usually a no-op now).
            FFTSmdLanePackerReport pack_report;
            FFTSmdFile packed = pack_lanes_to_track_budget(
                comp.smd, effective_budget.max_tracks,
                /*first_packable_track=*/0, &pack_report);
            const bool packer_fit = pack_report.ok && packed.track_count <= effective_budget.max_tracks;
            if (packer_fit) {
                comp.smd = std::move(packed);
            } else if (!packer_strict) {
                auto reduced = reduce_doc_to_track_budget(doc);
                comp = compile_for_game(reduced);
            } else {
                if (stage_c_dbg) std::fprintf(stderr,
                    "[stage-c] cap=%d  pre_lanes=%d  packer=FAIL (strict reject)\n",
                    (int)cap, (int)pre_lanes);
                return std::numeric_limits<size_t>::max();
            }
            auto control_events = build_synthesized_control_track(doc.initial_tempo, cap);
            comp.smd.track_events.insert(comp.smd.track_events.begin(), std::move(control_events));
            comp.smd.track_count = static_cast<int32_t>(comp.smd.track_events.size());
            FFTSmdLoopRollerReport rr_inner;
            comp.smd = roll_loops_in_smd(comp.smd, &rr_inner);
            std::string serr;
            const auto bytes = serialize_smd_file(comp.smd, &serr);
            const size_t sz = bytes.empty() ? std::numeric_limits<size_t>::max() : bytes.size();
            if (stage_c_dbg) std::fprintf(stderr,
                "[stage-c] cap=%d  pre_lanes=%d  packer=%s  lanes_out=%d  bytes=%zu  budget=%zu\n",
                (int)cap, (int)pre_lanes, packer_fit ? "fit" : "lossy",
                (int)comp.smd.track_count, sz, normalized_budget.target_bytes);
            return sz;
        };

        int32_t hi_tick = 1;
        for (const auto& part : working.parts) {
            hi_tick = std::max(hi_tick, part_total_ticks(part));
        }

        auto run_search = [&](int32_t& out_cap, FFTSmdAuthoringDocument& out_doc) {
            int32_t lo_local = 1;
            int32_t hi_local = hi_tick;
            out_cap = -1;
            while (lo_local <= hi_local) {
                const int32_t mid = lo_local + (hi_local - lo_local) / 2;
                auto candidate = trimmed_copy_at(working, mid);
                const size_t size = serialized_target_size(candidate, mid);
                if (size <= normalized_budget.target_bytes) {
                    out_cap = mid;
                    out_doc = std::move(candidate);
                    lo_local = mid + 1;
                } else {
                    hi_local = mid - 1;
                }
            }
        };
        int32_t best_cap = -1;
        FFTSmdAuthoringDocument best_doc;
        run_search(best_cap, best_doc);
        if (best_cap < 0) {
            packer_strict = false;
            run_search(best_cap, best_doc);
        }

        if (best_cap >= 0) {
            for (size_t pi = 0; pi < working.parts.size() && pi < best_doc.parts.size(); ++pi) {
                const auto& orig = working.parts[pi];
                const auto& trimmed = best_doc.parts[pi];
                int32_t before = 0;
                int32_t after = 0;
                if (orig.kind == FFTSmdAuthoringPartKind::poly_track) {
                    before = static_cast<int32_t>(orig.poly_track.notes.size());
                    after = static_cast<int32_t>(trimmed.poly_track.notes.size());
                } else {
                    before = static_cast<int32_t>(orig.raw_track.spans.size());
                    after = static_cast<int32_t>(trimmed.raw_track.spans.size());
                }
                const int32_t trimmed_count = std::max(0, before - after);
                result.report.parts[pi].notes_trimmed_by_length += trimmed_count;
                result.report.total_notes_trimmed_by_length += trimmed_count;
            }
            result.report.trim_tick_cap = best_cap;
            working = std::move(best_doc);
            initial = compile_for_game(working);
        }
    }

    // Stage A: per-part lane reduction. Drop the entire top lane of the part
    // with the most lanes per iteration. Notes are recorded in priority order
    // so the report tells the user "these were the cheapest to lose." Greedy
    // lane re-assignment (run by the next compile) implicitly handles
    // intra-part lane merging.
    auto compiled = initial;

    // Stage A-prime: cross-track lane packing. Before dropping lanes
    // from poly_tracks (Stage A) or erasing whole parts (Stage B), try
    // to fold lighter lanes into silent windows of heavier lanes. Each
    // insertion is bracketed with the SMD state opcodes the guest needs
    // (instrument, octave, dynamics, pan, reverb) and a restore opcode
    // sequence so the host's subsequent notes still play correctly.
    // Profitability check inside the packer rejects merges whose
    // state-flip overhead outweighs the lane-preamble bytes saved, so
    // worst case the SMD is unchanged.
    // Stage 0-prime: global linear-scan lane reassignment. Replaces
    // per-part lane allocations (which sum local maxima and produce
    // many redundant lanes) with a single greedy global assignment
    // that hits the true cross-track polyphony minimum. Often brings
    // the lane count down to within budget directly, making the
    // packer (Stage A-prime) a no-op.
    {
        FFTSmdFile reassigned = global_lane_reassign(
            compiled.smd, /*max_lanes=*/budget.max_tracks,
            /*first_packable_track=*/0,  // no conductor yet
            &result.report.global_assign_report);
        if (result.report.global_assign_report.ok) {
            compiled.smd = std::move(reassigned);
            // authored_part_compiled_track_indices is stale after
            // reassignment (one source part may now span multiple
            // lanes' notes, or none). Stage A's per-part lane drop
            // depends on it; clear so Stage A becomes a no-op.
            compiled.authored_part_compiled_track_indices.clear();
            compiled.authored_part_compiled_track_indices.resize(
                working.parts.size());
        }
    }

    if (compiled.smd.track_count > effective_budget.max_tracks) {
        FFTSmdFile packed = pack_lanes_to_track_budget(
            compiled.smd, effective_budget.max_tracks,
            /*first_packable_track=*/0, &result.report.pack_report);
        compiled.smd = std::move(packed);
    }

    while (compiled.smd.track_count > effective_budget.max_tracks) {
        int32_t target_pi = -1;
        int32_t target_lanes = 1;
        for (size_t pi = 0; pi < working.parts.size(); ++pi) {
            if (working.parts[pi].kind != FFTSmdAuthoringPartKind::poly_track) continue;
            const int32_t lanes = static_cast<int32_t>(
                compiled.authored_part_compiled_track_indices[pi].size());
            if (lanes > target_lanes) {
                target_lanes = lanes;
                target_pi = static_cast<int32_t>(pi);
            }
        }
        if (target_pi < 0) {
            break;  // No part has >1 lanes — Stage B handles it below.
        }

        auto& part = working.parts[static_cast<size_t>(target_pi)];
        auto& part_report = result.report.parts[static_cast<size_t>(target_pi)];

        const std::vector<size_t> drop_indices = top_lane_note_indices_by_priority(part);
        if (drop_indices.empty()) {
            break;  // Defensive: nothing to drop, can't shrink.
        }

        std::vector<size_t> sorted_desc = drop_indices;
        std::sort(sorted_desc.begin(), sorted_desc.end(), std::greater<size_t>());
        for (size_t idx : sorted_desc) {
            if (idx >= part.poly_track.notes.size()) continue;
            const auto& n = part.poly_track.notes[idx];
            const int32_t end_tick = n.start_tick + n.total_ticks;
            if (part_report.earliest_dropped_tick < 0 || n.start_tick < part_report.earliest_dropped_tick) {
                part_report.earliest_dropped_tick = n.start_tick;
            }
            if (end_tick > part_report.latest_dropped_tick) {
                part_report.latest_dropped_tick = end_tick;
            }
            part_report.notes_dropped += 1;
            result.report.total_notes_dropped += 1;
            part.poly_track.notes.erase(part.poly_track.notes.begin() + static_cast<std::ptrdiff_t>(idx));
        }
        part_report.final_lanes = compute_part_lane_count(part);
        compiled = compile_for_game(working);
    }

    // Stage B: drop entire parts when no per-part reduction is possible.
    // Pick the part with the smallest "musical weight" (sum of velocity ×
    // total_ticks across remaining notes; raw_track parts use their span list
    // the same way). A part with zero notes weighs zero and goes first.
    while (compiled.smd.track_count > effective_budget.max_tracks && !working.parts.empty()) {
        int32_t lightest_pi = -1;
        int64_t lightest_w = std::numeric_limits<int64_t>::max();
        for (size_t pi = 0; pi < working.parts.size(); ++pi) {
            if (result.report.parts[pi].dropped_entirely) continue;
            const int64_t w = part_weight(working.parts[pi]);
            if (w < lightest_w) {
                lightest_w = w;
                lightest_pi = static_cast<int32_t>(pi);
            }
        }
        if (lightest_pi < 0) break;

        auto& pr = result.report.parts[static_cast<size_t>(lightest_pi)];
        auto& part = working.parts[static_cast<size_t>(lightest_pi)];

        // Tally any remaining notes in this part as dropped.
        if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
            for (const auto& n : part.poly_track.notes) {
                const int32_t end_tick = n.start_tick + n.total_ticks;
                if (pr.earliest_dropped_tick < 0 || n.start_tick < pr.earliest_dropped_tick) {
                    pr.earliest_dropped_tick = n.start_tick;
                }
                if (end_tick > pr.latest_dropped_tick) pr.latest_dropped_tick = end_tick;
                pr.notes_dropped += 1;
                result.report.total_notes_dropped += 1;
            }
            part.poly_track.notes.clear();
        } else {
            for (const auto& s : part.raw_track.spans) {
                if (s.relative_key < 0 || s.relative_key >= 12) continue;
                const int32_t end_tick = s.start_tick + s.total_ticks;
                if (pr.earliest_dropped_tick < 0 || s.start_tick < pr.earliest_dropped_tick) {
                    pr.earliest_dropped_tick = s.start_tick;
                }
                if (end_tick > pr.latest_dropped_tick) pr.latest_dropped_tick = end_tick;
                pr.notes_dropped += 1;
                result.report.total_notes_dropped += 1;
            }
            part.raw_track.spans.clear();
            part.raw_track.opcodes.clear();
        }
        pr.dropped_entirely = true;
        pr.final_lanes = 0;
        result.report.parts_dropped += 1;

        // Remove the part from the working doc so it produces no compiled track.
        working.parts.erase(working.parts.begin() + static_cast<std::ptrdiff_t>(lightest_pi));
        result.report.parts.erase(result.report.parts.begin() + static_cast<std::ptrdiff_t>(lightest_pi));
        compiled = compile_for_game(working);
    }

    // Prepend the synthesized control track 0. Targeted game exports are
    // finite/flattened; untargeted exports keep the legacy loop scaffold.
    // The engine requires it to drive the global playback rate.
    auto control_events = build_synthesized_control_track(working.initial_tempo, result.report.trim_tick_cap);
    compiled.smd.track_events.insert(
        compiled.smd.track_events.begin(), std::move(control_events));
    compiled.smd.track_count = static_cast<int32_t>(compiled.smd.track_events.size());

    // Stage D: loop rolling. Per-track post-pass that compresses
    // repeated bar-aligned phrases via REPEAT N + body + CODA pairs.
    // Engine support inside any track (including instrument tracks)
    // verified 2026-05-03 via the --smoke-loop gate. Runs AFTER
    // lane-packing (Stage A-prime) and AFTER the conductor track has
    // been prepended; rolling on a coalesced lane still finds repeats
    // because each source's contiguous block within the merged lane
    // preserves its internal repeat structure.
    {
        FFTSmdLoopRollerReport rr;
        compiled.smd = roll_loops_in_smd(compiled.smd, &rr);
        result.report.roll_report = std::move(rr);
    }

    result.report.final_track_count = compiled.smd.track_count;
    result.report.fits_track_budget = compiled.smd.track_count <= budget.max_tracks;

    std::string serialize_error;
    auto bytes = serialize_smd_file(compiled.smd, &serialize_error);
    result.report.encoded_bytes = bytes.size();
    const size_t byte_limit = normalized_budget.target_bytes > 0
        ? std::min(normalized_budget.max_bytes, normalized_budget.target_bytes)
        : normalized_budget.max_bytes;
    result.report.fits_byte_budget = !bytes.empty() && bytes.size() <= byte_limit;

    if (bytes.empty()) {
        result.ok = false;
        result.report.error = serialize_error.empty() ? std::string("SMD serialization failed") : serialize_error;
    } else if (!result.report.fits_track_budget) {
        result.ok = false;
        result.report.error = "Could not fit within " + std::to_string(budget.max_tracks) +
            " tracks (final " + std::to_string(compiled.smd.track_count) + ")";
    } else if (!result.report.fits_byte_budget) {
        result.ok = false;
        std::string heaviest_part;
        int32_t heaviest_lanes = 0;
        for (const auto& pr : result.report.parts) {
            if (pr.final_lanes > heaviest_lanes) {
                heaviest_lanes = pr.final_lanes;
                heaviest_part = pr.part_name;
            }
        }
        result.report.error = "Encoded size " + std::to_string(bytes.size()) +
            " bytes exceeds " + std::to_string(byte_limit) + " byte limit";
        if (!heaviest_part.empty()) {
            result.report.error += " (heaviest: '" + heaviest_part +
                "' with " + std::to_string(heaviest_lanes) + " lanes)";
        }
    } else {
        result.ok = true;
    }

    {
        const double headroom_pct = budget.shipped_max_bytes > 0
            ? 100.0 * static_cast<double>(bytes.size()) / static_cast<double>(budget.shipped_max_bytes)
            : 0.0;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%d tracks (was %d), %zu bytes (%.0f%% of largest stock %zu).",
            compiled.smd.track_count,
            result.report.pre_reduction_track_count,
            bytes.size(),
            headroom_pct,
            budget.shipped_max_bytes);
        result.report.summary = buf;
        if (result.report.total_notes_dropped > 0) {
            result.report.summary += " Dropped " +
                std::to_string(result.report.total_notes_dropped) + " note(s)";
            if (result.report.parts_dropped > 0) {
                result.report.summary += " and " +
                    std::to_string(result.report.parts_dropped) + " whole part(s)";
            }
            result.report.summary += ".";
        }
        if (result.report.trim_tick_cap >= 0) {
            char tbuf[160];
            std::snprintf(tbuf, sizeof(tbuf),
                " Trimmed song length to %d ticks (%d note(s) past the cap).",
                result.report.trim_tick_cap, result.report.total_notes_trimmed_by_length);
            result.report.summary += tbuf;
        }
        if (!result.ok && !result.report.error.empty()) {
            result.report.summary += " ERROR: " + result.report.error;
        }
    }

    result.compiled = std::move(compiled);
    return result;
}

}  // namespace fftplugin
