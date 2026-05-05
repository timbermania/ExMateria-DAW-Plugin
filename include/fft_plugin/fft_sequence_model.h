#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace fftplugin {

enum class OpcodeLaneType : uint8_t {
    instrument = 0,
    structure = 1,
    articulation = 2,
    modulation = 3,
    conductor = 4,
};

enum class OpcodeType : uint16_t {
    instrument_change = 0x00AC,
    repeat = 0x0098,
    coda = 0x0099,
    adsr_override = 0x00C0,
    pitch_lfo = 0x00D8,
    dynamics = 0x00E0,
    pan = 0x00E8,
    tempo = 0x00A0,
};

struct FFTNoteEvent {
    int32_t start_tick = 0;
    int32_t duration_ticks = 0;
    int16_t midi_note = 60;
    int16_t velocity = 100;
    int32_t gate_ticks = 0;
    bool slur_in = false;
    bool slur_out = false;
};

struct FFTInstrumentChangeEvent {
    int32_t start_tick = 0;
    int32_t instrument_id = 0;
};

struct FFTStructureEvent {
    int32_t start_tick = 0;
    OpcodeType opcode_type = OpcodeType::repeat;
    int32_t repeat_count = 0;
};

struct FFTControllerEvent {
    int32_t start_tick = 0;
    OpcodeType opcode_type = OpcodeType::dynamics;
    int32_t value_a = 0;
    int32_t value_b = 0;
};

using FFTTrackEvent = std::variant<
    FFTNoteEvent,
    FFTInstrumentChangeEvent,
    FFTStructureEvent,
    FFTControllerEvent>;

struct FFTTrack {
    int32_t id = 0;
    std::string name;
    int32_t default_instrument = 0;
    int16_t default_pan = 64;
    int16_t default_volume = 127;
    std::vector<FFTTrackEvent> events;
};

struct FFTConductorEvent {
    int32_t start_tick = 0;
    OpcodeType opcode_type = OpcodeType::tempo;
    int32_t value_a = 0;
    int32_t value_b = 0;
};

struct FFTSequence {
    int32_t format_version = 1;
    int32_t ppq = 48;
    std::string name;
    std::vector<FFTTrack> tracks;
    std::vector<FFTConductorEvent> conductor_events;
};

}  // namespace fftplugin
