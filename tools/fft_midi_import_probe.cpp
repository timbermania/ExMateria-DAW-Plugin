#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fft_plugin/fft_instrument_catalog.h"
#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_presentation_utils.h"
#include "fft_plugin/fft_smd_validation.h"
#include "FFTMidiImport.h"

namespace {

std::string note_name(int32_t relative_key) {
    static constexpr std::array<std::string_view, 12> kNames {{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    }};
    if (relative_key >= 0 && relative_key < static_cast<int32_t>(kNames.size())) {
        return std::string(kNames[static_cast<size_t>(relative_key)]);
    }
    return "?";
}

std::optional<int32_t> parse_root_midi_note_from_catalog_name(std::string_view name) {
    const size_t open = name.find('(');
    const size_t close = name.find(')', open == std::string_view::npos ? 0 : open + 1);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1) {
        return std::nullopt;
    }

    std::string token(name.substr(open + 1, close - open - 1));
    const size_t comma = token.find(',');
    if (comma != std::string::npos) {
        token.resize(comma);
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
        token.pop_back();
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())) != 0) {
        token.erase(token.begin());
    }

    static constexpr std::array<std::pair<std::string_view, int32_t>, 12> kNoteNames {{
        {"C#", 1}, {"D#", 3}, {"F#", 6}, {"G#", 8}, {"A#", 10},
        {"C", 0}, {"D", 2}, {"E", 4}, {"F", 5}, {"G", 7}, {"A", 9}, {"B", 11},
    }};
    for (const auto& [prefix, semitone] : kNoteNames) {
        if (token.rfind(prefix, 0) != 0) {
            continue;
        }
        if (token.size() <= prefix.size() + 1 || token[prefix.size()] != '-') {
            return std::nullopt;
        }
        return (std::stoi(token.substr(prefix.size() + 1)) * 12) + semitone;
    }
    return std::nullopt;
}

std::optional<int32_t> played_sample_root_midi_note(int32_t played_sample_id) {
    const auto& catalog = fftplugin::fft_instrument_catalog();
    const auto it = std::find_if(
        catalog.begin(),
        catalog.end(),
        [played_sample_id](const fftplugin::FFTInstrumentCatalogEntry& entry) {
            return entry.id == played_sample_id;
        });
    if (it == catalog.end()) {
        return std::nullopt;
    }
    return parse_root_midi_note_from_catalog_name(it->name);
}

std::string instrument_name(int32_t played_sample_id) {
    const auto& catalog = fftplugin::fft_instrument_catalog();
    const auto it = std::find_if(
        catalog.begin(),
        catalog.end(),
        [played_sample_id](const fftplugin::FFTInstrumentCatalogEntry& entry) {
            return entry.id == played_sample_id;
        });
    return it == catalog.end() ? ("Inst " + std::to_string(played_sample_id)) : it->name;
}

void print_part_analysis(const fftplugin::FFTSmdAuthoringPart& part, size_t part_index) {
    if (part.kind != fftplugin::FFTSmdAuthoringPartKind::poly_track || part.poly_track.notes.empty()) {
        return;
    }

    auto notes = part.poly_track.notes;
    std::stable_sort(
        notes.begin(),
        notes.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            return lhs.relative_key < rhs.relative_key;
        });

    auto opcodes = part.poly_track.opcodes;
    std::stable_sort(
        opcodes.begin(),
        opcodes.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.stack_order < rhs.stack_order;
        });

    int32_t current_inst = -1;
    int32_t current_oct = -1;
    size_t opcode_index = 0;
    int violations = 0;

    std::cout << "part " << part_index + 1 << ": " << part.name << "\n";
    for (size_t note_index = 0; note_index < notes.size(); ++note_index) {
        const auto& note = notes[note_index];
        while (opcode_index < opcodes.size() && opcodes[opcode_index].tick <= note.start_tick) {
            const auto& opcode = opcodes[opcode_index];
            if (opcode.opcode.opcode == 0xAC && !opcode.opcode.params.empty()) {
                current_inst = opcode.opcode.params[0];
            } else if (opcode.opcode.opcode == 0x94 && !opcode.opcode.params.empty()) {
                current_oct = opcode.opcode.params[0];
            } else if (opcode.opcode.opcode == 0x95 && current_oct >= 0) {
                ++current_oct;
            } else if (opcode.opcode.opcode == 0x96 && current_oct >= 0) {
                --current_oct;
            }
            ++opcode_index;
        }

        const int32_t played_sample_id = fftplugin::smd_instrument_opcode_param_to_played_sample_id(current_inst);
        const auto root_note = played_sample_root_midi_note(played_sample_id);
        const int32_t root_oct = root_note.has_value() ? (*root_note / 12) : -1;
        const bool bad = root_note.has_value() && current_oct >= 0 && std::abs(current_oct - root_oct) > 1;
        violations += bad ? 1 : 0;

        if (note_index == 0 || bad) {
            std::cout
                << "  note tick=" << note.start_tick
                << " key=" << note_name(note.relative_key)
                << " inst=" << played_sample_id
                << " (" << instrument_name(played_sample_id) << ")"
                << " octave=" << current_oct;
            if (root_note.has_value()) {
                std::cout << " root_oct=" << root_oct;
            } else {
                std::cout << " root_oct=?";
            }
            if (bad) {
                std::cout << "  <-- VIOLATION";
            }
            std::cout << "\n";
        }
    }
    std::cout << "  violations=" << violations << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fft_midi_import_probe <midi-path> [save-fftauth-path]\n";
        return 1;
    }

    const juce::File midi_file(argv[1]);
    std::string error;
    auto authored = fftplugin::jucewrap::import_midi_file_to_authoring_document(midi_file, &error);
    if (!authored.has_value()) {
        std::cerr << "import_error=" << error << "\n";
        return 2;
    }

    if (argc >= 3) {
        if (!fftplugin::save_smd_authoring_document(argv[2], *authored, &error)) {
            std::cerr << "save_error=" << error << "\n";
            return 3;
        }
        std::cout << "saved=" << argv[2] << "\n";
    }

    const auto diagnostics = fftplugin::validate_smd_authoring_document(*authored);
    std::cout << "parts=" << authored->parts.size() << "\n";
    for (size_t i = 0; i < authored->parts.size(); ++i) {
        const auto& part = authored->parts[i];
        const size_t diag_count = i < diagnostics.size() ? diagnostics[i].size() : 0;
        std::cout << (i + 1) << ": " << part.name << " diag=" << diag_count << "\n";
        if (i + 1 == 19) {
            print_part_analysis(part, i);
            if (i < diagnostics.size()) {
                for (const auto& diagnostic : diagnostics[i]) {
                    std::cout << "  diag tick=" << diagnostic.tick
                              << " label=" << diagnostic.short_label
                              << " msg=" << diagnostic.message << "\n";
                }
            }
        }
    }
    return 0;
}
