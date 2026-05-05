#ifndef FFT_SPU_MIX_TOOLS_H
#define FFT_SPU_MIX_TOOLS_H

#include <cstdint>
#include <vector>

#include "fft_spu_pitch_runtime.h"
#include "fft_spu_sample_runtime.h"
#include "fft_spu_voice_runtime.h"

namespace fftshared {

struct FFTSpuVoiceMixResult {
	bool active = false;
	int32_t env_vol = 0;
	int32_t sample = 0;
	int32_t vol_l = 0;
	int32_t vol_r = 0;
};

struct FFTSpuFrameRenderResult {
	int32_t sum_l = 0;
	int32_t sum_r = 0;
	int32_t rvb_in_l = 0;
	int32_t rvb_in_r = 0;
	int32_t target_sample = 0;
};

bool fft_finalize_voice_mix_frame(FFTSpuVoiceRuntime &voice, int32_t sample,
		int32_t volume_divisor, FFTSpuVoiceMixResult &result);
bool fft_render_voice_mix_frame(FFTSpuVoiceRuntime &voice, int32_t effective_sinc,
		int32_t volume_divisor, const uint8_t *spu_ram, int32_t spu_ram_size,
		FFTSpuVoiceMixResult &result);
void fft_render_mix_frame(std::vector<FFTSpuVoiceRuntime> &voices,
		bool lfo_pitch_bias_enabled, int32_t volume_max, int32_t pitch_to_sinc_shift,
		int32_t volume_divisor, const uint8_t *spu_ram, int32_t spu_ram_size,
		int32_t target_voice_idx, std::vector<FFTSpuVoiceMixResult> &voice_results,
		FFTSpuFrameRenderResult &frame_result);

}  // namespace fftshared

#endif  // FFT_SPU_MIX_TOOLS_H
