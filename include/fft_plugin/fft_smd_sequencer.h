#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_inspector.h"
#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_spu_core.h"
#include "fft_plugin/fft_waveset_service.h"

namespace fftshared {
class FFTSmdSequencerCore;
}

namespace fftplugin {

class FFTSpuPreviewCore;

class FFTSmdSequencer {
public:
    FFTSmdSequencer(IFFTSpuCore* spu_core, const IFFTWavesetService* waveset_service);
    ~FFTSmdSequencer();

    bool load_smd(const FFTSmdFile& smd, std::string* error_message = nullptr);
    bool tick();
    std::vector<int16_t> render_tick_pcm16();
    std::vector<int16_t> render_frames_only_pcm16(int32_t frame_count);
    bool has_active_audio() const;
    bool all_done() const;
    void set_track_muted(int32_t track_idx, bool muted);
    void set_track_soloed(int32_t track_idx, bool soloed);
    bool track_muted(int32_t track_idx) const;
    bool track_soloed(int32_t track_idx) const;
    FFTSmdSongPresentation build_playback_presentation(
        const FFTSmdFile& smd,
        int32_t max_ticks = 16384,
        int32_t max_trace_events = 16384
    ) const;

    double tempo_bpm() const;
    double samples_per_tick() const;
    double tick_accumulator() const;
    int32_t total_ticks() const;
    std::vector<int32_t> source_cursor_ticks() const;

    std::vector<bool> track_muted_;
    std::vector<bool> track_soloed_;

    IFFTSpuCore* spu_core_ = nullptr;
    FFTSpuPreviewCore* preview_core_ = nullptr;
    const IFFTWavesetService* waveset_service_ = nullptr;
    std::unique_ptr<fftshared::FFTSmdSequencerCore> shared_core_;
};

}  // namespace fftplugin
