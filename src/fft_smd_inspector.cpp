#include "fft_plugin/fft_smd_inspector.h"
#include "fft_plugin/fft_smd_opcodes.h"
#include "fft_plugin/fft_smd_presentation_utils.h"
#include "fft_plugin/fft_smd_song_metadata_builder.h"

#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

namespace fftplugin {

namespace {

constexpr const char* kRelativeKeyNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

std::string note_name_for_relative_key(int32_t relative_key) {
    return smd_note_name_for_relative_key(relative_key);
}

int32_t event_tick_advance(const FFTSmdTrackEvent& event) {
    if (std::holds_alternative<FFTSmdNoteEvent>(event)) {
        return std::get<FFTSmdNoteEvent>(event).delta_time;
    }

    const FFTSmdOpcodeEvent& opcode = std::get<FFTSmdOpcodeEvent>(event);
    if (fftplugin::is_time_only_opcode(opcode.opcode) && !opcode.params.empty()) {
        return opcode.params[0];
    }
    return 0;
}

std::string describe_note(const FFTSmdNoteEvent& note) {
    std::ostringstream out;
    if (note.relative_key >= 0 && note.relative_key < 12) {
        out << "Note " << kRelativeKeyNames[note.relative_key];
    } else if (note.relative_key == 12) {
        out << "Tie";
    } else if (note.relative_key == 13) {
        out << "RestNote";
    } else {
        out << "Note rel=" << note.relative_key;
    }
    out << " vel=" << note.velocity << " dt=" << note.delta_time;
    return out.str();
}

std::string format_params(const std::vector<int32_t>& params) {
    std::ostringstream out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << params[i];
    }
    return out.str();
}

int32_t signed_byte_label_value(int32_t value) {
    return value >= 128 ? value - 256 : value;
}

std::string describe_opcode(const FFTSmdOpcodeEvent& opcode) {
    std::ostringstream out;
    out << smd_opcode_name(static_cast<uint8_t>(opcode.opcode));

    if (opcode.opcode == FFTSmdOpcode::REST && !opcode.params.empty()) {
        out << " wait=" << opcode.params[0];
    } else if (opcode.opcode == FFTSmdOpcode::FERMATA && !opcode.params.empty()) {
        out << " extend=" << opcode.params[0];
    } else if (opcode.opcode == FFTSmdOpcode::TEMPO && !opcode.params.empty()) {
        out << " value=" << opcode.params[0] << " (~" << std::fixed << std::setprecision(2)
            << fft_tempo_to_bpm(opcode.params[0]) << " bpm)";
    } else if (opcode.opcode == FFTSmdOpcode::INSTRUMENT && !opcode.params.empty()) {
        out << " sample=" << smd_instrument_opcode_param_to_played_sample_id(opcode.params[0]);
    } else if (opcode.opcode == FFTSmdOpcode::BANK_SELECT && !opcode.params.empty()) {
        out << " value=" << opcode.params[0];
    } else if (!opcode.params.empty()) {
        out << " [" << format_params(opcode.params) << "]";
    }

    out << " (0x" << std::uppercase << std::hex << opcode.opcode << std::dec << ")";
    return out.str();
}

bool is_structure_opcode(int32_t opcode) {
    return smd_is_structure_opcode(opcode);
}

bool is_time_only_opcode(int32_t opcode) {
    return smd_is_time_only_opcode(opcode);
}

bool is_tempo_opcode(int32_t opcode) {
    return smd_is_tempo_opcode(opcode);
}

bool is_time_signature_opcode(int32_t opcode) {
    return smd_is_time_signature_opcode(opcode);
}

std::string short_opcode_label(const FFTSmdOpcodeEvent& opcode) {
    return smd_short_opcode_label(opcode);
}

FFTSmdLaneCommandBlock build_note_command(
    int32_t tick,
    int32_t sequence_index,
    int32_t source_event_index,
    const FFTSmdNoteEvent& note
) {
    return smd_build_note_command(tick, sequence_index, source_event_index, note);
}

FFTSmdLaneInsertAnchor build_insert_anchor(
    int32_t tick,
    int32_t insertion_sequence_index,
    int32_t source_event_index,
    int32_t opcode,
    FFTSmdLaneInsertAnchorKind kind,
    std::string label
) {
    return FFTSmdLaneInsertAnchor {
        .tick = tick,
        .insertion_sequence_index = insertion_sequence_index,
        .source_event_index = source_event_index,
        .opcode = opcode,
        .kind = kind,
        .label = std::move(label),
    };
}

FFTSmdLaneCommandBlock build_opcode_command(
    int32_t tick,
    int32_t sequence_index,
    int32_t source_event_index,
    const FFTSmdOpcodeEvent& opcode,
    bool enabled
) {
    return smd_build_opcode_command(
        tick,
        sequence_index,
        source_event_index,
        -1,
        opcode,
        enabled);
}

struct NoteDisplayInfo {
    int32_t duration = 1;
    bool has_fermata = false;
    int32_t fermata_extension_ticks = 0;
};

NoteDisplayInfo note_display_info(
    const std::vector<FFTSmdTrackEvent>& track,
    size_t note_index,
    const FFTSmdNoteEvent& note
) {
    int32_t sustain = note.delta_time;
    bool has_fermata = false;
    for (size_t i = note_index + 1; i < track.size(); ++i) {
        if (!std::holds_alternative<FFTSmdOpcodeEvent>(track[i])) {
            break;
        }
        const FFTSmdOpcodeEvent& opcode = std::get<FFTSmdOpcodeEvent>(track[i]);
        if (opcode.opcode == FFTSmdOpcode::FERMATA) {
            sustain += !opcode.params.empty() ? opcode.params[0] : 0;
            has_fermata = true;
            continue;
        }
        if (opcode.opcode == FFTSmdOpcode::REST) {
            continue;
        }
        continue;
    }
    return NoteDisplayInfo {
        .duration = std::max(sustain, 1),
        .has_fermata = has_fermata,
        .fermata_extension_ticks = std::max(0, sustain - note.delta_time),
    };
}

void maybe_add_rest_block(
    FFTSmdTrackLanePresentation& track_presentation,
    int32_t tick,
    int32_t duration_ticks,
    int32_t source_event_index
) {
    if (duration_ticks <= 0) {
        return;
    }
    track_presentation.notes.push_back(FFTSmdLaneNoteBlock {
        .start_tick = tick,
        .duration_ticks = duration_ticks,
        .relative_key = 13,
        .source_event_index = source_event_index,
    });
}

struct PlaybackLoopEntry {
    int32_t repeat_start_idx = 0;
    int32_t remaining = 0;
};

FFTSmdTrackLanePresentation build_source_track_presentation(
    const std::vector<FFTSmdTrackEvent>& track,
    const FFTSmdTrackSummary& summary,
    int32_t track_index,
    std::vector<FFTSmdLaneMarker>* conductor_markers,
    std::vector<std::pair<int32_t, std::pair<int32_t, int32_t>>>* time_signature_changes,
    std::vector<std::pair<int32_t, int32_t>>* tempo_changes,
    const std::function<bool(int32_t, int32_t)>& is_source_event_disabled
) {
    FFTSmdTrackLanePresentation track_presentation;
    track_presentation.track_index = track_index;
    track_presentation.summary = summary;
    track_presentation.insert_anchors.push_back(build_insert_anchor(
        0,
        0,
        -1,
        -1,
        FFTSmdLaneInsertAnchorKind::track_start,
        "Track Start"));

    int32_t tick = 0;
    for (size_t event_index = 0; event_index < track.size(); ++event_index) {
        const FFTSmdTrackEvent& event = track[event_index];
        if (std::holds_alternative<FFTSmdNoteEvent>(event)) {
            const FFTSmdNoteEvent& note = std::get<FFTSmdNoteEvent>(event);
            track_presentation.insert_anchors.push_back(build_insert_anchor(
                tick,
                static_cast<int32_t>(event_index),
                static_cast<int32_t>(event_index),
                -1,
                note.relative_key == 13 ? FFTSmdLaneInsertAnchorKind::rest_start : FFTSmdLaneInsertAnchorKind::note_start,
                note.relative_key == 13 ? "Rest Start" : ("Note " + note_name_for_relative_key(note.relative_key))));
            track_presentation.commands.push_back(build_note_command(
                tick,
                static_cast<int32_t>(event_index),
                static_cast<int32_t>(event_index),
                note));
            if (note.relative_key >= 0 && note.relative_key < 12) {
                const NoteDisplayInfo display = note_display_info(track, event_index, note);
                track_presentation.notes.push_back(FFTSmdLaneNoteBlock {
                    .start_tick = tick,
                    .duration_ticks = display.duration,
                    .relative_key = note.relative_key,
                    .has_fermata = display.has_fermata,
                    .fermata_extension_ticks = display.fermata_extension_ticks,
                    .source_event_index = static_cast<int32_t>(event_index),
                });
            } else if (note.relative_key == 13) {
                maybe_add_rest_block(
                    track_presentation,
                    tick,
                    std::max(note.delta_time, 1),
                    static_cast<int32_t>(event_index));
            }
        } else {
            const FFTSmdOpcodeEvent& opcode = std::get<FFTSmdOpcodeEvent>(event);
            const bool enabled =
                !(is_source_event_disabled && is_source_event_disabled(track_index, static_cast<int32_t>(event_index)));
            if (!is_time_only_opcode(opcode.opcode)) {
                track_presentation.insert_anchors.push_back(build_insert_anchor(
                    tick,
                    static_cast<int32_t>(event_index),
                    static_cast<int32_t>(event_index),
                    opcode.opcode,
                    FFTSmdLaneInsertAnchorKind::command,
                    "Before " + short_opcode_label(opcode)));
            }
            track_presentation.commands.push_back(build_opcode_command(
                tick,
                static_cast<int32_t>(event_index),
                static_cast<int32_t>(event_index),
                opcode,
                enabled));
            if (opcode.opcode == FFTSmdOpcode::REST && !opcode.params.empty()) {
                maybe_add_rest_block(
                    track_presentation,
                    tick,
                    opcode.params[0],
                    static_cast<int32_t>(event_index));
            }
            if (opcode.opcode == FFTSmdOpcode::TIME_SIGNATURE && opcode.params.size() >= 2 && time_signature_changes != nullptr) {
                time_signature_changes->push_back({tick, {opcode.params[0], opcode.params[1]}});
            }
            if (opcode.opcode == FFTSmdOpcode::TEMPO && !opcode.params.empty() && tempo_changes != nullptr) {
                tempo_changes->push_back({tick, opcode.params[0]});
            }
            if (!is_time_only_opcode(opcode.opcode)) {
                const FFTSmdLaneMarker marker {
                    .tick = tick,
                    .kind = is_tempo_opcode(opcode.opcode)
                        ? FFTSmdLaneMarkerKind::tempo
                        : (is_structure_opcode(opcode.opcode)
                            ? FFTSmdLaneMarkerKind::structure
                            : FFTSmdLaneMarkerKind::opcode),
                    .label = short_opcode_label(opcode),
                };
                track_presentation.markers.push_back(marker);
                if (marker.kind == FFTSmdLaneMarkerKind::tempo && conductor_markers != nullptr) {
                    conductor_markers->push_back(marker);
                }
            }
        }
        tick += event_tick_advance(event);
    }

    track_presentation.total_ticks = tick;
    return track_presentation;
}

void append_measure_insert_anchors(
    FFTSmdTrackLanePresentation& track_presentation,
    const std::vector<FFTSmdTrackEvent>& track,
    const std::vector<FFTSmdGridSegment>& grid_segments
) {
    if (grid_segments.empty()) {
        return;
    }

    std::vector<int32_t> source_ticks;
    source_ticks.reserve(track.size());
    int32_t tick = 0;
    for (const auto& event : track) {
        source_ticks.push_back(tick);
        tick += event_tick_advance(event);
    }

    int32_t bar_number = 1;
    for (const auto& segment : grid_segments) {
        for (int32_t bar_tick = segment.start_tick; bar_tick < segment.end_tick; bar_tick += segment.ticks_per_bar) {
            if (bar_tick <= 0) {
                bar_number += 1;
                continue;
            }
            const auto insertion_it = std::lower_bound(source_ticks.begin(), source_ticks.end(), bar_tick);
            const int32_t insertion_index = static_cast<int32_t>(std::distance(source_ticks.begin(), insertion_it));
            track_presentation.insert_anchors.push_back(build_insert_anchor(
                bar_tick,
                insertion_index,
                -1,
                -1,
                FFTSmdLaneInsertAnchorKind::measure,
                "Bar " + std::to_string(bar_number)));
            bar_number += 1;
        }
    }
}

FFTSmdTrackLanePresentation build_playback_track_presentation(
    const std::vector<FFTSmdTrackEvent>& track,
    const FFTSmdTrackSummary& summary,
    int32_t track_index,
    std::vector<FFTSmdLaneMarker>* conductor_markers
) {
    constexpr int32_t kMaxPlaybackTicks = 16384;
    constexpr int32_t kMaxTrackLoopPasses = 4;
    constexpr int32_t kMaxVisitedEvents = 4096;

    FFTSmdTrackLanePresentation track_presentation;
    track_presentation.track_index = track_index;
    track_presentation.summary = summary;

    int32_t tick = 0;
    int32_t event_idx = 0;
    int32_t loop_point = -1;
    int32_t track_loop_passes = 0;
    int32_t visited_events = 0;
    std::vector<PlaybackLoopEntry> repeat_stack;

    while (visited_events < kMaxVisitedEvents && tick <= kMaxPlaybackTicks) {
        if (event_idx < 0 || event_idx >= static_cast<int32_t>(track.size())) {
            if (loop_point >= 0 && track_loop_passes < kMaxTrackLoopPasses) {
                track_loop_passes += 1;
                track_presentation.markers.push_back(FFTSmdLaneMarker {
                    .tick = tick,
                    .kind = FFTSmdLaneMarkerKind::structure,
                    .label = "LoopTrk",
                });
                event_idx = loop_point + 1;
                continue;
            }
            break;
        }

        const FFTSmdTrackEvent& event = track[static_cast<size_t>(event_idx)];
        visited_events += 1;

        if (std::holds_alternative<FFTSmdNoteEvent>(event)) {
            const FFTSmdNoteEvent& note = std::get<FFTSmdNoteEvent>(event);
            if (note.relative_key >= 0 && note.relative_key < 12) {
                const NoteDisplayInfo display = note_display_info(track, static_cast<size_t>(event_idx), note);
                track_presentation.notes.push_back(FFTSmdLaneNoteBlock {
                    .start_tick = tick,
                    .duration_ticks = display.duration,
                    .relative_key = note.relative_key,
                    .has_fermata = display.has_fermata,
                    .fermata_extension_ticks = display.fermata_extension_ticks,
                    .source_event_index = event_idx,
                });
            }
            tick += note.delta_time;
            event_idx += 1;
            continue;
        }

        const FFTSmdOpcodeEvent& opcode = std::get<FFTSmdOpcodeEvent>(event);
        const int32_t op = opcode.opcode;
        const auto add_structure_marker = [&](const std::string& label) {
            track_presentation.markers.push_back(FFTSmdLaneMarker {
                .tick = tick,
                .kind = FFTSmdLaneMarkerKind::structure,
                .label = label,
            });
        };

        if (fftplugin::is_time_only_opcode(op)) {
            tick += !opcode.params.empty() ? opcode.params[0] : 0;
            event_idx += 1;
            continue;
        }

        if (is_tempo_opcode(op)) {
            const FFTSmdLaneMarker marker {
                .tick = tick,
                .kind = FFTSmdLaneMarkerKind::tempo,
                .label = short_opcode_label(opcode),
            };
            if (conductor_markers != nullptr) {
                conductor_markers->push_back(marker);
            }
            event_idx += 1;
            continue;
        }

        if (op == FFTSmdOpcode::LOOP) {
            loop_point = event_idx;
            add_structure_marker("LoopTrk");
            event_idx += 1;
            continue;
        }
        if (op == FFTSmdOpcode::REPEAT) {
            const int32_t count = !opcode.params.empty() ? (opcode.params[0] - 1) : 0;
            repeat_stack.push_back(PlaybackLoopEntry {.repeat_start_idx = event_idx + 1, .remaining = count});
            add_structure_marker("RptStart");
            event_idx += 1;
            continue;
        }
        if (op == FFTSmdOpcode::CODA) {
            add_structure_marker("RptEnd");
            if (!repeat_stack.empty()) {
                PlaybackLoopEntry& entry = repeat_stack.back();
                if (entry.remaining > 0) {
                    entry.remaining -= 1;
                    event_idx = entry.repeat_start_idx;
                    continue;
                }
                repeat_stack.pop_back();
            }
            event_idx += 1;
            continue;
        }
        if (op == FFTSmdOpcode::REPEAT_BREAK) {
            add_structure_marker("RptBreak");
            if (!repeat_stack.empty() && repeat_stack.back().remaining == 0) {
                int32_t depth = 1;
                int32_t search = event_idx + 1;
                while (search < static_cast<int32_t>(track.size()) && depth > 0) {
                    const FFTSmdTrackEvent& search_event = track[static_cast<size_t>(search)];
                    if (std::holds_alternative<FFTSmdOpcodeEvent>(search_event)) {
                        const FFTSmdOpcodeEvent& search_opcode = std::get<FFTSmdOpcodeEvent>(search_event);
                        if (search_opcode.opcode == FFTSmdOpcode::REPEAT) {
                            depth += 1;
                        } else if (search_opcode.opcode == FFTSmdOpcode::CODA) {
                            depth -= 1;
                        }
                    }
                    search += 1;
                }
                repeat_stack.pop_back();
                event_idx = search;
                continue;
            }
            event_idx += 1;
            continue;
        }
        if (op == FFTSmdOpcode::END_BAR) {
            add_structure_marker("End");
            if (loop_point >= 0 && track_loop_passes < kMaxTrackLoopPasses) {
                track_loop_passes += 1;
                event_idx = loop_point + 1;
                continue;
            }
            break;
        }

        event_idx += 1;
    }

    track_presentation.total_ticks = tick;
    return track_presentation;
}

}  // namespace

