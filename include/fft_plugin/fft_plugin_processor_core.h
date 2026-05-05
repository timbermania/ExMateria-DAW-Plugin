#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fft_plugin/fft_document.h"
#include "fft_plugin/fft_file_playback_engine.h"
#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/processor/authoring_bridge.h"
#include "fft_plugin/processor/track_control.h"
#include "fft_plugin/processor/track_edit_service.h"
#include "fft_plugin/processor/track_grouping_service.h"
#include "fft_plugin/processor/track_note_service.h"
#include "fft_plugin/processor/transport_control.h"

namespace fftplugin {

struct FFTProcessSetup {
    double sample_rate = 44100.0;
    int32_t max_block_size = 512;
    int32_t output_channels = 2;
};

struct FFTStateReloadReport {
    bool waveset_requested = false;
    bool waveset_ok = false;
    std::string waveset_message;

    bool smd_requested = false;
    bool smd_ok = false;
    std::string smd_message;

    bool ready = false;
    std::string summary;
};

class FFTPluginProcessorCore {
public:
    FFTPluginProcessorCore();

    bool prepare_to_play(const FFTProcessSetup& setup, std::string* error_message = nullptr);
    const FFTProcessSetup& process_setup() const;
    bool prepared() const;
    bool supports_process_setup(const FFTProcessSetup& setup) const;

    FFTFilePlaybackLoadResult load_waveset_path(const std::string& waveset_path);
    FFTFilePlaybackLoadResult load_smd_path(const std::string& smd_path);
    FFTFilePlaybackLoadResult load_music_document_path(const std::string& path);
    FFTFilePlaybackLoadResult create_new_music_document_path(const std::string& authoring_path);
    FFTFilePlaybackLoadResult create_music_document_from_authoring_document(
        const std::string& authoring_path,
        FFTSmdAuthoringDocument document,
        const std::string& success_message = "Created music document");
    bool export_smd_path(const std::string& path, std::string* error_message = nullptr);
    FFTSmdGameCompileReport export_smd_path_for_game(const std::string& path);
    FFTSmdGameCompileReport export_smd_path_for_game(
        const std::string& path,
        const FFTSmdGameCompileBudget& budget);
    bool reload_from_state(std::string* error_message = nullptr);
    FFTStateReloadReport reload_from_state_report();
    bool convert_tracks_to_poly_track(
        const std::vector<int32_t>& track_indices,
        std::string* error_message = nullptr);
    bool ungroup_poly_track(int32_t track_idx, std::string* error_message = nullptr);
    bool authored_track_is_poly_track(int32_t track_idx) const;

