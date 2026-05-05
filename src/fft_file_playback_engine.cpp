#include "fft_plugin/fft_file_playback_engine.h"

#include <algorithm>
#include <utility>

#include "fft_plugin/fft_smd_opcodes.h"

namespace fftplugin {

namespace {

uint64_t source_event_key(int32_t track_idx, int32_t source_event_index) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(track_idx)) << 32U) |
        static_cast<uint32_t>(source_event_index);
}

FFTSmdFile filtered_smd_without_disabled_opcodes(
    const FFTSmdFile& smd,
    const std::unordered_set<uint64_t>& disabled_source_opcodes
) {
    if (disabled_source_opcodes.empty()) {
        return smd;
    }

    FFTSmdFile filtered = smd;
    for (size_t track_idx = 0; track_idx < filtered.track_events.size(); ++track_idx) {
        std::vector<FFTSmdTrackEvent> filtered_track;
        filtered_track.reserve(filtered.track_events[track_idx].size());
        for (size_t event_idx = 0; event_idx < filtered.track_events[track_idx].size(); ++event_idx) {
            const FFTSmdTrackEvent& event = filtered.track_events[track_idx][event_idx];
            const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&event);
            if (opcode != nullptr &&
                disabled_source_opcodes.contains(source_event_key(
                    static_cast<int32_t>(track_idx),
                    static_cast<int32_t>(event_idx)))) {
                continue;
            }
            filtered_track.push_back(event);
        }
        filtered.track_events[track_idx] = std::move(filtered_track);
    }
    return filtered;
}

}  // namespace

FFTFilePlaybackEngine::FFTFilePlaybackEngine()
    : sequencer_(&spu_core_, &waveset_service_) {}

FFTFilePlaybackLoadResult FFTFilePlaybackEngine::load_waveset_path(const std::string& waveset_path) {
    const FFTWavesetLoadResult load_result = waveset_service_.load_from_path(waveset_path);
    if (!load_result.ok) {
        last_error_ = load_result.message;
        playback_ready_ = false;
        playing_ = false;
        finished_ = false;
        pcm_queue_.clear();
        return make_result(false, last_error_);
    }

    waveset_path_ = waveset_path;
    last_error_.clear();
    if (smd_file_.has_value()) {
        std::string rebuild_error;
        if (!rebuild_playback_internal(&rebuild_error)) {
            last_error_ = rebuild_error;
            return make_result(false, last_error_);
        }
    }
    return make_result(true, "Loaded WAVESET");
}

FFTFilePlaybackLoadResult FFTFilePlaybackEngine::load_smd_path(const std::string& smd_path) {
    std::string error_message;
    const std::optional<FFTSmdFile> loaded_smd = load_smd_file(smd_path, &error_message);
    if (!loaded_smd.has_value()) {
        last_error_ = error_message;
        playback_ready_ = false;
        playing_ = false;
        finished_ = false;
        pcm_queue_.clear();
        return make_result(false, last_error_);
    }

    smd_path_ = smd_path;
    smd_file_ = loaded_smd;
    disabled_source_opcodes_.clear();
    last_error_.clear();
    if (waveset_service_.is_loaded()) {
        std::string rebuild_error;
        if (!rebuild_playback_internal(&rebuild_error)) {
            last_error_ = rebuild_error;
            return make_result(false, last_error_);
        }
    }
    return make_result(true, "Loaded SMD");
}

bool FFTFilePlaybackEngine::load_compiled_smd_document(
    const FFTSmdCompiledDocument& document,
    const std::string& smd_path,
    std::string* error_message
) {
    smd_path_ = smd_path;
    smd_file_ = document.smd;
    disabled_source_opcodes_ = document.disabled_opcode_keys;
    last_error_.clear();
    playback_ready_ = false;
    playing_ = false;
    finished_ = false;
    pcm_queue_.clear();

    if (waveset_service_.is_loaded()) {
        std::string rebuild_error;
        if (!rebuild_playback_internal(&rebuild_error)) {
            last_error_ = rebuild_error;
            if (error_message != nullptr) {
                *error_message = last_error_;
            }
            return false;
        }
    }

    return true;
}

