#pragma once

#include <string>

#include "fft_plugin/fft_plugin_state.h"

namespace fftplugin {

class FFTDocument {
public:
    FFTDocument();

    static FFTPluginState make_default_state();

    const FFTPluginState& state() const;
    FFTPluginState& state();

    FFTTrack* find_track(int32_t track_id);
    const FFTTrack* find_track(int32_t track_id) const;

    FFTTrack& append_track(std::string name);
    FFTInstrumentChangeEvent& append_instrument_change(
        int32_t track_id,
        int32_t start_tick,
        int32_t instrument_id
    );
    FFTNoteEvent& append_note(
        int32_t track_id,
        int32_t start_tick,
        int32_t duration_ticks,
        int16_t midi_note,
        int16_t velocity
    );

private:
    FFTPluginState plugin_state_;
    int32_t next_track_id_ = 1;
};

}  // namespace fftplugin
