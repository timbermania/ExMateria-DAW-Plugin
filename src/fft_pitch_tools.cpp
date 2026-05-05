#include "fft_plugin/fft_pitch_tools.h"

namespace fftplugin {

namespace {

#include "../vendor/exmateria-spu-core/detail/pitch_tables.inc"

}  // namespace

int32_t fft_pre_pitch_from_note(int16_t midi_note, int32_t fine_tune) {
    return static_cast<int32_t>(midi_note) * 256 + fine_tune;
}

int32_t fft_raw_pitch_from_pre_pitch(int32_t pre_pitch) {
    if (pre_pitch < 0) {
        pre_pitch = 0;
    }

    const int32_t octave_index = (pre_pitch >> 8) & 0x7F;
    const int32_t fine_byte = pre_pitch & 0xFF;
    const int32_t semitone = static_cast<int32_t>(SEMITONE_LOOKUP[octave_index]);
    const int32_t shift = 6 - static_cast<int32_t>(OCTAVE_SHIFT_LOOKUP[octave_index]);
    int32_t table_index = semitone * 256 + fine_byte;
    constexpr int32_t table_size = static_cast<int32_t>(sizeof(PITCH_TABLE) / sizeof(PITCH_TABLE[0]));
    if (table_index >= table_size) {
        table_index = table_size - 1;
    }

    const int32_t base = static_cast<int32_t>(PITCH_TABLE[table_index]);
    return shift >= 0 ? (base >> shift) : (base << (-shift));
}

int32_t fft_raw_pitch_from_note(int16_t midi_note, int32_t fine_tune) {
    return fft_raw_pitch_from_pre_pitch(fft_pre_pitch_from_note(midi_note, fine_tune));
}

}  // namespace fftplugin
