#pragma once

#include <optional>
#include <vector>

#include <JuceHeader.h>

#include "FFTMidiImport.h"
#include "FFTMatchLabSeed.h"
#include "fft_plugin/fft_audio_backend.h"
#include "fft_plugin/fft_plugin_processor_core.h"
#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {
namespace jucewrap {

class FFTJuceAudioProcessor final : public juce::AudioProcessor {
public:
    FFTJuceAudioProcessor();
    ~FFTJuceAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool set_waveset_path(const juce::String& path);
    bool set_music_document_path(const juce::String& path);
    bool create_new_music_document_path(const juce::String& path);
    bool import_midi_path(const juce::String& midi_path, const juce::String& authoring_path);
    bool set_smd_path(const juce::String& path);
    bool export_smd_path(const juce::String& path);
    bool export_smd_path(const juce::String& path, size_t target_bytes);
    FFTSmdGameCompileReport export_smd_path_with_report(
        const juce::String& path, size_t target_bytes);
    bool convert_tracks_to_poly_track(const std::vector<int>& track_indices);
    bool ungroup_poly_track(int track_index);
    bool authored_track_is_poly_track(int track_index) const;
    juce::String waveset_path() const;
    juce::String music_document_path() const;
    juce::String smd_path() const;
    juce::String waveset_status_text() const;
    juce::String smd_status_text() const;
    juce::String status_text() const;
    juce::String diagnostic_text() const;
    void set_status_text_for_debug(const juce::String& text);
    juce::String playback_summary() const;
    int inspector_track_count() const;
    juce::String inspector_metadata_text() const;
    juce::String inspector_track_summary_text() const;
    juce::String inspector_track_events_text(int track_index) const;
    FFTSmdSongPresentation inspector_song_presentation(FFTSmdPresentationMode mode) const;
    bool start_local_playback(int32_t start_tick = 0, int32_t end_tick = -1);
    void stop_local_playback();
    bool local_transport_active() const;
    void set_track_muted(int track_index, bool muted);
    void set_track_soloed(int track_index, bool soloed);
    bool track_muted(int track_index) const;
    bool track_soloed(int track_index) const;
    int selected_track_id() const;
    void set_selected_track_id(int track_index);
    bool set_track_opcode_code(int track_index, int source_event_index, int expected_opcode, int new_opcode);
    bool delete_track_opcode(int track_index, int source_event_index, int expected_opcode);
    bool delete_authored_opcode(int track_index, int authored_opcode_index, int expected_opcode);
    bool move_track_opcode(
        int track_index,
        int source_event_index,
        int expected_opcode,
        int target_tick,
        int insertion_sequence_index,
        int* moved_source_event_index = nullptr);
    bool move_authored_opcode(
        int track_index,
        int authored_opcode_index,
        int expected_opcode,
        int target_tick,
        int insertion_sequence_index,
        int* moved_authored_opcode_index = nullptr);
    bool move_track_opcodes(
        int track_index,
        const std::vector<int32_t>& source_event_indices,
        int dragged_source_event_index,
        int target_tick,
        int insertion_sequence_index,
        std::vector<int32_t>* moved_source_event_indices = nullptr,
        int* moved_dragged_source_event_index = nullptr);
    bool move_authored_opcodes(
        int track_index,
        const std::vector<int32_t>& authored_opcode_indices,
        int dragged_authored_opcode_index,
        int target_tick,
        int insertion_sequence_index,
        std::vector<int32_t>* moved_authored_opcode_indices = nullptr,
        int* moved_dragged_authored_opcode_index = nullptr);
    bool insert_track_time(int track_index, int start_tick, int duration_ticks);
    bool delete_track_time(int track_index, int start_tick, int duration_ticks);
    bool replace_track_note_with_rest(int track_index, int source_event_index);
    bool replace_authored_note_with_rest(int track_index, int authored_span_index);
    bool replace_track_rest_with_note(
        int track_index,
        int source_event_index,
        int note_relative_key,
        int start_offset_ticks,
        int duration_ticks);
    bool replace_authored_rest_with_note(
        int track_index,
        int authored_span_index,
        int note_relative_key,
        int start_offset_ticks,
        int duration_ticks);
    bool insert_authored_poly_note(
        int track_index,
        int note_relative_key,
        int start_tick,
        int duration_ticks);
    bool insert_authored_poly_rest(
        int track_index,
        int start_tick,
        int duration_ticks);
    bool set_track_note_fermata_extension(
        int track_index,
        int source_event_index,
        int extension_ticks);
    bool set_authored_note_fermata_extension(
        int track_index,
        int authored_span_index,
        int extension_ticks);
    bool set_track_note_geometry(
        int track_index,
        int source_event_index,
        int start_tick,
        int base_duration_ticks,
        int extension_ticks);
    bool set_authored_note_geometry(
        int track_index,
        int authored_span_index,
        int start_tick,
        int base_duration_ticks,
        int extension_ticks);
    bool resize_track_rest_duration(
        int track_index,
        int source_event_index,
        int delta_ticks);
    bool resize_authored_rest_duration(
        int track_index,
        int authored_span_index,
        int delta_ticks);
    bool insert_track_opcode(
        int track_index,
        int target_tick,
        int insertion_sequence_index,
        int opcode,
        const std::vector<int32_t>& params,
        int* inserted_source_event_index = nullptr);
    bool set_track_source_opcode_disabled(int track_index, int source_event_index, bool disabled);
    bool track_source_opcode_disabled(int track_index, int source_event_index) const;
    bool set_track_generic_opcode_param_value(
        int track_index,
        int source_event_index,
        int expected_opcode,
        int value,
        int min_value,
        int max_value);
    bool set_track_generic_opcode_param_values(
        int track_index,
        int source_event_index,
        int expected_opcode,
        const std::vector<int32_t>& values);
    bool set_track_dynamics_opcode_value(int track_index, int source_event_index, int dynamics_value);
    bool set_track_pan_opcode_value(int track_index, int source_event_index, int pan_value);
    bool set_track_adsr_attack_opcode_value(int track_index, int source_event_index, int attack_value);
    bool set_track_adsr_sustain_rate_opcode_value(int track_index, int source_event_index, int sustain_rate_value);
    bool set_track_adsr_release_opcode_value(int track_index, int source_event_index, int release_value);
    bool set_track_adsr_decay_opcode_value(int track_index, int source_event_index, int decay_value);
    bool set_track_adsr_sustain_level_opcode_value(int track_index, int source_event_index, int sustain_level_value);
    bool set_track_adsr_decay_sustain_opcode_values(int track_index, int source_event_index, int decay_value, int sustain_level_value);
    bool set_track_pitch_lfo_depth_opcode_value(int track_index, int source_event_index, int depth_value);
    bool set_track_pitch_lfo_opcode_values(int track_index, int source_event_index, int length_value, int signed_shape_value, int depth_value);
    bool set_track_pitch_bend_opcode_value(int track_index, int source_event_index, int bend_value);
    bool set_track_conditional_seq_flag_opcode_value(int track_index, int source_event_index, int flag_value);
    bool set_track_detune_opcode_value(int track_index, int source_event_index, int detune_value);
    bool set_track_unknown_ad_opcode_value(int track_index, int source_event_index, int value);
    bool set_track_adsr_slide_opcode_value(int track_index, int source_event_index, int value);
    bool set_track_volume_lfo_depth_opcode_value(int track_index, int source_event_index, int depth_value);
    bool set_track_tempo_slide_opcode_values(int track_index, int source_event_index, int first_value, int second_value);
    bool set_track_volume_lfo_opcode_values(int track_index, int source_event_index, int length_value, int signed_shape_value, int depth_value);
    bool set_track_octave_opcode_value(int track_index, int source_event_index, int octave_value);
    bool set_track_bank_select_opcode_value(int track_index, int source_event_index, int bank_value);
    bool set_track_tempo_opcode_value(int track_index, int source_event_index, int tempo_value);
    bool set_track_time_signature_opcode_values(int track_index, int source_event_index, int numerator, int denominator);
    bool set_track_instrument_opcode_value(int track_index, int source_event_index, int instrument_id);
    int track_transposition(int track_index) const;
    bool set_track_transposition(int track_index, int semitones);
    bool start_preview_note(const FFTPreviewNoteRequest& request);
    void stop_preview_note(int16_t midi_note);
    bool play_match_lab_fft_note(const FFTPreviewNoteRequest& request, int duration_ms, int release_ms = 220);
    bool load_match_lab_reference_wav(const juce::String& path);
    void play_match_lab_reference();
    void stop_match_lab_reference();
    std::optional<FFTInstrumentInfo> loaded_instrument_info(int32_t played_sample_id) const;
    std::optional<FFTMatchLabSeed> match_lab_seed_for_note(int track_index, int source_event_index) const;
    int current_playback_tick() const;
    std::vector<int32_t> current_source_track_ticks() const;
    bool reload_from_state();

private:
    struct MatchLabSamplePlayback {
        juce::SpinLock lock;
        std::vector<float> interleaved;
        int64_t frame_position = 0;
        bool active = false;
    };

    FFTPluginProcessorCore core_;
    juce::String status_text_;
    juce::String waveset_status_text_;
    juce::String smd_status_text_;
    MatchLabSamplePlayback match_lab_reference_;
    MatchLabSamplePlayback match_lab_fft_preview_;

    // Resampling support for hosts running at rates other than 44100 Hz.
    double host_sample_rate_ = 44100.0;
    juce::LagrangeInterpolator resamp_left_;
    juce::LagrangeInterpolator resamp_right_;
    std::vector<float> resamp_buf_left_;
    std::vector<float> resamp_buf_right_;
    std::vector<FFTImportedMidiPartProvenance> imported_midi_part_provenance_;

    void apply_reload_report(const FFTStateReloadReport& report, const juce::String& success_text);
    void ensure_default_paths();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FFTJuceAudioProcessor)
};

}  // namespace jucewrap
}  // namespace fftplugin