FFTFilePlaybackLoadResult FFTFilePlaybackEngine::rebuild_playback() {
    std::string error_message;
    const bool ok = rebuild_playback_internal(&error_message);
    if (!ok) {
        last_error_ = error_message;
        return make_result(false, last_error_);
    }
    last_error_.clear();
    return make_result(true, "Playback rebuilt");
}

void FFTFilePlaybackEngine::play() {
    if (!playback_ready_) {
        std::string ignored_error;
        if (!rebuild_playback_internal(&ignored_error)) {
            last_error_ = ignored_error;
            return;
        }
    }
    playing_ = playback_ready_;
    finished_ = false;
}

void FFTFilePlaybackEngine::stop(bool rewind_to_start) {
    playing_ = false;
    finished_ = false;
    pcm_queue_.clear();
    if (rewind_to_start) {
        std::string ignored_error;
        rebuild_playback_internal(&ignored_error);
    }
}

bool FFTFilePlaybackEngine::rewind() {
    std::string error_message;
    const bool ok = rebuild_playback_internal(&error_message);
    if (!ok) {
        last_error_ = error_message;
        return false;
    }
    last_error_.clear();
    return true;
}

bool FFTFilePlaybackEngine::seek_to_tick(int32_t target_tick) {
    std::string error_message;
    if (!rebuild_playback_internal(&error_message)) {
        last_error_ = error_message;
        return false;
    }

    pcm_queue_.clear();
    const int32_t clamped_target = std::max(0, target_tick);
    while (sequencer_.total_ticks() < clamped_target) {
        if (!sequencer_.tick()) {
            break;
        }
        auto discarded_pcm = sequencer_.render_tick_pcm16();
        (void)discarded_pcm;
    }
    pcm_queue_.clear();
    playing_ = false;
    finished_ = false;
    last_error_.clear();
    return true;
}

std::vector<int16_t> FFTFilePlaybackEngine::render_interleaved_pcm16(int32_t frame_count) {
    if (frame_count <= 0) {
        return {};
    }

    std::vector<int16_t> output(static_cast<size_t>(frame_count) * 2U, 0);
    if (!playback_ready_ || (!playing_ && pcm_queue_.empty())) {
        return output;
    }

    while (queued_frame_count() < frame_count && playing_) {
        if (sequencer_.tick()) {
            append_pcm(sequencer_.render_tick_pcm16());
            continue;
        }

        if (sequencer_.has_active_audio()) {
            const int32_t frames_needed = std::max(frame_count - queued_frame_count(), 1);
            append_pcm(sequencer_.render_frames_only_pcm16(frames_needed));
            continue;
        }

        playing_ = false;
        finished_ = true;
    }

    const size_t sample_count = std::min(output.size(), pcm_queue_.size());
    for (size_t i = 0; i < sample_count; ++i) {
        output[i] = pcm_queue_.front();
        pcm_queue_.pop_front();
    }
    return output;
}

std::vector<float> FFTFilePlaybackEngine::render_interleaved_f32(int32_t frame_count) {
    const std::vector<int16_t> pcm16 = render_interleaved_pcm16(frame_count);
    std::vector<float> output;
    output.reserve(pcm16.size());
    for (const int16_t sample : pcm16) {
        output.push_back(static_cast<float>(sample) / 32768.0F);
    }
    return output;
}

bool FFTFilePlaybackEngine::has_waveset() const {
    return waveset_service_.is_loaded();
}

bool FFTFilePlaybackEngine::has_smd() const {
    return smd_file_.has_value();
}

bool FFTFilePlaybackEngine::ready() const {
    return playback_ready_;
}

bool FFTFilePlaybackEngine::playing() const {
    return playing_;
}

bool FFTFilePlaybackEngine::finished() const {
    return finished_;
}

int32_t FFTFilePlaybackEngine::active_voice_count() const {
    return sequencer_.has_active_audio() ? spu_core_.active_voice_count() : 0;
}