    void set_transport_playing(bool playing);
    void sync_host_transport(bool playing, int64_t host_sample_position = -1);
    bool start_local_playback(int32_t start_tick = 0, int32_t end_tick = -1);
    void stop_local_playback(bool rewind_to_start = false);
    bool local_transport_active() const;
    bool transport_playing() const;
    bool rewind();
    void set_track_muted(int32_t track_idx, bool muted);
    void set_track_soloed(int32_t track_idx, bool soloed);
    bool track_muted(int32_t track_idx) const;
    bool track_soloed(int32_t track_idx) const;
    int32_t selected_track_id() const;
    void set_selected_track_id(int32_t track_idx);
    bool set_track_opcode_code(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        int32_t new_opcode,
        std::string* error_message = nullptr);
    bool delete_track_opcode(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        std::string* error_message = nullptr);
    bool delete_authored_opcode(
        int32_t track_idx,
        int32_t authored_opcode_index,
        int32_t expected_opcode,
        std::string* error_message = nullptr);
    bool move_track_opcode(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        int32_t* moved_source_event_index = nullptr,
        std::string* error_message = nullptr);
    bool move_authored_opcode(
        int32_t track_idx,
        int32_t authored_opcode_index,
        int32_t expected_opcode,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        int32_t* moved_authored_opcode_index = nullptr,
        std::string* error_message = nullptr);
    bool move_track_opcodes(
        int32_t track_idx,
        const std::vector<int32_t>& source_event_indices,
        int32_t dragged_source_event_index,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        std::vector<int32_t>* moved_source_event_indices = nullptr,
        int32_t* moved_dragged_source_event_index = nullptr,
        std::string* error_message = nullptr);
    bool move_authored_opcodes(
        int32_t track_idx,
        const std::vector<int32_t>& authored_opcode_indices,
        int32_t dragged_authored_opcode_index,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        std::vector<int32_t>* moved_authored_opcode_indices = nullptr,
        int32_t* moved_dragged_authored_opcode_index = nullptr,
        std::string* error_message = nullptr);
    bool insert_track_time(
        int32_t track_idx,
        int32_t start_tick,
        int32_t duration_ticks,
        std::string* error_message = nullptr);
    bool delete_track_time(
        int32_t track_idx,
        int32_t start_tick,
        int32_t duration_ticks,
        std::string* error_message = nullptr);
    bool replace_track_note_with_rest(
        int32_t track_idx,
        int32_t source_event_index,
        std::string* error_message = nullptr);
    bool replace_authored_note_with_rest(
        int32_t track_idx,
        int32_t authored_span_index,
        std::string* error_message = nullptr);
    bool replace_track_rest_with_note(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t note_relative_key,
        int32_t start_offset_ticks,
        int32_t duration_ticks,
        std::string* error_message = nullptr);
    bool replace_authored_rest_with_note(
        int32_t track_idx,
        int32_t authored_span_index,
        int32_t note_relative_key,
        int32_t start_offset_ticks,
        int32_t duration_ticks,
        std::string* error_message = nullptr);
    bool insert_authored_poly_note(
        int32_t track_idx,
        int32_t note_relative_key,
        int32_t start_tick,
        int32_t duration_ticks,
        std::string* error_message = nullptr);
    bool insert_authored_poly_rest(
        int32_t track_idx,
        int32_t start_tick,
        int32_t duration_ticks,
        std::string* error_message = nullptr);
    bool set_track_note_fermata_extension(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t extension_ticks,
        std::string* error_message = nullptr);
    bool set_authored_note_fermata_extension(
        int32_t track_idx,
        int32_t authored_span_index,
        int32_t extension_ticks,
        std::string* error_message = nullptr);
    bool set_track_note_geometry(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t start_tick,
        int32_t base_duration_ticks,
        int32_t extension_ticks,
        std::string* error_message = nullptr);
    bool set_authored_note_geometry(
        int32_t track_idx,
        int32_t authored_span_index,
        int32_t start_tick,
        int32_t base_duration_ticks,
        int32_t extension_ticks,
        std::string* error_message = nullptr);
    bool resize_track_rest_duration(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t delta_ticks,
        std::string* error_message = nullptr);
    bool resize_authored_rest_duration(
        int32_t track_idx,
        int32_t authored_span_index,
        int32_t delta_ticks,
        std::string* error_message = nullptr);
    bool insert_track_opcode(
        int32_t track_idx,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        int32_t opcode,
        const std::vector<int32_t>& params,
        int32_t* inserted_source_event_index = nullptr,
        std::string* error_message = nullptr);
    bool set_track_source_opcode_disabled(
        int32_t track_idx,
        int32_t source_event_index,
        bool disabled,
        std::string* error_message = nullptr);
    bool track_source_opcode_disabled(int32_t track_idx, int32_t source_event_index) const;
    bool set_track_generic_opcode_param_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        int32_t value,
        int32_t min_value,
        int32_t max_value,
        std::string* error_message = nullptr);
    bool set_track_generic_opcode_param_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        const std::vector<int32_t>& values,
        std::string* error_message = nullptr);
    bool set_track_dynamics_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t dynamics_value,
        std::string* error_message = nullptr);
    bool set_track_pan_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t pan_value,
        std::string* error_message = nullptr);
    bool set_track_adsr_attack_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t attack_value,
        std::string* error_message = nullptr);
    bool set_track_adsr_sustain_rate_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t sustain_rate_value,
        std::string* error_message = nullptr);
    bool set_track_adsr_release_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t release_value,
        std::string* error_message = nullptr);
    bool set_track_adsr_decay_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t decay_value,
        std::string* error_message = nullptr);
    bool set_track_adsr_sustain_level_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t sustain_level_value,
        std::string* error_message = nullptr);
    bool set_track_adsr_decay_sustain_opcode_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t decay_value,
        int32_t sustain_level_value,
        std::string* error_message = nullptr);
    bool set_track_pitch_lfo_depth_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t depth_value,
        std::string* error_message = nullptr);
    bool set_track_pitch_lfo_opcode_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t length_value,
        int32_t signed_shape_value,
        int32_t depth_value,
        std::string* error_message = nullptr);
    bool set_track_pitch_bend_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t bend_value,
        std::string* error_message = nullptr);
    bool set_track_conditional_seq_flag_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t flag_value,
        std::string* error_message = nullptr);
    bool set_track_detune_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t detune_value,
        std::string* error_message = nullptr);
    bool set_track_unknown_ad_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t value,
        std::string* error_message = nullptr);
    bool set_track_adsr_slide_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t value,
        std::string* error_message = nullptr);
    bool set_track_volume_lfo_depth_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t depth_value,
        std::string* error_message = nullptr);
    bool set_track_tempo_slide_opcode_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t first_value,
        int32_t second_value,
        std::string* error_message = nullptr);
    bool set_track_volume_lfo_opcode_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t length_value,
        int32_t signed_shape_value,
        int32_t depth_value,
        std::string* error_message = nullptr);
    bool set_track_octave_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t octave_value,
        std::string* error_message = nullptr);
    bool set_track_bank_select_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t bank_value,
        std::string* error_message = nullptr);
    bool set_track_tempo_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t tempo_value,
        std::string* error_message = nullptr);
    bool set_track_time_signature_opcode_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t numerator,
        int32_t denominator,
        std::string* error_message = nullptr);
    bool set_track_instrument_opcode_value(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t instrument_id,
        std::string* error_message = nullptr);

    int32_t track_transposition(int32_t track_idx) const;
    bool set_track_transposition(
        int32_t track_idx,
        int32_t semitones,
        std::string* error_message = nullptr);

    void process(float* output_left, float* output_right, int32_t frame_count);
    void process_interleaved(float* output, int32_t frame_count, int32_t output_channels = 2);

    std::vector<uint8_t> serialize_state() const;
    bool restore_state(const std::vector<uint8_t>& bytes, std::string* error_message = nullptr);

    const FFTPluginState& state() const;
    const FFTFilePlaybackEngine& playback_engine() const;

    // Narrow path-state mutators. The plugin state owns the canonical
    // (waveset_path, smd_path, authoring_path) triple; UI/host glue
    // updates it via these setters rather than reaching into state()
    // directly. They do NOT trigger a reload — pair them with
    // reload_from_state[_report]() when the engine should pick up the
    // change.
    void set_waveset_path_in_state(std::string path);
    void set_smd_path_in_state(std::string path);
    void clear_authoring_path_in_state();
    int32_t current_playback_tick() const;
    std::vector<int32_t> current_source_track_ticks() const;
    const std::string& last_error() const;
    const FFTStateReloadReport& last_reload_report() const;

private:
    void zero_outputs(float* output_left, float* output_right, int32_t frame_count) const;
    void sync_state_from_engine();
    void sync_derived_state_from_engine();
    std::optional<FFTSmdAuthoringDocument> build_authoring_document_from_engine() const;
    bool ensure_authored_document_loaded(std::string* error_message = nullptr);
    bool load_engine_from_authored_state(std::string* error_message = nullptr);
    bool save_authored_document_to_disk(std::string* error_message = nullptr) const;
    bool finalize_successful_authored_edit(
        const FFTSmdCompiledDocument& compiled,
        std::string* error_message = nullptr);
    void finalize_successful_edit();
    FFTPluginState state_;
    FFTFilePlaybackEngine playback_engine_;
    FFTAuthoringBridge authoring_bridge_;
    FFTTrackControlService track_control_;
    FFTTransportControl transport_;
    FFTTrackEditService track_edit_;
    FFTTrackGroupingService track_grouping_;
    FFTTrackNoteService track_note_;
    FFTProcessSetup process_setup_;
    bool prepared_ = false;
    std::string last_error_;
    FFTStateReloadReport last_reload_report_;
};

}  // namespace fftplugin
