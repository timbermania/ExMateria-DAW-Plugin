#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "fft_plugin/fft_spu_core.h"

namespace fftshared {
class FFTSpuCoreRuntime;
}

namespace fftplugin {

class FFTSpuPreviewCore final : public IFFTSpuCore {
public:
    FFTSpuPreviewCore();
    ~FFTSpuPreviewCore() override;

    FFTSpuLoadResult load_instruments(
        const std::vector<FFTSpuInstrumentData>& instruments,
        const std::vector<uint8_t>& adpcm_bank
    ) override;

    void reset() override;
    void key_on(const FFTSpuVoiceStartRequest& request) override;
    void key_off(int32_t voice_index) override;
    void set_voice_pre_pitch(int32_t voice_index, int32_t pre_pitch) override;
    void set_voice_pitch(int32_t voice_index, int32_t raw_pitch) override;
    void init_voice_pitch_lfo(int32_t voice_index, int32_t count, int32_t signed_step, int32_t rate_reload) override;
    void clear_voice_pitch_lfo(int32_t voice_index) override;
    void set_voice_pitch_lfo_depth(int32_t voice_index, int32_t depth, int32_t depth_delta) override;
    void set_reverb_enabled(bool enabled) override;
    void set_reverb_buffer_start(int32_t address) override;
    void set_lfo_tick_samples(int32_t samples) override;
    std::vector<int16_t> render_interleaved_pcm16(int32_t frame_count) override;
    int32_t active_voice_count() const override;

    fftshared::FFTSpuCoreRuntime& runtime();
    const fftshared::FFTSpuCoreRuntime& runtime() const;

private:
    std::unique_ptr<fftshared::FFTSpuCoreRuntime> runtime_;
};

}  // namespace fftplugin
