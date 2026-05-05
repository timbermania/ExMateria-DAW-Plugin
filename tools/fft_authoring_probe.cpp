#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "fft_plugin/fft_instrument_catalog.h"
#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_presentation_utils.h"

namespace {

std::string note_name(int32_t relative_key) {
    static constexpr std::array<const char*, 12> kNames {{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    }};
    if (relative_key >= 0 && relative_key < static_cast<int32_t>(kNames.size())) {
        return kNames[static_cast<size_t>(relative_key)];
    }
    return "?";
}

std::string instrument_name(int32_t played_sample_id) {
    if (const auto* entry = fftplugin::find_fft_instrument_catalog_entry(played_sample_id)) {
        return entry->name;
    }
    return "Inst " + std::to_string(played_sample_id);
}

struct NoteState {
    int32_t played_sample_id = -1;
    int32_t octave = -1;
};

template <typename OpcodeVec>
NoteState state_at_tick(const OpcodeVec& opcodes, int32_t tick) {
    NoteState state;
    auto sorted = opcodes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.tick != b.tick) {
            return a.tick < b.tick;
        }
        return a.stack_order < b.stack_order;
    });
    for (const auto& opcode : sorted) {
        if (opcode.tick > tick) {
            break;
        }
        if (opcode.opcode.opcode == 0xAC && !opcode.opcode.params.empty()) {
            state.played_sample_id =
                fftplugin::smd_instrument_opcode_param_to_played_sample_id(opcode.opcode.params[0]);
        } else if (opcode.opcode.opcode == 0x94 && !opcode.opcode.params.empty()) {
            state.octave = opcode.opcode.params[0];
        } else if (opcode.opcode.opcode == 0x95 && state.octave >= 0) {
            ++state.octave;
        } else if (opcode.opcode.opcode == 0x96 && state.octave >= 0) {
            --state.octave;
        }
    }
    return state;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fft_authoring_probe <authoring-path>\n";
        return 1;
    }

    std::string error;
    const auto authored = fftplugin::load_smd_authoring_document(argv[1], &error);
    if (!authored.has_value()) {
        std::cerr << "load_error=" << error << "\n";
        return 2;
    }

    for (size_t i = 0; i < authored->parts.size(); ++i) {
        const auto& part = authored->parts[i];
        std::cout << "row=" << (i + 1) << " track_index=" << i << " name=" << part.name;
        if (part.kind == fftplugin::FFTSmdAuthoringPartKind::raw_track) {
            const auto first_note_it = std::find_if(
                part.raw_track.spans.begin(),
                part.raw_track.spans.end(),
                [](const auto& span) { return span.relative_key != 13; });
            if (first_note_it != part.raw_track.spans.end()) {
                const auto state = state_at_tick(part.raw_track.opcodes, first_note_it->start_tick);
                std::cout << " first_note=" << note_name(first_note_it->relative_key)
                          << " first_tick=" << first_note_it->start_tick;
                if (state.played_sample_id >= 0) {
                    std::cout << " inst=" << state.played_sample_id
                              << " inst_name=" << instrument_name(state.played_sample_id);
                }
                if (state.octave >= 0) {
                    std::cout << " octave=" << state.octave;
                }
            }
        } else {
            if (!part.poly_track.notes.empty()) {
                auto notes = part.poly_track.notes;
                std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
                    if (a.start_tick != b.start_tick) {
                        return a.start_tick < b.start_tick;
                    }
                    return a.relative_key < b.relative_key;
                });
                const auto& first_note = notes.front();
                const auto state = state_at_tick(part.poly_track.opcodes, first_note.start_tick);
                std::cout << " first_note=" << note_name(first_note.relative_key)
                          << " first_tick=" << first_note.start_tick;
                if (state.played_sample_id >= 0) {
                    std::cout << " inst=" << state.played_sample_id
                              << " inst_name=" << instrument_name(state.played_sample_id);
                }
                if (state.octave >= 0) {
                    std::cout << " octave=" << state.octave;
                }
            }
        }
        std::cout << "\n";
    }

    return 0;
}
