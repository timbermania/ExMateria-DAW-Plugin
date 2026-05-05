#include "fft_plugin/fft_spu_preview_core.h"

#include <algorithm>
#include <vector>

#include "../vendor/exmateria-spu-core/fft_spu_core_runtime.h"
#include "../vendor/exmateria-spu-core/fft_spu_voice_runtime.h"

namespace fftplugin {

namespace {

fftshared::FFTSpuInstrumentData to_shared_instrument(const FFTSpuInstrumentData& instrument) {
    return fftshared::FFTSpuInstrumentData {
        .is_null = instrument.is_null,
        .fine_tune = instrument.fine_tune,
        .adsr1 = instrument.adsr1,
        .adsr2 = instrument.adsr2,
        .sample_offset = instrument.sample_offset,
        .sample_size = instrument.sample_size,
        .loop_start = instrument.loop_start,
        .loop_offset_bytes = instrument.loop_offset_bytes,
        .has_explicit_loop_start = instrument.has_explicit_loop_start,
        .has_loop_repeat = instrument.has_loop_repeat,
        .start_offset_bytes = instrument.start_offset_bytes,
    };
}

}  // namespace

FFTSpuPreviewCore::FFTSpuPreviewCore()
    : runtime_(std::make_unique<fftshared::FFTSpuCoreRuntime>()) {}

FFTSpuPreviewCore::~FFTSpuPreviewCore() = default;

FFTSpuLoadResult FFTSpuPreviewCore::load_instruments(
    const std::vector<FFTSpuInstrumentData>& instruments,
    const std::vector<uint8_t>& adpcm_bank
) {
    std::vector<fftshared::FFTSpuInstrumentData> shared_instruments;
    shared_instruments.reserve(instruments.size());
    for (const FFTSpuInstrumentData& instrument : instruments) {
        shared_instruments.push_back(to_shared_instrument(instrument));
    }

    const bool ok = runtime_->load_instruments(
        shared_instruments,
        adpcm_bank.empty() ? nullptr : adpcm_bank.data(),
        static_cast<int32_t>(adpcm_bank.size())
    );
    return FFTSpuLoadResult {
        .ok = ok,
        .instrument_count = static_cast<int32_t>(shared_instruments.size()),
    };
}

void FFTSpuPreviewCore::reset() {
    runtime_->reset();
}

void FFTSpuPreviewCore::key_on(const FFTSpuVoiceStartRequest& request) {
    runtime_->key_on(
        request.voice_index,
        request.instrument_index,
        request.pitch,
        request.left_volume,
        request.right_volume,
        request.adsr1,
        request.adsr2,
        request.reverb
    );
}

void FFTSpuPreviewCore::key_off(int32_t voice_index) {
    runtime_->key_off(voice_index);
}

void FFTSpuPreviewCore::set_voice_pre_pitch(int32_t voice_index, int32_t pre_pitch) {
    runtime_->set_voice_pre_pitch(voice_index, pre_pitch);
}

void FFTSpuPreviewCore::set_voice_pitch(int32_t voice_index, int32_t raw_pitch) {
    runtime_->set_voice_pitch(voice_index, raw_pitch);
}

void FFTSpuPreviewCore::init_voice_pitch_lfo(
    int32_t voice_index,
    int32_t count,
    int32_t signed_step,
    int32_t rate_reload
) {
    runtime_->init_voice_pitch_lfo(voice_index, count, signed_step, rate_reload);
}

void FFTSpuPreviewCore::clear_voice_pitch_lfo(int32_t voice_index) {
    runtime_->clear_voice_pitch_lfo(voice_index);
}

void FFTSpuPreviewCore::set_voice_pitch_lfo_depth(
    int32_t voice_index,
    int32_t depth,
    int32_t depth_delta
) {
    runtime_->set_voice_pitch_lfo_depth(voice_index, depth, depth_delta);
}

void FFTSpuPreviewCore::set_reverb_enabled(bool enabled) {
    runtime_->set_reverb_enabled(enabled);
}

void FFTSpuPreviewCore::set_reverb_buffer_start(int32_t address) {
    runtime_->set_reverb_buffer_start(address);
    runtime_->set_reverb_curr_addr(address);
}

void FFTSpuPreviewCore::set_lfo_tick_samples(int32_t samples) {
    runtime_->set_lfo_tick_samples(std::max(1, samples));
}

std::vector<int16_t> FFTSpuPreviewCore::render_interleaved_pcm16(int32_t frame_count) {
    return runtime_->render_interleaved_pcm16(frame_count);
}

int32_t FFTSpuPreviewCore::active_voice_count() const {
    return runtime_->active_voice_count();
}

fftshared::FFTSpuCoreRuntime& FFTSpuPreviewCore::runtime() {
    return *runtime_;
}

const fftshared::FFTSpuCoreRuntime& FFTSpuPreviewCore::runtime() const {
    return *runtime_;
}

}  // namespace fftplugin
