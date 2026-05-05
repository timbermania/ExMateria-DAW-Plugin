#include "fft_spu_pitch_runtime.h"

#include <algorithm>
#include <cstdint>

#include "fft_pitch_tools.h"

namespace fftshared {

void fft_set_voice_pitch(FFTSpuVoiceRuntime &voice, int32_t raw_pitch, int32_t volume_max, int32_t pitch_to_sinc_shift) {
	voice.raw_pitch = std::clamp(raw_pitch, 1, volume_max);
	voice.sinc = voice.raw_pitch << pitch_to_sinc_shift;
}

void fft_set_voice_pre_pitch(FFTSpuVoiceRuntime &voice, int32_t pre_pitch) {
	voice.pre_pitch = pre_pitch;
}

void fft_tick_voice_pitch_lfo(FFTSpuVoiceRuntime &voice) {
	fft_tick_pitch_lfo(voice.lfo_blocks[0]);
}

bool fft_advance_lfo_tick_counter(int32_t &sample_counter, int32_t tick_samples) {
	sample_counter += 1;
	if (sample_counter < tick_samples) {
		return false;
	}
	sample_counter = 0;
	return true;
}

int32_t fft_effective_voice_sinc(const FFTSpuVoiceRuntime &voice, bool lfo_pitch_bias_enabled,
		int32_t volume_max, int32_t pitch_to_sinc_shift) {
	const FFTPitchLfoBlock &block = voice.lfo_blocks[0];
	if (!lfo_pitch_bias_enabled || !block.enabled || block.scaled_output == 0) {
		return voice.sinc;
	}

	int32_t biased_raw;
	if (voice.pre_pitch != 0) {
		biased_raw = fft_raw_pitch_from_pre_pitch(voice.pre_pitch + block.scaled_output);
	} else {
		biased_raw = voice.raw_pitch + block.scaled_output;
	}
	biased_raw = std::clamp(biased_raw, 1, volume_max);
	return biased_raw << pitch_to_sinc_shift;
}

}  // namespace fftshared
