#pragma once

#include <cstdint>

namespace fftplugin {

int32_t fft_pre_pitch_from_note(int16_t midi_note, int32_t fine_tune);
int32_t fft_raw_pitch_from_pre_pitch(int32_t pre_pitch);
int32_t fft_raw_pitch_from_note(int16_t midi_note, int32_t fine_tune);

}  // namespace fftplugin
