#pragma once

#include <cstdint>
#include <string>

namespace fftplugin {

class FFTFilePlaybackEngine;

// Encapsulates transport state — global play/stop, local playback range,
// and host-position tracking. Held by FFTPluginProcessorCore. The processor
// retains the cross-cutting orchestrators (set_transport_playing,
// start_local_playback, sync_host_transport) because they thread through
// reload_from_state + track_control + bridge; this service owns the raw
// state and the leaf operations (stop_local_playback, rewind).
class FFTTransportControl {
public:
    explicit FFTTransportControl(FFTFilePlaybackEngine& engine);

    // ----- raw state access -----
    bool transport_playing() const { return transport_playing_; }
    void set_transport_playing(bool playing) { transport_playing_ = playing; }

    bool local_transport_active() const { return local_transport_active_; }
    void set_local_transport_active(bool active) { local_transport_active_ = active; }

    int32_t local_playback_start_tick() const { return local_playback_start_tick_; }
    int32_t local_playback_end_tick() const { return local_playback_end_tick_; }
    void set_local_range(int32_t start_tick, int32_t end_tick) {
        local_playback_start_tick_ = start_tick;
        local_playback_end_tick_ = end_tick;
    }

    bool last_host_playing() const { return last_host_playing_; }
    void set_last_host_playing(bool playing) { last_host_playing_ = playing; }

    int64_t last_host_sample_position() const { return last_host_sample_position_; }
    void set_last_host_sample_position(int64_t pos) { last_host_sample_position_ = pos; }

    // ----- leaf operations -----

    // Engine rewind. Returns false on failure; populates last_error from
    // the engine in that case.
    bool rewind(std::string& last_error);

    // Drop local-playback flags and stop the engine. Mirrors the original
    // FFTPluginProcessorCore::stop_local_playback.
    void stop_local_playback(bool rewind_to_start);

    // Returns true if the new host position is more than tolerance frames
    // before the last seen host position (host scrubbed back / restarted).
    bool host_jumped_backwards(int64_t host_sample_position, int64_t tolerance) const;

    // Called from process()/process_interleaved() after each render block.
    // If a local playback range ran past its end_tick, stop. If the engine
    // is fully finished, drop the transport flags. Returns true if we
    // stopped (caller can use this as a signal to flush/etc., though
    // FFTPluginProcessorCore doesn't currently care).
    bool post_render_check();

private:
    FFTFilePlaybackEngine* engine_;

    bool transport_playing_ = false;
    bool local_transport_active_ = false;
    int32_t local_playback_start_tick_ = 0;
    int32_t local_playback_end_tick_ = -1;
    bool last_host_playing_ = false;
    int64_t last_host_sample_position_ = -1;
};

}  // namespace fftplugin
