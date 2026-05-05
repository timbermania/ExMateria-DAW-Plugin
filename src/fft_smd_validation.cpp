#include "fft_plugin/fft_smd_validation.h"
#include "fft_plugin/fft_instrument_catalog.h"
#include "fft_plugin/fft_smd_presentation_utils.h"

#include <algorithm>
#include <optional>

namespace fftplugin {

namespace {

bool is_control_flow_opcode(int32_t opcode) {
    switch (opcode) {
    case 0x90:
    case 0x91:
    case 0x98:
    case 0x99:
    case 0x9A:
        return true;
    default:
        return false;
    }
}

bool is_inst_255_opcode(const FFTSmdAuthoredOpcode& opcode) {
    return opcode.opcode.opcode == 0xAC &&
        !opcode.opcode.params.empty() &&
        opcode.opcode.params[0] == 255;
}

bool is_dyn_setup_opcode(int32_t opcode) {
    return opcode == 0xE0 || opcode == 0xE1 || opcode == 0xE2;
}

bool is_pan_setup_opcode(int32_t opcode) {
    return opcode == 0xE8 || opcode == 0xE9 || opcode == 0xEA ||
        opcode == 0xEB || opcode == 0xEC || opcode == 0xED;
}

bool is_reverb_setup_opcode(int32_t opcode) {
    return opcode == 0xBA || opcode == 0xBB;
}

bool is_octave_setup_opcode(int32_t opcode) {
    return opcode == 0x94 || opcode == 0x95 || opcode == 0x96;
}

struct IndexedOpcode {
    FFTSmdAuthoredOpcode opcode;
    size_t index = 0;
};

std::optional<int32_t> played_sample_root_midi_note(int32_t played_sample_id) {
    const FFTInstrumentCatalogEntry* entry = find_fft_instrument_catalog_entry(played_sample_id);
    if (entry == nullptr || entry->root_midi_note < 0) {
        return std::nullopt;
    }
    return entry->root_midi_note;
}

void add_diagnostic(
    std::vector<FFTSmdLaneDiagnostic>& diagnostics,
    int32_t tick,
    FFTSmdLaneDiagnosticSeverity severity,
    std::string short_label,
    std::string message
) {
    diagnostics.push_back(FFTSmdLaneDiagnostic {
        .tick = std::max(0, tick),
        .severity = severity,
        .short_label = std::move(short_label),
        .message = std::move(message),
    });
}

bool part_has_real_boundary_at(const FFTSmdAuthoringPart& part, int32_t tick) {
    if (tick <= 0 || tick >= (part.kind == FFTSmdAuthoringPartKind::poly_track ? part.poly_track.total_ticks : part.raw_track.total_ticks)) {
        return true;
    }
    if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
        return std::any_of(
            part.poly_track.notes.begin(),
            part.poly_track.notes.end(),
            [tick](const FFTSmdAuthoredPolyNote& note) {
                return note.start_tick == tick ||
                    tick == note.start_tick + note.base_ticks ||
                    tick == note.start_tick + note.total_ticks;
            });
    }
    return std::any_of(
        part.raw_track.spans.begin(),
        part.raw_track.spans.end(),
        [tick](const FFTSmdAuthoredSpan& span) {
            return span.start_tick == tick;
        });
}

std::vector<IndexedOpcode> sorted_part_opcodes(const FFTSmdAuthoringPart& part) {
    std::vector<IndexedOpcode> sorted;
    const auto& opcodes = part.kind == FFTSmdAuthoringPartKind::poly_track ? part.poly_track.opcodes : part.raw_track.opcodes;
    sorted.reserve(opcodes.size());
    for (size_t index = 0; index < opcodes.size(); ++index) {
        sorted.push_back(IndexedOpcode {
            .opcode = opcodes[index],
            .index = index,
        });
    }

    std::stable_sort(
        sorted.begin(),
        sorted.end(),
        [](const IndexedOpcode& lhs, const IndexedOpcode& rhs) {
            if (lhs.opcode.tick != rhs.opcode.tick) {
                return lhs.opcode.tick < rhs.opcode.tick;
            }
            if (lhs.opcode.stack_order != rhs.opcode.stack_order) {
                return lhs.opcode.stack_order < rhs.opcode.stack_order;
            }
            return lhs.index < rhs.index;
        });

    return sorted;
}

void validate_control_flow_placement(
    const FFTSmdAuthoringPart& part,
    const std::vector<IndexedOpcode>& opcodes,
    std::vector<FFTSmdLaneDiagnostic>& diagnostics
) {
    for (const auto& indexed_opcode : opcodes) {
        const int32_t opcode = indexed_opcode.opcode.opcode.opcode;
        if (!is_control_flow_opcode(opcode)) {
            continue;
        }
        if (!part_has_real_boundary_at(part, indexed_opcode.opcode.tick)) {
            add_diagnostic(
                diagnostics,
                indexed_opcode.opcode.tick,
                FFTSmdLaneDiagnosticSeverity::error,
                "Control Flow",
                "Control-flow opcode must be placed on a real time boundary.");
        }
    }
}

void validate_end_and_repeat_structure(
    const FFTSmdAuthoringPart& part,
    const std::vector<IndexedOpcode>& opcodes,
    std::vector<FFTSmdLaneDiagnostic>& diagnostics
) {
    const bool has_note_data = part.kind == FFTSmdAuthoringPartKind::poly_track
        ? !part.poly_track.notes.empty()
        : std::any_of(
            part.raw_track.spans.begin(),
            part.raw_track.spans.end(),
            [](const FFTSmdAuthoredSpan& span) {
                return span.relative_key >= 0 && span.relative_key < 12;
            });
    const int32_t total_ticks =
        part.kind == FFTSmdAuthoringPartKind::poly_track ? part.poly_track.total_ticks : part.raw_track.total_ticks;
    struct RepeatStart {
        int32_t tick = 0;
    };

    std::vector<RepeatStart> repeat_stack;
    std::optional<size_t> first_end_index;
    for (size_t index = 0; index < opcodes.size(); ++index) {
        const auto& indexed_opcode = opcodes[index];
        const int32_t opcode = indexed_opcode.opcode.opcode.opcode;
        switch (opcode) {
        case 0x90:
            if (!first_end_index.has_value()) {
                first_end_index = index;
            } else {
                add_diagnostic(
                    diagnostics,
                    indexed_opcode.opcode.tick,
                    FFTSmdLaneDiagnosticSeverity::error,
                    "Extra End",
                    "Track has multiple End opcodes.");
            }
            break;
        case 0x98:
            repeat_stack.push_back(RepeatStart {.tick = indexed_opcode.opcode.tick});
            break;
        case 0x99:
            if (repeat_stack.empty()) {
                add_diagnostic(
                    diagnostics,
                    indexed_opcode.opcode.tick,
                    FFTSmdLaneDiagnosticSeverity::error,
                    "RptEnd",
                    "RptEnd has no matching RptStart.");
            } else {
                repeat_stack.pop_back();
            }
            break;
        case 0x9A:
            if (repeat_stack.empty()) {
                add_diagnostic(
                    diagnostics,
                    indexed_opcode.opcode.tick,
                    FFTSmdLaneDiagnosticSeverity::error,
                    "RptBreak",
                    "RptBreak must be inside a repeat block.");
            }
            break;
        default:
            break;
        }
    }

    for (const auto& repeat_start : repeat_stack) {
        add_diagnostic(
            diagnostics,
            repeat_start.tick,
            FFTSmdLaneDiagnosticSeverity::error,
            "RptStart",
            "RptStart has no matching RptEnd.");
    }

    if (!first_end_index.has_value()) {
        add_diagnostic(
            diagnostics,
            total_ticks,
            FFTSmdLaneDiagnosticSeverity::error,
            "Missing End",
            "Track must end with an End opcode.");
        return;
    }

    const auto& first_end = opcodes[*first_end_index];
    if (first_end.opcode.tick < total_ticks) {
        add_diagnostic(
            diagnostics,
            first_end.opcode.tick,
            FFTSmdLaneDiagnosticSeverity::error,
            "End Early",
            "Track has time data after End.");
    }

    if (*first_end_index + 1 < opcodes.size()) {
        add_diagnostic(
            diagnostics,
            opcodes[*first_end_index + 1].opcode.tick,
            FFTSmdLaneDiagnosticSeverity::error,
            "After End",
            "Track has opcodes after End.");
    }

    if (has_note_data &&
        (*first_end_index == 0 || !is_inst_255_opcode(opcodes[*first_end_index - 1].opcode))) {
        add_diagnostic(
            diagnostics,
            first_end.opcode.tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "Inst255",
            "Track should place Inst 255 immediately before End.");
    }
}

void validate_pre_note_setup(
    const FFTSmdAuthoringPart& part,
    const std::vector<IndexedOpcode>& opcodes,
    std::vector<FFTSmdLaneDiagnostic>& diagnostics
) {
    int32_t first_note_tick = -1;
    if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
        if (part.poly_track.notes.empty()) {
            return;
        }
        first_note_tick = std::min_element(
            part.poly_track.notes.begin(),
            part.poly_track.notes.end(),
            [](const FFTSmdAuthoredPolyNote& lhs, const FFTSmdAuthoredPolyNote& rhs) {
                return lhs.start_tick < rhs.start_tick;
            })->start_tick;
    } else {
        const auto first_note_it = std::find_if(
            part.raw_track.spans.begin(),
            part.raw_track.spans.end(),
            [](const FFTSmdAuthoredSpan& span) {
                return span.relative_key >= 0 && span.relative_key < 12;
            });
        if (first_note_it == part.raw_track.spans.end()) {
            return;
        }
        first_note_tick = first_note_it->start_tick;
    }

