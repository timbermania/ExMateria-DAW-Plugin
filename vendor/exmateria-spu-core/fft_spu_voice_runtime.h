#ifndef FFT_SPU_VOICE_RUNTIME_H
#define FFT_SPU_VOICE_RUNTIME_H

#include <array>
#include <cstdint>

#include "fft_adsr_envelope.h"
#include "fft_spu_lfo_tools.h"

namespace fftshared {

struct FFTSpuInstrumentData {
	bool is_null = true;
	int32_t fine_tune = 0;
	int32_t adsr1 = 0;
	int32_t adsr2 = 0;
	int32_t sample_offset = 0;
	int32_t sample_size = 0;
	int32_t loop_start = -1;
	int32_t loop_offset_bytes = -1;
	bool has_explicit_loop_start = false;
	bool has_loop_repeat = false;
	int32_t start_offset_bytes = 0;
};

struct FFTSpuVoiceAddresses {
	int32_t start_addr = 0;
	int32_t loop_addr = 0;
	int32_t end_addr = 0;
};

struct FFTSpuVoiceRuntime {
	bool on = false;
	bool fresh_key_on = false;
	bool stop_requested = false;
	bool stop_after_block = false;
	int32_t instrument_idx = -1;
	int32_t start_addr = 0;
	int32_t curr_addr = 0;
	int32_t loop_addr = 0;
	int32_t end_addr = 0;
	int32_t requested_loop_addr = 0;
	std::array<int32_t, 64> sample_buf = {};
	int32_t buf_pos = 28;
	int32_t adpcm_s1 = 0;
	int32_t adpcm_s2 = 0;
	int32_t latest_sample = 0;
	int32_t latest_interp_sample = 0;
	int32_t spos = 0;
	int32_t sinc = 0;
	std::array<int32_t, 4> gauss_buf = { 0, 0, 0, 0 };
	int32_t gauss_pos = 0;
	int32_t left_volume = 0x3FFF;
	int32_t right_volume = 0x3FFF;
	int32_t mix_volume_base = 0;
	int32_t mix_velocity_gain = 0;
	int32_t mix_pan_base = 0;
	int32_t mix_master_gain = 0x7F00;
	int32_t mix_global_pan = 0;
	int32_t raw_pitch = 0;
	int32_t pre_pitch = 0;
	int32_t adsr1 = 0;
	int32_t adsr2 = 0;
	FFTAdsrEnvelope adsr;
	bool reverb = false;
	int32_t sval = 0;
	std::array<FFTPitchLfoBlock, 4> lfo_blocks = {};
};

bool fft_spu_instrument_playable(const FFTSpuInstrumentData &instrument);
FFTSpuVoiceAddresses fft_compute_voice_addresses(const FFTSpuInstrumentData &instrument,
		int32_t ram_instrument_base, int32_t spu_ram_size, int32_t adpcm_block_size,
		int32_t start_addr_override, int32_t loop_addr_override, bool use_overrides);
void fft_prepare_voice_for_key_on(FFTSpuVoiceRuntime &voice, int32_t instrument_idx,
		const FFTSpuVoiceAddresses &addresses, int32_t raw_pitch,
		int32_t vol_l, int32_t vol_r, int32_t adsr1, int32_t adsr2,
		bool reverb, bool preserve_repeat_addr, int32_t volume_max,
		int32_t pitch_to_sinc_shift, int32_t adpcm_samples_per_block);

}  // namespace fftshared

#endif  // FFT_SPU_VOICE_RUNTIME_H
