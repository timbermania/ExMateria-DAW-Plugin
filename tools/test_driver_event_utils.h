// Test-driver-only utilities for printing and comparing SMD events /
// authored opcodes. Header-only so each driver can include without
// dragging in extra build deps. Used by:
//   - fft_polytrack_roundtrip_driver.cpp
//   - fft_smd_authoring_parity_driver.cpp
// (not built into fft_plugin_core; drivers each compile their own copy
//  via this single source.)

#pragma once

#include <string>
#include <vector>
#include <variant>

#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_file.h"

namespace fftplugin::test_driver {

inline std::string describe_event(const fftplugin::FFTSmdTrackEvent& event) {
    if (const auto* note = std::get_if<fftplugin::FFTSmdNoteEvent>(&event)) {
        return "note key=" + std::to_string(note->relative_key) +
            " vel=" + std::to_string(note->velocity) +
            " dt=" + std::to_string(note->delta_time);
    }
    const auto& opcode = std::get<fftplugin::FFTSmdOpcodeEvent>(event);
    std::string text = "opcode=0x";
    constexpr char kHex[] = "0123456789ABCDEF";
    text.push_back(kHex[(opcode.opcode >> 4) & 0xF]);
    text.push_back(kHex[opcode.opcode & 0xF]);
    text += " params=[";
    for (size_t index = 0; index < opcode.params.size(); ++index) {
        if (index != 0) text += ",";
        text += std::to_string(opcode.params[index]);
    }
    text += "]";
    return text;
}

inline std::string describe_authored_opcode(const fftplugin::FFTSmdAuthoredOpcode& opcode) {
    std::string text =
        "tick=" + std::to_string(opcode.tick) +
        " stack=" + std::to_string(opcode.stack_order) +
        " enabled=" + std::to_string(opcode.enabled ? 1 : 0) +
        " exact=" + std::to_string(opcode.exact_timing ? 1 : 0) +
        " opcode=0x";
    constexpr char kHex[] = "0123456789ABCDEF";
    text.push_back(kHex[(opcode.opcode.opcode >> 4) & 0xF]);
    text.push_back(kHex[opcode.opcode.opcode & 0xF]);
    text += " params=[";
    for (size_t index = 0; index < opcode.opcode.params.size(); ++index) {
        if (index != 0) text += ",";
        text += std::to_string(opcode.opcode.params[index]);
    }
    text += "]";
    return text;
}

inline bool same_event(
    const fftplugin::FFTSmdTrackEvent& lhs, const fftplugin::FFTSmdTrackEvent& rhs
) {
    if (lhs.index() != rhs.index()) return false;
    if (const auto* lhs_note = std::get_if<fftplugin::FFTSmdNoteEvent>(&lhs)) {
        const auto& rhs_note = std::get<fftplugin::FFTSmdNoteEvent>(rhs);
        return lhs_note->velocity == rhs_note.velocity &&
            lhs_note->relative_key == rhs_note.relative_key &&
            lhs_note->delta_time == rhs_note.delta_time;
    }
    const auto& lhs_opcode = std::get<fftplugin::FFTSmdOpcodeEvent>(lhs);
    const auto& rhs_opcode = std::get<fftplugin::FFTSmdOpcodeEvent>(rhs);
    return lhs_opcode.opcode == rhs_opcode.opcode &&
        lhs_opcode.params == rhs_opcode.params;
}

inline bool same_authored_opcode(
    const fftplugin::FFTSmdAuthoredOpcode& lhs, const fftplugin::FFTSmdAuthoredOpcode& rhs
) {
    return lhs.tick == rhs.tick &&
        lhs.stack_order == rhs.stack_order &&
        lhs.enabled == rhs.enabled &&
        lhs.exact_timing == rhs.exact_timing &&
        lhs.opcode.opcode == rhs.opcode.opcode &&
        lhs.opcode.params == rhs.opcode.params;
}

}  // namespace fftplugin::test_driver