    bool has_inst = false;
    bool has_dyn = false;
    bool has_pan = false;
    bool has_reverb = false;
    bool has_octave = false;
    for (const auto& indexed_opcode : opcodes) {
        if (indexed_opcode.opcode.tick > first_note_tick) {
            break;
        }

        const int32_t opcode = indexed_opcode.opcode.opcode.opcode;
        has_inst = has_inst || opcode == 0xAC;
        has_dyn = has_dyn || is_dyn_setup_opcode(opcode);
        has_pan = has_pan || is_pan_setup_opcode(opcode);
        has_reverb = has_reverb || is_reverb_setup_opcode(opcode);
        has_octave = has_octave || is_octave_setup_opcode(opcode);
    }

    const int32_t tick = first_note_tick;
    if (!has_inst) {
        add_diagnostic(
            diagnostics,
            tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "No Inst",
            "Track should set instrument before the first note.");
    }
    if (!has_dyn) {
        add_diagnostic(
            diagnostics,
            tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "No Dyn",
            "Track should set dynamics before the first note.");
    }
    if (!has_pan) {
        add_diagnostic(
            diagnostics,
            tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "No Pan",
            "Track should set pan before the first note.");
    }
    if (!has_reverb) {
        add_diagnostic(
            diagnostics,
            tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "No Rev",
            "Track should set reverb before the first note.");
    }
    if (!has_octave) {
        add_diagnostic(
            diagnostics,
            tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "No Oct",
            "Track should set octave before the first note.");
    }
}

