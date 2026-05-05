#pragma once

#include <cstdint>

namespace fftplugin {

class FFTFilePlaybackEngine;
struct FFTPluginState;

// Owns the editor-view track-control state (mute / solo / selection) and
// relays it to the playback engine. Held by FFTPluginProcessorCore; kept
// behind that façade so legacy callers see the same surface.
class FFTTrackControlService {
public:
    FFTTrackControlService(FFTPluginState& state, FFTFilePlaybackEngine& engine);

    void set_track_muted(int32_t track_idx, bool muted);
    void set_track_soloed(int32_t track_idx, bool soloed);
    bool track_muted(int32_t track_idx) const;
    bool track_soloed(int32_t track_idx) const;
    int32_t selected_track_id() const;
    void set_selected_track_id(int32_t track_idx);

    // Replays the persisted mute/solo state through to the engine. Call
    // after the authored document changes shape (track count, part order,
    // poly group/ungroup) so the engine sees the right voices muted.
    void apply_to_engine();

private:
    FFTPluginState* state_;
    FFTFilePlaybackEngine* engine_;
};

}  // namespace fftplugin
