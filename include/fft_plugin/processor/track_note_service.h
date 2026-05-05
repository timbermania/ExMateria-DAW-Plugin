#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

class FFTAuthoringBridge;
struct FFTPluginState;

// Owns the span / poly-note edit operations: insert/delete time on a
// track, replace note-with-rest / rest-with-note, set fermata extension,
// set note geometry, resize rest duration, insert poly note/rest.
//
// Held by FFTPluginProcessorCore alongside the other edit services. The
// processor's public methods are thin forwarders. Cross-cutting finalize
// is passed in as a callback so this service stays focused on document
// mutation.
class FFTTrackNoteService {
public:
    using FinalizeCallback = std::function<bool(const FFTSmdCompiledDocument&, std::string*)>;

    FFTTrackNoteService(FFTPluginState& state, FFTAuthoringBridge& bridge, FinalizeCallback finalize);

    bool insert_track_time(int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message);
    bool delete_track_time(int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message);

    bool replace_note_with_rest_by_source_event(
        int32_t track_idx, int32_t source_event_index, std::string* error_message);
    bool replace_note_with_rest_by_authored_index(
        int32_t track_idx, int32_t authored_span_index, std::string* error_message);

    bool replace_rest_with_note_by_source_event(
        int32_t track_idx, int32_t source_event_index,
        int32_t note_relative_key, int32_t start_offset_ticks, int32_t duration_ticks,
        std::string* error_message);
    bool replace_rest_with_note_by_authored_index(
        int32_t track_idx, int32_t authored_span_index,
        int32_t note_relative_key, int32_t start_offset_ticks, int32_t duration_ticks,
        std::string* error_message);

    bool insert_authored_poly_note(
        int32_t track_idx, int32_t note_relative_key,
        int32_t start_tick, int32_t duration_ticks, std::string* error_message);
    bool insert_authored_poly_rest(
        int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message);

    bool set_fermata_extension_by_source_event(
        int32_t track_idx, int32_t source_event_index, int32_t extension_ticks,
        std::string* error_message);
    bool set_fermata_extension_by_authored_index(
        int32_t track_idx, int32_t authored_span_index, int32_t extension_ticks,
        std::string* error_message);

    bool set_note_geometry_by_source_event(
        int32_t track_idx, int32_t source_event_index,
        int32_t start_tick, int32_t base_duration_ticks, int32_t extension_ticks,
        std::string* error_message);
    bool set_note_geometry_by_authored_index(
        int32_t track_idx, int32_t authored_span_index,
        int32_t start_tick, int32_t base_duration_ticks, int32_t extension_ticks,
        std::string* error_message);

    bool resize_rest_duration_by_source_event(
        int32_t track_idx, int32_t source_event_index, int32_t delta_ticks,
        std::string* error_message);
    bool resize_rest_duration_by_authored_index(
        int32_t track_idx, int32_t authored_span_index, int32_t delta_ticks,
        std::string* error_message);

private:
    FFTPluginState* state_;
    FFTAuthoringBridge* bridge_;
    FinalizeCallback finalize_;
};

}  // namespace fftplugin
