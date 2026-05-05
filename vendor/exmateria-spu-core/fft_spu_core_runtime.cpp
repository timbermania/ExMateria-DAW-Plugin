#include "fft_spu_core_runtime.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include "fft_spu_core_state_tools.h"
#include "fft_spu_pitch_runtime.h"

namespace fftshared {

namespace {

void fft_apply_voice_volume_lfo_consumer(FFTSpuCoreRuntime::Voice &voice) {
	if (voice.mix_velocity_gain <= 0 || voice.mix_master_gain <= 0) {
		return;
	}
	const int32_t volume_bias = voice.lfo_blocks[1].consumer_output;
	const int32_t pan_bias = voice.lfo_blocks[2].consumer_output;
	const int32_t mixed_volume = std::clamp(voice.mix_volume_base + volume_bias, 0, 0x7FFF);
	int32_t gain = (voice.mix_velocity_gain * mixed_volume) >> 15;
	gain = (voice.mix_master_gain * gain) >> 16;

	const int32_t mixed_pan = std::clamp(voice.mix_pan_base + pan_bias + voice.mix_global_pan, 0, 0x7F00);
	int32_t vol_l;
	int32_t vol_r;
	if (mixed_pan < 0x4000) {
		vol_l = ((0x7F00 - ((mixed_pan * 0x2500) >> 14)) * gain) >> 15;
		vol_r = (((mixed_pan * 0x5A00) >> 14) * gain) >> 15;
	} else {
		const int32_t mirror = 0x8000 - mixed_pan;
		vol_l = (((mirror * 0x5A00) >> 14) * gain) >> 15;
		vol_r = ((0x7F00 - ((mirror * 0x2500) >> 14)) * gain) >> 15;
	}
	voice.left_volume = std::clamp(vol_l, 0, FFTSpuCoreRuntime::kVolumeMax);
	voice.right_volume = std::clamp(vol_r, 0, FFTSpuCoreRuntime::kVolumeMax);
}

}  // namespace

FFTSpuCoreRuntime::FFTSpuCoreRuntime() {
	spu_ram_.resize(kSpuRamSize);
	voices_.resize(kNumVoices);
	reverb_ = std::make_unique<FFTSpuReverb>();
	reset();
}

bool FFTSpuCoreRuntime::load_instruments(const std::vector<InstrumentData> &instruments, const uint8_t *adpcm_bank, int32_t adpcm_bank_size) {
	instruments_ = instruments;
	fft_load_spu_adpcm_bank(spu_ram_, adpcm_bank, adpcm_bank_size, kSpuRamSize, kRamInstrumentBase);
	reset();
	return true;
}

void FFTSpuCoreRuntime::reset() {
	std::fill(spu_ram_.begin(), spu_ram_.begin() + std::min<int32_t>(kRamInstrumentBase, static_cast<int32_t>(spu_ram_.size())), 0);
	fft_reset_voice_states(voices_);
	reverb_->reset_state();
	lfo_tick_sample_counter_ = 0;
}

void FFTSpuCoreRuntime::key_on(int32_t voice_idx, int32_t instrument_idx, int32_t pitch, int32_t vol_l, int32_t vol_r, int32_t adsr1, int32_t adsr2, bool reverb) {
	if (instrument_idx < 0 || instrument_idx >= static_cast<int32_t>(instruments_.size())) {
		if (voice_idx >= 0 && voice_idx < kNumVoices) {
			voices_[voice_idx].on = false;
		}
		return;
	}
	const InstrumentData &inst = instruments_[instrument_idx];
	const FFTSpuVoiceAddresses addresses = fft_compute_voice_addresses(
			inst, kRamInstrumentBase, kSpuRamSize, kAdpcmBlockSize, 0, 0, false);
	key_on_with_addresses(voice_idx, instrument_idx, pitch, vol_l, vol_r, adsr1, adsr2, addresses.start_addr, addresses.loop_addr, reverb);
}

void FFTSpuCoreRuntime::key_on_with_addresses(int32_t voice_idx, int32_t instrument_idx, int32_t pitch, int32_t vol_l, int32_t vol_r,
		int32_t adsr1, int32_t adsr2, int32_t start_addr, int32_t loop_addr, bool reverb) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	if (instrument_idx < 0 || instrument_idx >= static_cast<int32_t>(instruments_.size())) {
		voices_[voice_idx].on = false;
		return;
	}
	const InstrumentData &inst = instruments_[instrument_idx];
	if (!fft_spu_instrument_playable(inst)) {
		voices_[voice_idx].on = false;
		return;
	}
	Voice &voice = voices_[voice_idx];
	const FFTSpuVoiceAddresses addresses = fft_compute_voice_addresses(
			inst, kRamInstrumentBase, kSpuRamSize, kAdpcmBlockSize, start_addr, loop_addr, true);
	const bool preserve_repeat_addr = voice.on && voice.start_addr == addresses.start_addr && voice.loop_addr > addresses.loop_addr;
	fft_prepare_voice_for_key_on(voice, instrument_idx, addresses, pitch, vol_l, vol_r, adsr1, adsr2,
			reverb, preserve_repeat_addr, kVolumeMax, kPitchToSincShift, kAdpcmSamplesPerBlock);
}

