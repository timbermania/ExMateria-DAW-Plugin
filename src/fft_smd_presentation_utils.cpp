#include "fft_plugin/fft_smd_presentation_utils.h"

#include <algorithm>

#include "fft_plugin/fft_smd_opcodes.h"

namespace fftplugin {

namespace {

constexpr const char* kRelativeKeyNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

}  // namespace

std::string smd_note_name_for_relative_key(int32_t relative_key) {
    if (relative_key >= 0 && relative_key < 12) {
        return kRelativeKeyNames[relative_key];
    }
    if (relative_key == 13) {
        return "Rest";
    }
    return "Note";
}

int32_t smd_instrument_opcode_param_to_played_sample_id(int32_t instrument_param) {
    if (instrument_param >= 255) {
        return 255;
    }
    if (instrument_param < 0) {
        return instrument_param;
    }
    return instrument_param + 1;
}

int32_t smd_played_sample_id_to_instrument_opcode_param(int32_t played_sample_id) {
    if (played_sample_id >= 255) {
        return 255;
    }
    if (played_sample_id < 0) {
        return played_sample_id;
    }
    return std::max(0, played_sample_id - 1);
}

std::string smd_format_instrument_label_from_opcode_param(int32_t instrument_param) {
    if (instrument_param >= 255) {
        return "Inst 255";
    }
    const int32_t played_sample = smd_instrument_opcode_param_to_played_sample_id(instrument_param);
    return "Inst " + std::to_string(played_sample);
}

bool smd_is_structure_opcode(int32_t opcode) {
    return fftplugin::is_structure_opcode(opcode);
}

bool smd_is_time_only_opcode(int32_t opcode) {
    return fftplugin::is_time_only_opcode(opcode);
}

bool smd_is_tempo_opcode(int32_t opcode) {
    return fftplugin::is_tempo_opcode(opcode);
}

bool smd_is_time_signature_opcode(int32_t opcode) {
    return fftplugin::is_time_signature_opcode(opcode);
}

std::string smd_short_opcode_label(const FFTSmdOpcodeEvent& opcode) {
    switch (opcode.opcode) {
    case 0x90: return "End";
    case 0x91: return "LoopTrk";
    case 0x98:
        return opcode.params.empty() ? "RptStart" : ("RptStart " + std::to_string(opcode.params[0]));
    case 0x99: return "RptEnd";
    case 0x9A: return "RptBreak";
    case 0x97:
        if (opcode.params.size() >= 2) {
            return "TimeSig " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "TimeSig 4/4";
    case 0x94:
        return opcode.params.empty() ? "Oct" : ("Oct " + std::to_string(opcode.params[0]));
    case 0x95: return "RaiseO";
    case 0x96: return "LowerO";
    case 0xA0:
        return opcode.params.empty() ? "Tempo" : ("Tempo " + std::to_string(opcode.params[0]));
    case 0xA2:
        if (opcode.params.size() >= 2) {
            return "TmpSl " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "TmpSl";
    case 0xA3:
        if (opcode.params.size() >= 2) {
            return "A3 " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "A3";
    case 0xA4:
        return opcode.params.empty() ? "A4" : ("A4 " + std::to_string(opcode.params[0]));
    case 0xA9:
        return opcode.params.empty() ? "A9" : ("A9 " + std::to_string(opcode.params[0]));
    case 0xAC:
        return opcode.params.empty() ? "Inst" : smd_format_instrument_label_from_opcode_param(opcode.params[0]);
    case 0xAD:
        return opcode.params.empty() ? "AD?" : ("AD? " + std::to_string(opcode.params[0]));
    case 0xB9:
        return opcode.params.empty() ? "B9" : ("B9 " + std::to_string(opcode.params[0]));
    case 0xC1:
        return opcode.params.empty() ? "C1" : ("C1 " + std::to_string(opcode.params[0]));
    case 0xC2:
        return opcode.params.empty() ? "Atk" : ("Atk " + std::to_string(opcode.params[0]));
    case 0xC4:
        return opcode.params.empty() ? "SusRt" : ("SusRt " + std::to_string(opcode.params[0]));
    case 0xC5:
        return opcode.params.empty() ? "Rel" : ("Rel " + std::to_string(opcode.params[0]));
    case 0xC6:
        return opcode.params.empty() ? "Slide" : ("Slide " + std::to_string(opcode.params[0]));
    case 0xC7:
        if (opcode.params.size() >= 2) {
            return "Dec/Sus " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "Dec/Sus";
    case 0xC8:
        return opcode.params.empty() ? "C8" : ("C8 " + std::to_string(opcode.params[0]));
    case 0xC9:
        return opcode.params.empty() ? "Dec" : ("Dec " + std::to_string(opcode.params[0]));
    case 0xCA:
        return opcode.params.empty() ? "SusLv" : ("SusLv " + std::to_string(opcode.params[0]));
    case 0xD0:
        return opcode.params.empty() ? "Bend" : ("Bend " + std::to_string(opcode.params[0]));
    case 0xD1:
        return opcode.params.empty() ? "Bend+" : ("Bend+ " + std::to_string(opcode.params[0]));
    case 0xD2:
        return opcode.params.empty() ? "Cond" : ("Cond " + std::to_string(opcode.params[0]));
    case 0xD4:
        if (opcode.params.size() >= 2) {
            return "Port " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "Port";
    case 0xD6:
        return opcode.params.empty() ? "Detune" : ("Detune " + std::to_string(opcode.params[0]));
    case 0xD7:
        return opcode.params.empty() ? "LFO" : ("LFO " + std::to_string(opcode.params[0]));
    case 0xD8:
        if (opcode.params.size() >= 2) {
            return "LFOlen " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "LFOlen";
    case 0xDB:
        return opcode.params.empty() ? "Dyn" : ("Dyn " + std::to_string(opcode.params[0]));
    case 0xDC:
        return opcode.params.empty() ? "Dyn+" : ("Dyn+ " + std::to_string(opcode.params[0]));
    case 0xDD:
        if (opcode.params.size() >= 2) {
            return "Expr " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "Expr";
    case 0xDE:
        return opcode.params.empty() ? "VolLFO" : ("VolLFO " + std::to_string(opcode.params[0]));
    case 0xDF:
        if (opcode.params.size() >= 2) {
            return "VolLFOlen " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "VolLFOlen";
    case 0xE8:
        return opcode.params.empty() ? "Pan" : ("Pan " + std::to_string(opcode.params[0]));
    case 0xE9:
        return opcode.params.empty() ? "Pan?" : ("Pan? " + std::to_string(opcode.params[0]));
    case 0xEA:
        if (opcode.params.size() >= 2) {
            return "PanSl " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "PanSl";
    case 0xEB:
        return opcode.params.empty() ? "PanLFO" : ("PanLFO " + std::to_string(opcode.params[0]));
    case 0xEC:
        if (opcode.params.size() >= 2) {
            return "PanLFOLn " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "PanLFOLn";
    case 0xF4:
        return opcode.params.empty() ? "F4" : ("F4 " + std::to_string(opcode.params[0]));
    case 0xF7:
        return opcode.params.empty() ? "F7" : ("F7 " + std::to_string(opcode.params[0]));
    case 0xF8:
        if (opcode.params.size() >= 2) {
            return "F8 " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "F8";
    case 0xF9:
        if (opcode.params.size() >= 2) {
            return "F9 " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "F9";
    case 0xFB:
        return opcode.params.empty() ? "FB" : ("FB " + std::to_string(opcode.params[0]));
    case 0xFC:
        if (opcode.params.size() >= 2) {
            return "FC " + std::to_string(opcode.params[0]) + "/" + std::to_string(opcode.params[1]);
        }
        return "FC";
    case 0xFD:
        return opcode.params.empty() ? "FD" : ("FD " + std::to_string(opcode.params[0]));
    case 0xFE:
        return opcode.params.empty() ? "Bank" : ("Bank " + std::to_string(opcode.params[0]));
    default: {
        std::string name = smd_opcode_name(static_cast<uint8_t>(opcode.opcode));
        if (name.size() > 6) {
            name.resize(6);
        }
        return name;
    }
    }
}

std::vector<FFTVisibleAuthoredTickAnchor> smd_build_visible_authored_tick_anchors(
    const FFTSmdTrackLanePresentation& track
) {
    std::vector<FFTVisibleAuthoredTickAnchor> anchors;
    anchors.reserve(
        track.notes.size() * 2U +
        track.commands.size() +
        track.loop_boundaries.size());

    auto append_anchor = [&anchors](int32_t visible_tick, int32_t authored_tick) {
        if (authored_tick < 0) {
            return;
        }
        anchors.push_back(FFTVisibleAuthoredTickAnchor {
            .visible_tick = visible_tick,
            .authored_tick = authored_tick,
        });
    };

    for (const auto& boundary : track.loop_boundaries) {
        append_anchor(boundary.tick, boundary.authored_tick);
    }
    for (const auto& command : track.commands) {
        append_anchor(command.tick, command.authored_tick);
    }
    for (const auto& note : track.notes) {
        if (note.relative_key == 13 && note.source_event_index < 0) {
            continue;
        }
        append_anchor(note.start_tick, note.authored_start_tick);
        if (note.authored_start_tick >= 0 && note.duration_ticks > 0) {
            append_anchor(
                note.start_tick + note.duration_ticks,
                note.authored_start_tick + note.duration_ticks);
        }
    }

    std::stable_sort(
        anchors.begin(),
        anchors.end(),
        [](const FFTVisibleAuthoredTickAnchor& lhs, const FFTVisibleAuthoredTickAnchor& rhs) {
            if (lhs.visible_tick != rhs.visible_tick) {
                return lhs.visible_tick < rhs.visible_tick;
            }
            return lhs.authored_tick < rhs.authored_tick;
        });
    anchors.erase(
        std::unique(
            anchors.begin(),
            anchors.end(),
            [](const FFTVisibleAuthoredTickAnchor& lhs, const FFTVisibleAuthoredTickAnchor& rhs) {
                return lhs.visible_tick == rhs.visible_tick && lhs.authored_tick == rhs.authored_tick;
            }),
        anchors.end());
    return anchors;
}

int32_t smd_map_visible_tick_to_authored_tick(
    const FFTSmdTrackLanePresentation& track,
    int32_t visible_tick
) {
    const auto* segment = smd_find_visible_time_map_segment(track, visible_tick);
    if (segment == nullptr || segment->authored_start_tick < 0) {
        return visible_tick;
    }
    return segment->authored_start_tick + (visible_tick - segment->start_tick);
}

const FFTSmdLaneTimeMapSegment* smd_find_visible_time_map_segment(
    const FFTSmdTrackLanePresentation& track,
    int32_t visible_tick
) {
    const FFTSmdLaneTimeMapSegment* best = nullptr;
    for (const auto& segment : track.time_map_segments) {
        if (segment.authored_start_tick < 0 || segment.duration_ticks <= 0) {
            continue;
        }
        const int32_t end_tick = segment.start_tick + segment.duration_ticks;
        const bool contains_tick =
            (visible_tick >= segment.start_tick && visible_tick < end_tick) ||
            visible_tick == segment.start_tick;
        if (!contains_tick) {
            continue;
        }
        if (best == nullptr ||
            segment.occurrence_count > best->occurrence_count ||
            (segment.occurrence_count == best->occurrence_count &&
             segment.duration_ticks > best->duration_ticks) ||
            (segment.occurrence_count == best->occurrence_count &&
             segment.duration_ticks == best->duration_ticks &&
             segment.start_tick > best->start_tick)) {
            best = &segment;
        }
    }
    return best;
}

FFTSmdLaneCommandKind smd_command_kind_for_opcode(const FFTSmdOpcodeEvent& opcode) {
    if (opcode.opcode == 0x80) {
        return FFTSmdLaneCommandKind::rest;
    }
    if (opcode.opcode == 0x81) {
        return FFTSmdLaneCommandKind::hold;
    }
    if (smd_is_structure_opcode(opcode.opcode)) {
        return FFTSmdLaneCommandKind::structure;
    }
    if (smd_is_tempo_opcode(opcode.opcode) || smd_is_time_signature_opcode(opcode.opcode)) {
        return FFTSmdLaneCommandKind::tempo;
    }
    return FFTSmdLaneCommandKind::opcode;
}

FFTSmdLaneCommandBlock smd_build_note_command(
    int32_t tick,
    int32_t sequence_index,
    int32_t source_event_index,
    const FFTSmdNoteEvent& note
) {
    const bool is_rest = note.relative_key == 13;
    return FFTSmdLaneCommandBlock {
        .tick = tick,
        .duration_ticks = std::max(note.delta_time, 1),
        .sequence_index = sequence_index,
        .source_event_index = source_event_index,
        .opcode = -1,
        .kind = is_rest ? FFTSmdLaneCommandKind::rest : FFTSmdLaneCommandKind::note,
        .label = is_rest
            ? ("Rest " + std::to_string(std::max(note.delta_time, 1)))
            : smd_note_name_for_relative_key(note.relative_key),
        .enabled = true,
    };
}

FFTSmdLaneCommandBlock smd_build_opcode_command(
    int32_t tick,
    int32_t sequence_index,
    int32_t source_event_index,
    int32_t authored_opcode_index,
    const FFTSmdOpcodeEvent& opcode,
    bool enabled
) {
    std::string label = smd_short_opcode_label(opcode);
    if (opcode.opcode == 0x80 && !opcode.params.empty()) {
        label = "Rest " + std::to_string(opcode.params[0]);
    } else if (opcode.opcode == 0x81 && !opcode.params.empty()) {
        label = "Hold " + std::to_string(opcode.params[0]);
    }

    return FFTSmdLaneCommandBlock {
        .tick = tick,
        .authored_tick = tick,
        .authored_opcode_index = authored_opcode_index,
        .duration_ticks = smd_is_time_only_opcode(opcode.opcode) && !opcode.params.empty()
            ? std::max(opcode.params[0], 1)
            : 0,
        .opcode_params = opcode.params,
        .sequence_index = sequence_index,
        .source_event_index = source_event_index,
        .opcode = opcode.opcode,
        .kind = smd_command_kind_for_opcode(opcode),
        .label = std::move(label),
        .enabled = enabled,
    };
}

}  // namespace fftplugin
