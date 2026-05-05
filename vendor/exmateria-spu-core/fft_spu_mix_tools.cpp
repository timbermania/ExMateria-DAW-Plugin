#include "fft_spu_mix_tools.h"

#include <cstdint>

namespace fftshared {

namespace {

int32_t floor_div_i64(int64_t numer, int64_t denom) {
	int64_t q = numer / denom;
	int64_t r = numer % denom;
	if (r != 0 && ((r < 0) != (denom < 0))) {
		q -= 1;
	}
	return static_cast<int32_t>(q);
}

}  // namespace

bool fft_finalize_voice_mix_frame(FFTSpuVoiceRuntime &voice, int32_t sample,
		int32_t volume_divisor, FFTSpuVoiceMixResult &result) {
	result = FFTSpuVoiceMixResult {};
	if (!voice.on) {
		return false;
	}

	if (voice.stop_requested) {
		voice.adsr.key_off();
		voice.stop_requested = false;
	}

	result.env_vol = voice.adsr.mix();
	if (voice.adsr.state == FFTAdsrEnvelope::STOPPED) {
		voice.on = false;
		return false;
	}

	result.sample = floor_div_i64(int64_t(sample) * result.env_vol, 1023);
	voice.sval = result.sample;
	result.vol_l = floor_div_i64(int64_t(result.sample) * voice.left_volume, volume_divisor);
	result.vol_r = floor_div_i64(int64_t(result.sample) * voice.right_volume, volume_divisor);
	result.active = true;
	return true;
}

bool fft_render_voice_mix_frame(FFTSpuVoiceRuntime &voice, int32_t effective_sinc,
		int32_t volume_divisor, const uint8_t *spu_ram, int32_t spu_ram_size,
		FFTSpuVoiceMixResult &result) {
	if (!voice.on) {
		result = FFTSpuVoiceMixResult {};
		return false;
	}
	const int32_t source_sample = fft_get_voice_source_sample(voice, effective_sinc, spu_ram, spu_ram_size);
	if (!voice.on) {
		result = FFTSpuVoiceMixResult {};
		return false;
	}
	return fft_finalize_voice_mix_frame(voice, source_sample, volume_divisor, result);
}

void fft_render_mix_frame(std::vector<FFTSpuVoiceRuntime> &voices,
		bool lfo_pitch_bias_enabled, int32_t volume_max, int32_t pitch_to_sinc_shift,
		int32_t volume_divisor, const uint8_t *spu_ram, int32_t spu_ram_size,
		int32_t target_voice_idx, std::vector<FFTSpuVoiceMixResult> &voice_results,
		FFTSpuFrameRenderResult &frame_result) {
	frame_result = FFTSpuFrameRenderResult {};
	voice_results.assign(voices.size(), FFTSpuVoiceMixResult {});

	for (size_t idx = 0; idx < voices.size(); ++idx) {
		FFTSpuVoiceRuntime &voice = voices[idx];
		if (!voice.on) {
			continue;
		}

		FFTSpuVoiceMixResult &mix_result = voice_results[idx];
		const int32_t effective_sinc = fft_effective_voice_sinc(
				voice, lfo_pitch_bias_enabled, volume_max, pitch_to_sinc_shift);
		if (!fft_render_voice_mix_frame(voice, effective_sinc, volume_divisor, spu_ram, spu_ram_size, mix_result)) {
			continue;
		}

		frame_result.sum_l += mix_result.vol_l;
		frame_result.sum_r += mix_result.vol_r;
		if (voice.reverb) {
			frame_result.rvb_in_l += mix_result.vol_l;
			frame_result.rvb_in_r += mix_result.vol_r;
		}
		if (static_cast<int32_t>(idx) == target_voice_idx) {
			frame_result.target_sample = mix_result.sample;
		}
	}
}

}  // namespace fftshared