double FFTFilePlaybackEngine::tempo_bpm() const {
    return sequencer_.tempo_bpm();
}

int32_t FFTFilePlaybackEngine::current_playback_tick() const {
    return sequencer_.total_ticks();
}

std::vector<int32_t> FFTFilePlaybackEngine::current_source_track_ticks() const {
    return sequencer_.source_cursor_ticks();
}

void FFTFilePlaybackEngine::set_track_muted(int32_t track_idx, bool muted) {
    sequencer_.set_track_muted(track_idx, muted);
}

void FFTFilePlaybackEngine::set_track_soloed(int32_t track_idx, bool soloed) {
    sequencer_.set_track_soloed(track_idx, soloed);
}

bool FFTFilePlaybackEngine::track_muted(int32_t track_idx) const {
    return sequencer_.track_muted(track_idx);
}

bool FFTFilePlaybackEngine::track_soloed(int32_t track_idx) const {
    return sequencer_.track_soloed(track_idx);
}


bool FFTFilePlaybackEngine::track_source_opcode_disabled(int32_t track_idx, int32_t source_event_index) const {
    if (track_idx < 0 || source_event_index < 0) {
        return false;
    }
    return disabled_source_opcodes_.contains(source_event_key(track_idx, source_event_index));
}

const std::string& FFTFilePlaybackEngine::waveset_path() const {
    return waveset_path_;
}

const std::string& FFTFilePlaybackEngine::smd_path() const {
    return smd_path_;
}

std::string FFTFilePlaybackEngine::bank_name() const {
    return waveset_service_.bank_name();
}

const std::string& FFTFilePlaybackEngine::last_error() const {
    return last_error_;
}

const std::optional<FFTSmdFile>& FFTFilePlaybackEngine::smd_file() const {
    return smd_file_;
}

const std::unordered_set<uint64_t>& FFTFilePlaybackEngine::disabled_source_opcodes() const {
    return disabled_source_opcodes_;
}

FFTSmdSongPresentation FFTFilePlaybackEngine::build_playback_song_presentation(
    int32_t max_ticks,
    int32_t max_trace_events
) const {
    if (!smd_file_.has_value() || !waveset_service_.is_loaded()) {
        return {};
    }
    return sequencer_.build_playback_presentation(
        filtered_smd_without_disabled_opcodes(*smd_file_, disabled_source_opcodes_),
        max_ticks,
        max_trace_events);
}

FFTFilePlaybackLoadResult FFTFilePlaybackEngine::make_result(bool ok, std::string message) const {
    return FFTFilePlaybackLoadResult {
        .ok = ok,
        .message = std::move(message),
        .instrument_count = waveset_service_.instrument_count(),
        .track_count = smd_file_.has_value() ? smd_file_->track_count : 0,
        .tempo_bpm = smd_file_.has_value() ? sequencer_.tempo_bpm() : 0.0,
    };
}

bool FFTFilePlaybackEngine::rebuild_playback_internal(std::string* error_message) {
    playback_ready_ = false;
    playing_ = false;
    finished_ = false;
    pcm_queue_.clear();

    if (!waveset_service_.is_loaded()) {
        if (error_message != nullptr) {
            *error_message = "No WAVESET loaded";
        }
        return false;
    }
    if (!smd_file_.has_value()) {
        if (error_message != nullptr) {
            *error_message = "No SMD loaded";
        }
        return false;
    }
    const FFTSmdFile playback_smd =
        filtered_smd_without_disabled_opcodes(*smd_file_, disabled_source_opcodes_);
    if (!sequencer_.load_smd(playback_smd, error_message)) {
        return false;
    }

    playback_ready_ = true;
    return true;
}

void FFTFilePlaybackEngine::append_pcm(std::vector<int16_t>&& pcm) {
    pcm_queue_.insert(pcm_queue_.end(), pcm.begin(), pcm.end());
}

int32_t FFTFilePlaybackEngine::queued_frame_count() const {
    return static_cast<int32_t>(pcm_queue_.size() / 2U);
}

}  // namespace fftplugin
