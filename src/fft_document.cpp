#include "fft_plugin/fft_document.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace fftplugin {

namespace {

FFTTrack make_track(int32_t track_id, std::string name) {
    FFTTrack track;
    track.id = track_id;
    track.name = std::move(name);
    track.default_instrument = 0;
    track.default_pan = 64;
    track.default_volume = 100;
    return track;
}

}  // namespace

FFTDocument::FFTDocument()
    : plugin_state_(make_default_state())
    , next_track_id_(static_cast<int32_t>(plugin_state_.sequence.tracks.size()) + 1) {}

FFTPluginState FFTDocument::make_default_state() {
    FFTPluginState state;
    state.state_version = 3;
    state.editor_view.horizontal_zoom = 1.0;
    state.editor_view.vertical_zoom = 1.0;
    state.editor_view.selected_track_id = 1;
    state.editor_view.visible_note_low = 24;
    state.editor_view.visible_note_high = 96;
    state.editor_view.lanes = {
        {OpcodeLaneType::instrument, true},
        {OpcodeLaneType::structure, true},
        {OpcodeLaneType::articulation, false},
        {OpcodeLaneType::modulation, false},
        {OpcodeLaneType::conductor, true},
    };
    state.sequence.format_version = 1;
    state.sequence.ppq = 48;
    state.sequence.name = "New FFT Sequence";
    state.sequence.tracks.push_back(make_track(1, "Track 1"));
    return state;
}

const FFTPluginState& FFTDocument::state() const {
    return plugin_state_;
}

FFTPluginState& FFTDocument::state() {
    return plugin_state_;
}

FFTTrack* FFTDocument::find_track(int32_t track_id) {
    auto it = std::find_if(
        plugin_state_.sequence.tracks.begin(),
        plugin_state_.sequence.tracks.end(),
        [track_id](const FFTTrack& track) { return track.id == track_id; }
    );
    return it == plugin_state_.sequence.tracks.end() ? nullptr : &(*it);
}

const FFTTrack* FFTDocument::find_track(int32_t track_id) const {
    auto it = std::find_if(
        plugin_state_.sequence.tracks.begin(),
        plugin_state_.sequence.tracks.end(),
        [track_id](const FFTTrack& track) { return track.id == track_id; }
    );
    return it == plugin_state_.sequence.tracks.end() ? nullptr : &(*it);
}

FFTTrack& FFTDocument::append_track(std::string name) {
    if (name.empty()) {
        name = "Track " + std::to_string(next_track_id_);
    }

    plugin_state_.sequence.tracks.push_back(make_track(next_track_id_, std::move(name)));
    FFTTrack& track = plugin_state_.sequence.tracks.back();
    if (plugin_state_.editor_view.selected_track_id <= 0) {
        plugin_state_.editor_view.selected_track_id = track.id;
    }
    ++next_track_id_;
    return track;
}

FFTInstrumentChangeEvent& FFTDocument::append_instrument_change(
    int32_t track_id,
    int32_t start_tick,
    int32_t instrument_id
) {
    FFTTrack* track = find_track(track_id);
    if (track == nullptr) {
        throw std::runtime_error("track_id does not exist");
    }

    track->events.push_back(FFTInstrumentChangeEvent {
        .start_tick = start_tick,
        .instrument_id = instrument_id,
    });
    track->default_instrument = instrument_id;
    return std::get<FFTInstrumentChangeEvent>(track->events.back());
}

FFTNoteEvent& FFTDocument::append_note(
    int32_t track_id,
    int32_t start_tick,
    int32_t duration_ticks,
    int16_t midi_note,
    int16_t velocity
) {
    FFTTrack* track = find_track(track_id);
    if (track == nullptr) {
        throw std::runtime_error("track_id does not exist");
    }

    track->events.push_back(FFTNoteEvent {
        .start_tick = start_tick,
        .duration_ticks = duration_ticks,
        .midi_note = midi_note,
        .velocity = velocity,
        .gate_ticks = duration_ticks,
        .slur_in = false,
        .slur_out = false,
    });
    return std::get<FFTNoteEvent>(track->events.back());
}

}  // namespace fftplugin
