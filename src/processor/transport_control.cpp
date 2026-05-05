#include "fft_plugin/processor/transport_control.h"

#include "fft_plugin/fft_file_playback_engine.h"

namespace fftplugin {

FFTTransportControl::FFTTransportControl(FFTFilePlaybackEngine& engine) : engine_(&engine) {}

bool FFTTransportControl::rewind(std::string& last_error) {
    const bool ok = engine_->rewind();
    if (!ok) {
        last_error = engine_->last_error();
    }
    return ok;
}

void FFTTransportControl::stop_local_playback(bool rewind_to_start) {
    local_transport_active_ = false;
    transport_playing_ = false;
    engine_->stop(rewind_to_start);
}

bool FFTTransportControl::post_render_check() {
    bool stopped = false;
    if (local_transport_active_ &&
        local_playback_end_tick_ > local_playback_start_tick_ &&
        engine_->current_playback_tick() >= local_playback_end_tick_) {
        stop_local_playback(false);
        stopped = true;
    }
    if (engine_->finished()) {
        local_transport_active_ = false;
        transport_playing_ = false;
    }
    return stopped;
}

bool FFTTransportControl::host_jumped_backwards(int64_t host_sample_position, int64_t tolerance) const {
    if (host_sample_position < 0 || last_host_sample_position_ < 0) {
        return false;
    }
    return host_sample_position + tolerance < last_host_sample_position_;
}

}  // namespace fftplugin
