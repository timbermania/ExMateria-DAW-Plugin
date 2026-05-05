#pragma once

#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {
namespace jucewrap {

struct FFTOpcodeFilterState {
    bool show_note_commands = false;
    bool show_rest_commands = false;
    bool show_hold_commands = false;
    bool show_instrument_commands = false;
    bool show_dynamics_commands = false;
    bool show_pan_commands = false;
    bool show_tempo_commands = false;
    bool show_adsr_commands = false;
    bool show_octave_commands = false;
    bool show_lfo_commands = false;
    bool show_bend_commands = false;
    bool show_detune_commands = false;
    bool show_reverb_commands = false;
    bool show_slur_commands = false;
    bool show_percussion_commands = false;
    bool show_structure_commands = false;
};

inline bool fft_opcode_filter_active(const FFTOpcodeFilterState& state) {
    return state.show_instrument_commands ||
        state.show_dynamics_commands ||
        state.show_pan_commands ||
        state.show_tempo_commands ||
        state.show_adsr_commands ||
        state.show_octave_commands ||
        state.show_lfo_commands ||
        state.show_bend_commands ||
        state.show_detune_commands ||
        state.show_reverb_commands ||
        state.show_slur_commands ||
        state.show_percussion_commands ||
        state.show_structure_commands;
}

inline bool fft_should_draw_filtered_command(
    const FFTSmdLaneCommandBlock& command,
    const FFTOpcodeFilterState& state
) {
    switch (command.kind) {
    case FFTSmdLaneCommandKind::note:
        return state.show_note_commands;
    case FFTSmdLaneCommandKind::rest:
        return state.show_rest_commands;
    case FFTSmdLaneCommandKind::hold:
        return state.show_hold_commands;
    case FFTSmdLaneCommandKind::tempo:
        return !fft_opcode_filter_active(state) || state.show_tempo_commands;
    case FFTSmdLaneCommandKind::structure:
        return !fft_opcode_filter_active(state) || state.show_structure_commands;
    case FFTSmdLaneCommandKind::opcode:
    default:
        if (!fft_opcode_filter_active(state)) {
            return true;
        }
        if (command.opcode == 0xA9 || command.opcode == 0xAC || command.opcode == 0xFE) {
            return state.show_instrument_commands;
        }
        if (command.opcode == 0xE0 || command.opcode == 0xE1 || command.opcode == 0xE2) {
            return state.show_dynamics_commands;
        }
        if (command.opcode == 0xE8 ||
            command.opcode == 0xE9 ||
            command.opcode == 0xEA ||
            command.opcode == 0xEB ||
            command.opcode == 0xEC ||
            command.opcode == 0xED) {
            return state.show_pan_commands;
        }
        if (command.opcode == 0xC0 ||
            command.opcode == 0xC2 ||
            command.opcode == 0xC4 ||
            command.opcode == 0xC5 ||
            command.opcode == 0xC6 ||
            command.opcode == 0xC7 ||
            command.opcode == 0xC8 ||
            command.opcode == 0xC9 ||
            command.opcode == 0xCA ||
            command.opcode == 0xCF) {
            return state.show_adsr_commands;
        }
        if (command.opcode == 0x94 || command.opcode == 0x95 || command.opcode == 0x96) {
            return state.show_octave_commands;
        }
        if (command.opcode == 0xD7 || command.opcode == 0xD8 || command.opcode == 0xD9 ||
            command.opcode == 0xE3 || command.opcode == 0xE4 || command.opcode == 0xE5) {
            return state.show_lfo_commands;
        }
        if (command.opcode == 0xD0 ||
            command.opcode == 0xD1 ||
            command.opcode == 0xD2 ||
            command.opcode == 0xD4) {
            return state.show_bend_commands;
        }
        if (command.opcode == 0xD6) {
            return state.show_detune_commands;
        }
        if (command.opcode == 0xBA || command.opcode == 0xBB) {
            return state.show_reverb_commands;
        }
        if (command.opcode == 0xB0 || command.opcode == 0xB1) {
            return state.show_slur_commands;
        }
        if (command.opcode == 0xAE || command.opcode == 0xAF) {
            return state.show_percussion_commands;
        }
        return false;
    }
}

}  // namespace jucewrap
}  // namespace fftplugin
