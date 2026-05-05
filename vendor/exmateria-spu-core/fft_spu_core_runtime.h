#ifndef FFT_SPU_CORE_RUNTIME_H
#define FFT_SPU_CORE_RUNTIME_H

#include <cstdint>
#include <memory>
#include <vector>

#include "fft_spu_mix_tools.h"
#include "fft_spu_reverb.h"
#include "fft_spu_voice_runtime.h"

namespace fftshared {

class FFTSpuCoreRuntime {
public:
	static constexpr int32_t kNumVoices = 24;
	static constexpr int32_t kSpuRamSize = 0x80000;
	static constexpr int32_t kVolumeMax = 0x3FFF;
	static constexpr int32_t kVolumeDivisor = 0x4000;
	static constexpr int32_t kPitchToSincShift = 4;
	static constexpr int32_t kAdpcmBlockSize = 16;
	static constexpr int32_t kAdpcmSamplesPerBlock = 28;
	static constexpr int32_t kRamInstrumentBase = 0x1000;
	static constexpr int32_t kDefaultLfoTickSamples = 611;

	using InstrumentData = FFTSpuInstrumentData;
	using Voice = FFTSpuVoiceRuntime;

	FFTSpuCoreRuntime();

	bool load_instruments(const std::vector<InstrumentData> &instruments, const uint8_t *adpcm_bank, int32_t adpcm_bank_size);
	void reset();
	void key_on(int32_t voice_idx, int32_t instrument_idx, int32_t pitch, int32_t vol_l, int32_t vol_r, int32_t adsr1, int32_t adsr2, bool reverb);
	void key_on_with_addresses(int32_t voice_idx, int32_t instrument_idx, int32_t pitch, int32_t vol_l, int32_t vol_r,
			int32_t adsr1, int32_t adsr2, int32_t start_addr, int32_t loop_addr, bool reverb);
	void key_off(int32_t voice_idx);
	void set_voice_pitch(int32_t voice_idx, int32_t raw_pitch);
	void set_voice_pre_pitch(int32_t voice_idx, int32_t pre_pitch);
	void set_voice_adsr1_low(int32_t voice_idx, int32_t nibble);
	void set_voice_mix_controls(int32_t voice_idx, int32_t volume_base, int32_t velocity_gain,
			int32_t pan_base, int32_t master_gain, int32_t global_pan);
	void init_voice_pitch_lfo(int32_t voice_idx, int32_t count, int32_t signed_step, int32_t rate_reload);
	void clear_voice_pitch_lfo(int32_t voice_idx);
	void set_voice_pitch_lfo_depth(int32_t voice_idx, int32_t depth, int32_t depth_delta);
	void init_voice_volume_lfo(int32_t voice_idx, int32_t count, int32_t signed_step, int32_t rate_reload);
	void clear_voice_volume_lfo(int32_t voice_idx);
	void set_voice_volume_lfo_depth(int32_t voice_idx, int32_t depth, int32_t depth_delta);
	void set_lfo_tick_samples(int32_t samples);
	int32_t lfo_tick_samples() const { return lfo_tick_samples_; }
	void set_lfo_pitch_bias_enabled(bool enabled) { lfo_pitch_bias_enabled_ = enabled; }
	bool lfo_pitch_bias_enabled() const { return lfo_pitch_bias_enabled_; }
	void tick_pitch_lfo_all_voices();
	void tick_volume_lfo_all_voices();
	void apply_volume_lfo_consumer_all_voices();
	bool advance_lfo_tick();
	FFTSpuFrameRenderResult render_mix_frame(int32_t target_voice_idx);
	std::vector<int16_t> render_interleaved_pcm16(int32_t frame_count);
	int32_t active_voice_count() const;
	void set_reverb_enabled(bool enabled);
	bool reverb_enabled() const;
	void set_reverb_algorithm(FFTSpuReverbAlgorithm algorithm);
	FFTSpuReverbAlgorithm reverb_algorithm() const;
	const char *reverb_algorithm_name() const;
	void set_reverb_buffer_start(int32_t addr);
	int32_t reverb_buffer_start() const;
	void set_reverb_curr_addr(int32_t addr);
	int32_t reverb_curr_addr() const;
	void reset_reverb_state();
	bool next_reverb_mix_odd_branch() const;
	std::array<int32_t, 2> mix_reverb(int32_t input_l, int32_t input_r, FFTSpuReverbDebugSnapshot *debug_snapshot);

	const std::vector<InstrumentData> &instruments() const { return instruments_; }
	const std::vector<Voice> &voices() const { return voices_; }
	std::vector<Voice> &voices_mut() { return voices_; }
	const std::vector<FFTSpuVoiceMixResult> &frame_mix_results() const { return frame_mix_results_; }

private:
	std::vector<InstrumentData> instruments_;
	std::vector<uint8_t> spu_ram_;
	std::vector<Voice> voices_;
	std::vector<FFTSpuVoiceMixResult> frame_mix_results_;
	std::unique_ptr<FFTSpuReverbModel> reverb_;
	int32_t lfo_tick_samples_ = kDefaultLfoTickSamples;
	int32_t lfo_tick_sample_counter_ = 0;
	bool lfo_pitch_bias_enabled_ = true;
};

}  // namespace fftshared

#endif  // FFT_SPU_CORE_RUNTIME_H
