#pragma once

#include <cstdint>

#include "fft_plugin/fft_audio_backend.h"

namespace fftplugin {

struct FFTPreviewBackendConfig {
    int32_t sample_rate = 44100;
    int32_t max_voices = 24;
    bool reverb_enabled = true;
    int32_t lfo_tick_samples = 611;
};

class IFFTSpuPreviewBackend : public IFFTPreviewBackend {
public:
    ~IFFTSpuPreviewBackend() override = default;

    virtual void reset_preview_state() = 0;
    virtual void configure_preview(const FFTPreviewBackendConfig& config) = 0;
    virtual int32_t active_voice_count() const = 0;
};

}  // namespace fftplugin