void FFTSpuCoreRuntime::key_off(int32_t voice_idx) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	voices_[voice_idx].stop_requested = true;
}

void FFTSpuCoreRuntime::set_voice_pitch(int32_t voice_idx, int32_t raw_pitch) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_set_voice_pitch(voices_[voice_idx], raw_pitch, kVolumeMax, kPitchToSincShift);
}

void FFTSpuCoreRuntime::set_voice_pre_pitch(int32_t voice_idx, int32_t pre_pitch) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_set_voice_pre_pitch(voices_[voice_idx], pre_pitch);
}

void FFTSpuCoreRuntime::set_voice_adsr1_low(int32_t voice_idx, int32_t nibble) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	Voice &voice = voices_[voice_idx];
	voice.adsr1 = (voice.adsr1 & ~0xF) | (nibble & 0xF);
	voice.adsr.set_from_regs(voice.adsr1, voice.adsr2);
}

void FFTSpuCoreRuntime::set_voice_mix_controls(int32_t voice_idx, int32_t volume_base, int32_t velocity_gain,
		int32_t pan_base, int32_t master_gain, int32_t global_pan) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	Voice &voice = voices_[voice_idx];
	voice.mix_volume_base = volume_base;
	voice.mix_velocity_gain = velocity_gain;
	voice.mix_pan_base = pan_base;
	voice.mix_master_gain = master_gain;
	voice.mix_global_pan = global_pan;
}

void FFTSpuCoreRuntime::init_voice_pitch_lfo(int32_t voice_idx, int32_t count, int32_t signed_step, int32_t rate_reload) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_init_pitch_lfo(voices_[voice_idx].lfo_blocks[0], count, signed_step, rate_reload);
}

void FFTSpuCoreRuntime::clear_voice_pitch_lfo(int32_t voice_idx) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_clear_pitch_lfo(voices_[voice_idx].lfo_blocks[0]);
}

void FFTSpuCoreRuntime::set_voice_pitch_lfo_depth(int32_t voice_idx, int32_t depth, int32_t depth_delta) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_set_pitch_lfo_depth(voices_[voice_idx].lfo_blocks[0], depth, depth_delta);
}

void FFTSpuCoreRuntime::init_voice_volume_lfo(int32_t voice_idx, int32_t count, int32_t signed_step, int32_t rate_reload) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_init_volume_lfo(voices_[voice_idx].lfo_blocks[1], count, signed_step, rate_reload);
}

void FFTSpuCoreRuntime::clear_voice_volume_lfo(int32_t voice_idx) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_clear_volume_lfo(voices_[voice_idx].lfo_blocks[1]);
}

void FFTSpuCoreRuntime::set_voice_volume_lfo_depth(int32_t voice_idx, int32_t depth, int32_t depth_delta) {
	if (voice_idx < 0 || voice_idx >= kNumVoices) {
		return;
	}
	fft_set_volume_lfo_depth(voices_[voice_idx].lfo_blocks[1], depth, depth_delta);
}

void FFTSpuCoreRuntime::set_lfo_tick_samples(int32_t samples) {
	lfo_tick_samples_ = std::max(1, samples);
}

void FFTSpuCoreRuntime::tick_pitch_lfo_all_voices() {
	fft_tick_pitch_lfo_all_voices(voices_);
}

