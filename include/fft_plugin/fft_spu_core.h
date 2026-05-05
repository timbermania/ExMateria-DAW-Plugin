#pragma once

#include <cstdint>
#include <vector>

namespace fftplugin {

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
    int32_t start_sample_skip = 0;
};

struct FFTSpuLoadResult {
    bool ok = false;
    int32_t instrument_count = 0;
};

struct FFTSpuVoiceStartRequest {
    int32_t voice_index = 0;
    int32_t instrument_index = 0;
    int32_t pitch = 0;
    int32_t left_volume = 0x3FFF;
    int32_t right_volume = 0x3FFF;
    int32_t adsr1 = 0;
    int32_t adsr2 = 0;
    bool reverb = false;
};

class IFFTSpuCore {
public:
    virtual ~IFFTSpuCore() = default;

    virtual FFTSpuLoadResult load_instruments(
        const std::vector<FFTSpuInstrumentData>& instruments,
        const std::vector<uint8_t>& adpcm_bank
    ) = 0;

    virtual void reset() = 0;
    virtual void key_on(const FFTSpuVoiceStartRequest& request) = 0;
    virtual void key_off(int32_t voice_index) = 0;
    virtual void set_voice_pre_pitch(int32_t voice_index, int32_t pre_pitch) = 0;
    virtual void set_voice_pitch(int32_t voice_index, int32_t raw_pitch) = 0;
    virtual void init_voice_pitch_lfo(
        int32_t voice_index,
        int32_t count,
        int32_t signed_step,
        int32_t rate_reload
    ) = 0;
    virtual void clear_voice_pitch_lfo(int32_t voice_index) = 0;
    virtual void set_voice_pitch_lfo_depth(
        int32_t voice_index,
        int32_t depth,
        int32_t depth_delta
    ) = 0;
    virtual void set_reverb_enabled(bool enabled) = 0;
    virtual void set_reverb_buffer_start(int32_t address) = 0;
    virtual void set_lfo_tick_samples(int32_t samples) = 0;
    virtual std::vector<int16_t> render_interleaved_pcm16(int32_t frame_count) = 0;
    virtual int32_t active_voice_count() const = 0;
};

}  // namespace fftplugin
