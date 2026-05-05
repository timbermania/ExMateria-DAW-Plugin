#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/processor/track_grouping_helpers.h"
#include "test_driver_event_utils.h"

namespace {

using namespace fftplugin;
using fftplugin::test_driver::describe_event;
using fftplugin::test_driver::same_event;
using fftplugin::test_driver::same_authored_opcode;

// Local alias: drivers use `describe_opcode` for the authored variant.
inline std::string describe_opcode(const FFTSmdAuthoredOpcode& opcode) {
    return fftplugin::test_driver::describe_authored_opcode(opcode);
}

void print_opcode_list(const std::string& label, const std::vector<FFTSmdAuthoredOpcode>& opcodes) {
    std::cout << label << "_opcode_count=" << opcodes.size() << "\n";
    for (size_t index = 0; index < opcodes.size(); ++index) {
        std::cout << "  [" << index << "] " << describe_opcode(opcodes[index]) << "\n";
    }
}

[[nodiscard]] bool compare_opcode_lists(
    const std::string& lhs_label,
    const std::vector<FFTSmdAuthoredOpcode>& lhs,
    const std::string& rhs_label,
    const std::vector<FFTSmdAuthoredOpcode>& rhs
) {
    bool ok = true;
    if (lhs.size() != rhs.size()) {
        ok = false;
        std::cout << lhs_label << "_vs_" << rhs_label << "_opcode_count_mismatch "
                  << lhs.size() << " " << rhs.size() << "\n";
    }
    const size_t compare_count = std::min(lhs.size(), rhs.size());
    for (size_t index = 0; index < compare_count; ++index) {
        if (same_authored_opcode(lhs[index], rhs[index])) {
            continue;
        }
        ok = false;
        std::cout << lhs_label << "_vs_" << rhs_label << "_first_opcode_diff index=" << index << "\n";
        std::cout << "  " << lhs_label << ": " << describe_opcode(lhs[index]) << "\n";
        std::cout << "  " << rhs_label << ": " << describe_opcode(rhs[index]) << "\n";
        break;
    }
    if (ok) {
        std::cout << lhs_label << "_vs_" << rhs_label << "_opcode_exact=1\n";
    }
    return ok;
}

[[nodiscard]] bool compare_event_tracks(
    const std::string& lhs_label,
    const std::vector<FFTSmdTrackEvent>& lhs,
    const std::string& rhs_label,
    const std::vector<FFTSmdTrackEvent>& rhs
) {
    bool ok = true;
    if (lhs.size() != rhs.size()) {
        ok = false;
        std::cout << lhs_label << "_vs_" << rhs_label << "_event_count_mismatch "
                  << lhs.size() << " " << rhs.size() << "\n";
    }
    const size_t compare_count = std::min(lhs.size(), rhs.size());
    for (size_t index = 0; index < compare_count; ++index) {
        if (same_event(lhs[index], rhs[index])) {
            continue;
        }
        ok = false;
        std::cout << lhs_label << "_vs_" << rhs_label << "_first_event_diff index=" << index << "\n";
        std::cout << "  " << lhs_label << ": " << describe_event(lhs[index]) << "\n";
        std::cout << "  " << rhs_label << ": " << describe_event(rhs[index]) << "\n";
        break;
    }
    if (ok) {
        std::cout << lhs_label << "_vs_" << rhs_label << "_event_exact=1\n";
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string smd_path = argc > 1
        ? argv[1]
        : "C:\\Users\\acurr\\Documents\\GitHub\\fft-project\\project-assets\\fft-extract\\fft-extract\\SOUND\\MUSIC_31.SMD";
    const int32_t track_index = argc > 2 ? std::stoi(argv[2]) : 1;

    std::string error_message;
    const auto raw_smd = load_smd_file(smd_path, &error_message);
    if (!raw_smd.has_value()) {
        std::cerr << "load_smd_error=" << error_message << "\n";
        return 1;
    }

    const auto authored = import_smd_authoring_document(*raw_smd);
    if (track_index < 0 || static_cast<size_t>(track_index) >= authored.parts.size()) {
        std::cerr << "invalid_track_index=" << track_index << "\n";
        return 1;
    }
    const auto& raw_part = authored.parts[static_cast<size_t>(track_index)];
    if (raw_part.kind != FFTSmdAuthoringPartKind::raw_track) {
        std::cerr << "selected_part_is_not_raw_track\n";
        return 1;
    }

    FFTSmdAuthoringDocument grouped_document;
    grouped_document.initial_tempo = authored.initial_tempo;
    grouped_document.initial_volume = authored.initial_volume;
    grouped_document.assoc_wds_id = authored.assoc_wds_id;
    grouped_document.song_title = authored.song_title;

    FFTSmdAuthoringPart grouped_part;
    grouped_part.kind = FFTSmdAuthoringPartKind::poly_track;
    grouped_part.name = "PolyDebug";
    grouped_part.poly_track = poly_track_from_raw_parts({raw_part});
    grouped_document.parts.push_back(grouped_part);

    const auto grouped_compiled = compile_smd_authoring_document(grouped_document);
    const auto ungrouped_document = import_smd_authoring_document(grouped_compiled.smd);
    if (ungrouped_document.parts.empty()) {
        std::cerr << "ungroup_import_empty\n";
        return 1;
    }
    const auto& ungrouped_part = ungrouped_document.parts.front();
    if (ungrouped_part.kind != FFTSmdAuthoringPartKind::raw_track) {
        std::cerr << "ungrouped_part_is_not_raw_track\n";
        return 1;
    }

    const auto original_compiled = compile_smd_authoring_document(FFTSmdAuthoringDocument {
        .format_version = authored.format_version,
        .track_count = 1,
        .initial_tempo = authored.initial_tempo,
        .initial_volume = authored.initial_volume,
        .assoc_wds_id = authored.assoc_wds_id,
        .song_title = authored.song_title,
        .parts = {raw_part},
    });
    const auto ungrouped_compiled = compile_smd_authoring_document(FFTSmdAuthoringDocument {
        .format_version = ungrouped_document.format_version,
        .track_count = 1,
        .initial_tempo = ungrouped_document.initial_tempo,
        .initial_volume = ungrouped_document.initial_volume,
        .assoc_wds_id = ungrouped_document.assoc_wds_id,
        .song_title = ungrouped_document.song_title,
        .parts = {ungrouped_part},
    });

    print_opcode_list("raw", raw_part.raw_track.opcodes);
    print_opcode_list("poly", grouped_part.poly_track.opcodes);
    print_opcode_list("ungrouped", ungrouped_part.raw_track.opcodes);

    bool ok = true;
    ok &= compare_opcode_lists("raw", raw_part.raw_track.opcodes, "poly", grouped_part.poly_track.opcodes);
    ok &= compare_opcode_lists("raw", raw_part.raw_track.opcodes, "ungrouped", ungrouped_part.raw_track.opcodes);

    if (!original_compiled.smd.track_events.empty() && !grouped_compiled.smd.track_events.empty()) {
        ok &= compare_event_tracks(
            "raw_compiled",
            original_compiled.smd.track_events.front(),
            "poly_compiled_lane0",
            grouped_compiled.smd.track_events.front());
    }
    if (!original_compiled.smd.track_events.empty() && !ungrouped_compiled.smd.track_events.empty()) {
        ok &= compare_event_tracks(
            "raw_compiled",
            original_compiled.smd.track_events.front(),
            "ungrouped_compiled",
            ungrouped_compiled.smd.track_events.front());
    }

    return ok ? 0 : 2;
}