void FFTSpuCoreRuntime::tick_volume_lfo_all_voices() {
	for (Voice &voice : voices_) {
		fft_tick_volume_lfo(voice.lfo_blocks[1]);
	}
}

void FFTSpuCoreRuntime::apply_volume_lfo_consumer_all_voices() {
	for (Voice &voice : voices_) {
		if (!voice.on) {
			continue;
		}
		if (!voice.lfo_blocks[1].enabled && !voice.lfo_blocks[2].enabled) {
			continue;
		}
		fft_apply_voice_volume_lfo_consumer(voice);
	}
}

bool FFTSpuCoreRuntime::advance_lfo_tick() {
	if (!fft_advance_lfo_tick_counter(lfo_tick_sample_counter_, lfo_tick_samples_)) {
		return false;
	}
	tick_pitch_lfo_all_voices();
	tick_volume_lfo_all_voices();
	apply_volume_lfo_consumer_all_voices();
	return true;
}

FFTSpuFrameRenderResult FFTSpuCoreRuntime::render_mix_frame(int32_t target_voice_idx) {
	FFTSpuFrameRenderResult frame_result;
	fft_render_mix_frame(voices_, lfo_pitch_bias_enabled_, kVolumeMax, kPitchToSincShift,
			kVolumeDivisor, spu_ram_.data(), kSpuRamSize, target_voice_idx, frame_mix_results_, frame_result);
	return frame_result;
}

std::vector<int16_t> FFTSpuCoreRuntime::render_interleaved_pcm16(int32_t frame_count) {
	std::vector<int16_t> output(static_cast<size_t>(std::max(0, frame_count)) * 2, 0);

	for (int32_t frame = 0; frame < frame_count; ++frame) {
		advance_lfo_tick();

		const FFTSpuFrameRenderResult frame_result = render_mix_frame(-1);
		const std::array<int32_t, 2> reverb_out = mix_reverb(frame_result.rvb_in_l, frame_result.rvb_in_r, nullptr);
		output[static_cast<size_t>(frame) * 2] = static_cast<int16_t>(std::clamp(frame_result.sum_l + reverb_out[0], -32767, 32767));
		output[static_cast<size_t>(frame) * 2 + 1] = static_cast<int16_t>(std::clamp(frame_result.sum_r + reverb_out[1], -32767, 32767));
	}

	return output;
}

int32_t FFTSpuCoreRuntime::active_voice_count() const {
	return fft_count_active_voices(voices_);
}

void FFTSpuCoreRuntime::set_reverb_enabled(bool enabled) {
	reverb_->set_enabled(enabled);
}

bool FFTSpuCoreRuntime::reverb_enabled() const {
	return reverb_->enabled();
}

void FFTSpuCoreRuntime::set_reverb_algorithm(FFTSpuReverbAlgorithm algorithm) {
	reverb_->set_algorithm(algorithm);
}

FFTSpuReverbAlgorithm FFTSpuCoreRuntime::reverb_algorithm() const {
	return reverb_->algorithm();
}

const char *FFTSpuCoreRuntime::reverb_algorithm_name() const {
	return reverb_->algorithm_name();
}

void FFTSpuCoreRuntime::set_reverb_buffer_start(int32_t addr) {
	reverb_->set_buffer_start(addr);
}

int32_t FFTSpuCoreRuntime::reverb_buffer_start() const {
	return reverb_->buffer_start();
}

void FFTSpuCoreRuntime::set_reverb_curr_addr(int32_t addr) {
	reverb_->set_curr_addr(addr);
}

int32_t FFTSpuCoreRuntime::reverb_curr_addr() const {
	return reverb_->curr_addr();
}

void FFTSpuCoreRuntime::reset_reverb_state() {
	reverb_->reset_state();
}

bool FFTSpuCoreRuntime::next_reverb_mix_odd_branch() const {
	return reverb_->next_mix_odd_branch();
}

std::array<int32_t, 2> FFTSpuCoreRuntime::mix_reverb(
		int32_t input_l, int32_t input_r, FFTSpuReverbDebugSnapshot *debug_snapshot) {
	return reverb_->mix(input_l, input_r, debug_snapshot);
}

}  // namespace fftshared