void validate_instrument_root_octave_fit(
    const FFTSmdAuthoringPart& part,
    const std::vector<IndexedOpcode>& opcodes,
    std::vector<FFTSmdLaneDiagnostic>& diagnostics
) {
    struct TimedNote {
        int32_t tick = 0;
        int32_t relative_key = 0;
    };

    std::vector<TimedNote> notes;
    if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
        notes.reserve(part.poly_track.notes.size());
        for (const auto& note : part.poly_track.notes) {
            notes.push_back(TimedNote {.tick = note.start_tick, .relative_key = note.relative_key});
        }
    } else {
        for (const auto& span : part.raw_track.spans) {
            if (span.relative_key < 0 || span.relative_key >= 12) {
                continue;
            }
            notes.push_back(TimedNote {.tick = span.start_tick, .relative_key = span.relative_key});
        }
    }
    if (notes.empty()) {
        return;
    }

    std::stable_sort(
        notes.begin(),
        notes.end(),
        [](const TimedNote& lhs, const TimedNote& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.relative_key < rhs.relative_key;
        });

    int32_t current_instrument = -1;
    int32_t current_octave = -1;
    size_t opcode_index = 0;
    for (const auto& note : notes) {
        while (opcode_index < opcodes.size() && opcodes[opcode_index].opcode.tick <= note.tick) {
            const auto& opcode = opcodes[opcode_index].opcode.opcode;
            switch (opcode.opcode) {
            case 0xAC:
                if (!opcode.params.empty()) {
                    current_instrument = opcode.params[0];
                }
                break;
            case 0x94:
                if (!opcode.params.empty()) {
                    current_octave = opcode.params[0];
                }
                break;
            case 0x95:
                if (current_octave >= 0) {
                    ++current_octave;
                }
                break;
            case 0x96:
                if (current_octave >= 0) {
                    --current_octave;
                }
                break;
            default:
                break;
            }
            ++opcode_index;
        }

        if (current_instrument < 0 || current_instrument >= 255 || current_octave < 0) {
            continue;
        }
        const int32_t played_sample_id = smd_instrument_opcode_param_to_played_sample_id(current_instrument);
        const auto root_midi = played_sample_root_midi_note(played_sample_id);
        if (!root_midi.has_value()) {
            continue;
        }
        const int32_t root_octave = *root_midi / 12;
        if (std::abs(current_octave - root_octave) <= 1) {
            continue;
        }

        add_diagnostic(
            diagnostics,
            note.tick,
            FFTSmdLaneDiagnosticSeverity::warning,
            "RootOct",
            "Active octave is more than one octave away from the instrument root.");
    }
}

}  // namespace

std::vector<std::vector<FFTSmdLaneDiagnostic>> validate_smd_authoring_document(
    const FFTSmdAuthoringDocument& document
) {
    std::vector<std::vector<FFTSmdLaneDiagnostic>> diagnostics(document.parts.size());

    for (size_t track_index = 0; track_index < document.parts.size(); ++track_index) {
        const auto& track = document.parts[track_index];
        auto& track_diagnostics = diagnostics[track_index];
        const std::vector<IndexedOpcode> sorted_opcodes = sorted_part_opcodes(track);

        validate_control_flow_placement(track, sorted_opcodes, track_diagnostics);
        validate_end_and_repeat_structure(track, sorted_opcodes, track_diagnostics);
        validate_pre_note_setup(track, sorted_opcodes, track_diagnostics);
        validate_instrument_root_octave_fit(track, sorted_opcodes, track_diagnostics);

        std::stable_sort(
            track_diagnostics.begin(),
            track_diagnostics.end(),
            [](const FFTSmdLaneDiagnostic& lhs, const FFTSmdLaneDiagnostic& rhs) {
                if (lhs.tick != rhs.tick) {
                    return lhs.tick < rhs.tick;
                }
                return static_cast<int>(lhs.severity) > static_cast<int>(rhs.severity);
            });
    }

    return diagnostics;
}

}  // namespace fftplugin
