#ifndef FFT_SPU_PITCH_RUNTIME_H
#define FFT_SPU_PITCH_RUNTIME_H

#include <cstdint>

#include "fft_spu_voice_runtime.h"

namespace fftshared {

void fft_set_voice_pitch(FFTSpuVoiceRuntime &voice, int32_t raw_pitch, int32_t volume_max, int32_t pitch_to_sinc_shift);
void fft_set_voice_pre_pitch(FFTSpuVoiceRuntime &voice, int32_t pre_pitch);
void fft_tick_voice_pitch_lfo(FFTSpuVoiceRuntime &voice);
bool fft_advance_lfo_tick_counter(int32_t &sample_counter, int32_t tick_samples);
int32_t fft_effective_voice_sinc(const FFTSpuVoiceRuntime &voice, bool lfo_pitch_bias_enabled,
		int32_t volume_max, int32_t pitch_to_sinc_shift);

}  // namespace fftshared

#endif  // FFT_SPU_PITCH_RUNTIME_H
