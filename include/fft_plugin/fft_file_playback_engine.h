#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_inspector.h"
#include "fft_plugin/fft_smd_sequencer.h"
#include "fft_plugin/fft_spu_preview_core.h"
#include "fft_plugin/fft_waveset_file_service.h"

namespace fftplugin {

struct FFTFilePlaybackLoadResult {
    bool ok = false;
    std::string message;
    int32_t instrument_count = 0;
    int32_t track_count = 0;
    double tempo_bpm = 0.0;
};

class FFTFilePlaybackEngine {
public:
    static constexpr int32_t kSampleRate = 44100;

    FFTFilePlaybackEngine();

    FFTFilePlaybackLoadResult load_waveset_path(const std::string& waveset_path);
    FFTFilePlaybackLoadResult load_smd_path(const std::string& smd_path);
    bool load_compiled_smd_document(
        const FFTSmdCompiledDocument& document,
        const std::string& smd_path,
        std::string* error_message = nullptr);
    FFTFilePlaybackLoadResult rebuild_playback();

    void play();
    void stop(bool rewind_to_start = true);
    bool rewind();
    bool seek_to_tick(int32_t target_tick);

    std::vector<int16_t> render_interleaved_pcm16(int32_t frame_count);
    std::vector<float> render_interleaved_f32(int32_t frame_count);

    bool has_waveset() const;
    bool has_smd() const;
    bool ready() const;
    bool playing() const;
    bool finished() const;
    int32_t active_voice_count() const;
    double tempo_bpm() const;
    int32_t current_playback_tick() const;
    std::vector<int32_t> current_source_track_ticks() const;
    void set_track_muted(int32_t track_idx, bool muted);
    void set_track_soloed(int32_t track_idx, bool soloed);
    bool track_muted(int32_t track_idx) const;
    bool track_soloed(int32_t track_idx) const;

    bool track_source_opcode_disabled(int32_t track_idx, int32_t source_event_index) const;

    const std::string& waveset_path() const;
    const std::string& smd_path() const;
    std::string bank_name() const;
    const FFTWavesetFileService& waveset_service() const { return waveset_service_; }
    const std::string& last_error() const;
    const std::optional<FFTSmdFile>& smd_file() const;
    const std::unordered_set<uint64_t>& disabled_source_opcodes() const;
    FFTSmdSongPresentation build_playback_song_presentation(
        int32_t max_ticks = 16384,
        int32_t max_trace_events = 16384
    ) const;

private:
    FFTFilePlaybackLoadResult make_result(bool ok, std::string message) const;
    bool rebuild_playback_internal(std::string* error_message);
    void append_pcm(std::vector<int16_t>&& pcm);
    int32_t queued_frame_count() const;

    FFTWavesetFileService waveset_service_;
    FFTSpuPreviewCore spu_core_;
    FFTSmdSequencer sequencer_;
    std::optional<FFTSmdFile> smd_file_;
    std::string waveset_path_;
    std::string smd_path_;
    std::string last_error_;
    std::deque<int16_t> pcm_queue_;
    std::unordered_set<uint64_t> disabled_source_opcodes_;
    bool playback_ready_ = false;
    bool playing_ = false;
    bool finished_ = false;
};

}  // namespace fftplugin