std::vector<FFTSmdTrackSummary> build_smd_track_summaries(const FFTSmdFile& smd) {
    std::vector<FFTSmdTrackSummary> summaries;
    summaries.reserve(smd.track_events.size());
    for (size_t track_index = 0; track_index < smd.track_events.size(); ++track_index) {
        FFTSmdTrackSummary summary;
        summary.track_index = static_cast<int32_t>(track_index);
        summary.event_count = static_cast<int32_t>(smd.track_events[track_index].size());
        for (const FFTSmdTrackEvent& event : smd.track_events[track_index]) {
            if (std::holds_alternative<FFTSmdNoteEvent>(event)) {
                summary.note_count += 1;
            } else {
                summary.opcode_count += 1;
            }
        }
        summaries.push_back(summary);
    }
    return summaries;
}

std::string build_smd_metadata_text(const FFTSmdFile& smd) {
    std::ostringstream out;
    out << "Title: " << (smd.song_title.empty() ? "(untitled)" : smd.song_title) << "\n";
    out << "Track Count: " << smd.track_count << "\n";
    out << "Assoc WDS ID: " << (smd.assoc_wds_id > 0 ? std::to_string(smd.assoc_wds_id) : std::string("n/a")) << "\n";
    return out.str();
}

std::string build_smd_track_summary_text(const FFTSmdFile& smd) {
    const std::vector<FFTSmdTrackSummary> summaries = build_smd_track_summaries(smd);
    std::ostringstream out;
    out << "Track  Events  Notes  Opcodes\n";
    out << "-----  ------  -----  -------\n";
    for (const FFTSmdTrackSummary& summary : summaries) {
        out << std::setw(5) << summary.track_index << "  "
            << std::setw(6) << summary.event_count << "  "
            << std::setw(5) << summary.note_count << "  "
            << std::setw(7) << summary.opcode_count << "\n";
    }
    return out.str();
}

