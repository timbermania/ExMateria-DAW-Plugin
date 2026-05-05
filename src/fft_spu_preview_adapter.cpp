#include "fft_plugin/fft_spu_preview_adapter.h"

#include <algorithm>

namespace fftplugin {

FFTSpuPreviewAdapter::FFTSpuPreviewAdapter(IFFTSpuCore* spu_core)
    : spu_core_(spu_core) {
    configure_preview(config_);
}

void FFTSpuPreviewAdapter::set_instrument_bank(const IFFTInstrumentBank* instrument_bank) {
    instrument_bank_ = instrument_bank;
}

void FFTSpuPreviewAdapter::set_waveset_service(const IFFTWavesetService* waveset_service) {
    waveset_service_ = waveset_service;
    loaded_waveset_path_.clear();
}

void FFTSpuPreviewAdapter::note_on(const FFTPreviewNoteRequest& note_request) {
    if (spu_core_ == nullptr || waveset_service_ == nullptr || !sync_waveset_into_spu()) {
        return;
    }

    const auto instrument_info = waveset_service_->instrument_info(note_request.instrument_id);
    if (!instrument_info.has_value() || instrument_info->is_null) {
        return;
    }

    const int32_t voice_index = allocate_voice(note_request.midi_note);
    const int32_t pre_pitch = fft_pre_pitch_from_note(note_request.midi_note, instrument_info->fine_tune);
    const int32_t raw_pitch = fft_raw_pitch_from_pre_pitch(pre_pitch);
    const int32_t default_volume = velocity_to_volume(note_request.velocity);
    const int32_t left_volume = note_request.left_volume_override.value_or(default_volume);
    const int32_t right_volume = note_request.right_volume_override.value_or(default_volume);
    const int32_t adsr1 = note_request.adsr1_override.value_or(instrument_info->adsr1);
    const int32_t adsr2 = note_request.adsr2_override.value_or(instrument_info->adsr2);

    spu_core_->set_voice_pre_pitch(voice_index, pre_pitch);
    spu_core_->key_on(FFTSpuVoiceStartRequest {
        .voice_index = voice_index,
        .instrument_index = note_request.instrument_id,
        .pitch = raw_pitch,
        .left_volume = left_volume,
        .right_volume = right_volume,
        .adsr1 = adsr1,
        .adsr2 = adsr2,
        .reverb = config_.reverb_enabled,
    });
    active_voice_notes_[static_cast<size_t>(voice_index)] = note_request.midi_note;
}

void FFTSpuPreviewAdapter::note_off(int16_t midi_note) {
    if (spu_core_ == nullptr) {
        return;
    }

    for (size_t voice_index = 0; voice_index < active_voice_notes_.size(); ++voice_index) {
        if (active_voice_notes_[voice_index] == midi_note) {
            spu_core_->key_off(static_cast<int32_t>(voice_index));
            active_voice_notes_[voice_index] = -1;
        }
    }
}

void FFTSpuPreviewAdapter::all_notes_off() {
    if (spu_core_ == nullptr) {
        return;
    }

    for (size_t voice_index = 0; voice_index < active_voice_notes_.size(); ++voice_index) {
        if (active_voice_notes_[voice_index] >= 0) {
            spu_core_->key_off(static_cast<int32_t>(voice_index));
            active_voice_notes_[voice_index] = -1;
        }
    }
}

void FFTSpuPreviewAdapter::reset_preview_state() {
    if (spu_core_ != nullptr) {
        spu_core_->reset();
    }
    std::fill(active_voice_notes_.begin(), active_voice_notes_.end(), static_cast<int16_t>(-1));
    next_voice_index_ = 0;
}

void FFTSpuPreviewAdapter::configure_preview(const FFTPreviewBackendConfig& config) {
    config_ = config;
    active_voice_notes_.assign(static_cast<size_t>(std::max(1, config_.max_voices)), static_cast<int16_t>(-1));
    next_voice_index_ = 0;

    if (spu_core_ != nullptr) {
        spu_core_->set_reverb_enabled(config_.reverb_enabled);
        spu_core_->set_lfo_tick_samples(config_.lfo_tick_samples);
    }
}

int32_t FFTSpuPreviewAdapter::active_voice_count() const {
    return spu_core_ != nullptr ? spu_core_->active_voice_count() : 0;
}

bool FFTSpuPreviewAdapter::sync_waveset_into_spu() {
    if (spu_core_ == nullptr || waveset_service_ == nullptr || !waveset_service_->is_loaded()) {
        return false;
    }
    if (loaded_waveset_path_ == waveset_service_->loaded_path()) {
        return true;
    }

    const FFTSpuLoadResult result = spu_core_->load_instruments(
        waveset_service_->spu_instruments(),
        waveset_service_->adpcm_bank()
    );
    if (!result.ok) {
        return false;
    }

    loaded_waveset_path_ = waveset_service_->loaded_path();
    return true;
}

int32_t FFTSpuPreviewAdapter::allocate_voice(int16_t midi_note) {
    for (size_t voice_index = 0; voice_index < active_voice_notes_.size(); ++voice_index) {
        if (active_voice_notes_[voice_index] == midi_note) {
            spu_core_->key_off(static_cast<int32_t>(voice_index));
            active_voice_notes_[voice_index] = -1;
            return static_cast<int32_t>(voice_index);
        }
    }

    const int32_t voice_index = next_voice_index_;
    next_voice_index_ = (next_voice_index_ + 1) % static_cast<int32_t>(active_voice_notes_.size());
    if (active_voice_notes_[static_cast<size_t>(voice_index)] >= 0) {
        spu_core_->key_off(voice_index);
    }
    return voice_index;
}

int32_t FFTSpuPreviewAdapter::velocity_to_volume(int16_t velocity) const {
    const int32_t clamped = std::clamp(static_cast<int32_t>(velocity), 0, 127);
    return std::min(0x3FFF, clamped << 7);
}

}  // namespace fftplugin
