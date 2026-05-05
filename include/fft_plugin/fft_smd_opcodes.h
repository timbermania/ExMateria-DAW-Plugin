#pragma once

#include <cstdint>

namespace fftplugin {

// Centralized SMD opcode vocabulary. Every opcode handler in the plugin
// should reach for these enumerators rather than embedding the byte value.
// Values match the on-disc byte layout, so static_cast<FFTSmdOpcode>(byte)
// is the canonical bridge from the wire format.
//
// Source of truth: kOpcodeInfo + kExtraOpcodes in src/fft_smd_file.cpp.
// The free functions below derive their categorization from this enum so
// per-file constexpr blocks (e.g. fft_smd_lane_packer.cpp's OP_* constants)
// can be retired.
enum class FFTSmdOpcode : uint8_t {
    // Time-advancing
    REST                                = 0x80,
    FERMATA                             = 0x81,
    NOP                                 = 0x82,

    // Structural / loop
    END_BAR                             = 0x90,
    LOOP                                = 0x91,
    TIME_SIGNATURE                      = 0x97,
    REPEAT                              = 0x98,
    CODA                                = 0x99,
    REPEAT_BREAK                        = 0x9A,

    // Octave / pitch state
    OCTAVE                              = 0x94,
    RAISE_OCTAVE                        = 0x95,
    LOWER_OCTAVE                        = 0x96,

    // Tempo
    TEMPO                               = 0xA0,
    TEMPO_SLIDE                         = 0xA2,

    // Instrument / voice
    INSTRUMENT                          = 0xAC,
    UNKNOWN_AD                          = 0xAD,
    PERCUSSION_ON                       = 0xAE,
    PERCUSSION_OFF                      = 0xAF,

    // Articulation
    SLUR_ON                             = 0xB0,
    SLUR_OFF                            = 0xB1,
    REVERB_ON                           = 0xBA,
    REVERB_OFF                          = 0xBB,

    // ADSR
    ADSR_RESET                          = 0xC0,
    ADSR_ATTACK                         = 0xC2,
    ADSR_SUSTAIN_RATE                   = 0xC4,
    ADSR_RELEASE                        = 0xC5,
    ADSR1_LOWNIBBLE_SLIDE_TARGET        = 0xC6,
    ADSR_DECAY_AND_SUSTAIN_LEVEL        = 0xC7,
    ADSR_DECAY                          = 0xC9,
    ADSR_SUSTAIN_LEVEL                  = 0xCA,

    // Pitch / detune / LFO-pitch
    SET_PITCH_BEND                      = 0xD0,
    CONDITIONAL_SEQ_FLAG                = 0xD2,
    DETUNE                              = 0xD6,
    LFO_DEPTH_PITCH                     = 0xD7,
    LFO_LENGTH_PITCH                    = 0xD8,
    FLAG_SET_0xFE                       = 0xDA,
    FLAG_CLEAR_0xFE                     = 0xDB,

    // Dynamics / pan / LFO-vol
    DYNAMICS                            = 0xE0,
    LFO_DEPTH_VOLUME                    = 0xE3,
    LFO_LENGTH_VOLUME                   = 0xE4,
    FLAG_SET_0x11E                      = 0xE6,
    PAN                                 = 0xE8,

    // Bank
    BANK_SELECT                         = 0xFE,
};

// ----- predicate primitives (enum-typed) -----

constexpr bool is_time_only_opcode(FFTSmdOpcode op) {
    return op == FFTSmdOpcode::REST || op == FFTSmdOpcode::FERMATA;
}

constexpr bool is_structure_opcode(FFTSmdOpcode op) {
    switch (op) {
    case FFTSmdOpcode::END_BAR:
    case FFTSmdOpcode::LOOP:
    case FFTSmdOpcode::REPEAT:
    case FFTSmdOpcode::CODA:
    case FFTSmdOpcode::REPEAT_BREAK:
        return true;
    default:
        return false;
    }
}

constexpr bool is_tempo_opcode(FFTSmdOpcode op) {
    return op == FFTSmdOpcode::TEMPO || op == FFTSmdOpcode::TEMPO_SLIDE;
}

constexpr bool is_time_signature_opcode(FFTSmdOpcode op) {
    return op == FFTSmdOpcode::TIME_SIGNATURE;
}

// State-tracked opcodes the lane packer reasons about. Anything outside this
// set on a candidate lane disqualifies it from packing.
constexpr bool is_packer_known_opcode(FFTSmdOpcode op) {
    switch (op) {
    case FFTSmdOpcode::REST:
    case FFTSmdOpcode::FERMATA:
    case FFTSmdOpcode::NOP:
    case FFTSmdOpcode::END_BAR:
    case FFTSmdOpcode::LOOP:
    case FFTSmdOpcode::OCTAVE:
    case FFTSmdOpcode::RAISE_OCTAVE:
    case FFTSmdOpcode::LOWER_OCTAVE:
    case FFTSmdOpcode::INSTRUMENT:
    case FFTSmdOpcode::REVERB_ON:
    case FFTSmdOpcode::REVERB_OFF:
    case FFTSmdOpcode::DYNAMICS:
    case FFTSmdOpcode::PAN:
        return true;
    default:
        return false;
    }
}

// ----- int32_t-typed overloads -----
//
// FFTSmdOpcodeEvent::opcode is int32_t (the raw byte, sign-extended). These
// adapters let call sites that already hold an int32_t avoid casting noise.

constexpr bool is_time_only_opcode(int32_t opcode) {
    return is_time_only_opcode(static_cast<FFTSmdOpcode>(opcode & 0xFF));
}

constexpr bool is_structure_opcode(int32_t opcode) {
    return is_structure_opcode(static_cast<FFTSmdOpcode>(opcode & 0xFF));
}

constexpr bool is_tempo_opcode(int32_t opcode) {
    return is_tempo_opcode(static_cast<FFTSmdOpcode>(opcode & 0xFF));
}

constexpr bool is_time_signature_opcode(int32_t opcode) {
    return is_time_signature_opcode(static_cast<FFTSmdOpcode>(opcode & 0xFF));
}

constexpr bool is_packer_known_opcode(int32_t opcode) {
    return is_packer_known_opcode(static_cast<FFTSmdOpcode>(opcode & 0xFF));
}

// Convenience equality for places that compare an int32_t opcode to a
// specific enumerator. Avoids `op == 0x80` in favor of `op == FFTSmdOpcode::REST`.
constexpr bool operator==(int32_t lhs, FFTSmdOpcode rhs) {
    return static_cast<uint8_t>(lhs & 0xFF) == static_cast<uint8_t>(rhs);
}
constexpr bool operator==(FFTSmdOpcode lhs, int32_t rhs) {
    return rhs == lhs;
}
constexpr bool operator!=(int32_t lhs, FFTSmdOpcode rhs) { return !(lhs == rhs); }
constexpr bool operator!=(FFTSmdOpcode lhs, int32_t rhs) { return !(lhs == rhs); }

// Assignment helper. FFTSmdOpcodeEvent::opcode is int32_t; use
// `op_byte(FFTSmdOpcode::REST)` rather than a magic literal or static_cast.
// (Named `op_byte`, not `op`, to avoid shadowing the many local `op`
// variables already used in opcode handler call sites.)
constexpr int32_t op_byte(FFTSmdOpcode value) {
    return static_cast<int32_t>(static_cast<uint8_t>(value));
}

}  // namespace fftplugin