std::string build_smd_track_events_text(const FFTSmdFile& smd, int32_t track_index) {
    if (track_index < 0 || static_cast<size_t>(track_index) >= smd.track_events.size()) {
        return "No track selected.\n";
    }

    const std::vector<FFTSmdTrackEvent>& track = smd.track_events[static_cast<size_t>(track_index)];
    std::ostringstream out;
    out << "#     Tick    Kind  Description\n";
    out << "----  ------  ----  -----------\n";

    int32_t tick = 0;
    for (size_t event_index = 0; event_index < track.size(); ++event_index) {
        const FFTSmdTrackEvent& event = track[event_index];
        out << std::setw(4) << event_index << "  "
            << std::setw(6) << tick << "  ";

        if (std::holds_alternative<FFTSmdNoteEvent>(event)) {
            out << "NOTE  " << describe_note(std::get<FFTSmdNoteEvent>(event));
        } else {
            out << "OP    " << describe_opcode(std::get<FFTSmdOpcodeEvent>(event));
        }
        out << "\n";
        tick += event_tick_advance(event);
    }
    return out.str();
}

FFTSmdSongPresentation build_smd_song_presentation(
    const FFTSmdFile& smd,
    FFTSmdPresentationMode mode,
    std::function<bool(int32_t, int32_t)> is_source_event_disabled
) {
    FFTSmdSongPresentation presentation;
    const std::vector<FFTSmdTrackSummary> summaries = build_smd_track_summaries(smd);
    presentation.tracks.reserve(smd.track_events.size());
    std::vector<std::pair<int32_t, std::pair<int32_t, int32_t>>> conductor_time_sigs;
    std::vector<std::pair<int32_t, int32_t>> conductor_tempos;
    for (size_t track_index = 0; track_index < smd.track_events.size(); ++track_index) {
        const std::vector<FFTSmdTrackEvent>& track = smd.track_events[track_index];
        FFTSmdTrackLanePresentation track_presentation =
            mode == FFTSmdPresentationMode::playback
            ? build_playback_track_presentation(
                  track,
                  summaries[track_index],
                  static_cast<int32_t>(track_index),
                  &presentation.conductor_markers)
            : build_source_track_presentation(
                  track,
                  summaries[track_index],
                  static_cast<int32_t>(track_index),
                  &presentation.conductor_markers,
                  track_index == 0 ? &conductor_time_sigs : nullptr,
                  track_index == 0 ? &conductor_tempos : nullptr,
                  is_source_event_disabled);

        presentation.total_ticks = std::max(presentation.total_ticks, track_presentation.total_ticks);
        if (track_index == 0) {
            presentation.conductor_markers = track_presentation.markers;
        }
        presentation.tracks.push_back(std::move(track_presentation));
    }

    if (presentation.conductor_markers.empty()) {
        presentation.conductor_markers.push_back(FFTSmdLaneMarker {
            .tick = 0,
            .kind = FFTSmdLaneMarkerKind::tempo,
            .label = "T" + std::to_string(smd.initial_tempo),
        });
    }

    presentation.grid_segments = build_grid_segments_from_time_signatures(
        conductor_time_sigs,
        presentation.total_ticks);
    presentation.second_markers = build_second_markers_from_tempo_changes(
        conductor_tempos,
        presentation.total_ticks,
        smd.initial_tempo);

    if (mode == FFTSmdPresentationMode::source) {
        for (size_t track_index = 0; track_index < presentation.tracks.size(); ++track_index) {
            append_measure_insert_anchors(
                presentation.tracks[track_index],
                smd.track_events[track_index],
                presentation.grid_segments);
        }
    }

    return presentation;
}

}  // namespace fftplugin
