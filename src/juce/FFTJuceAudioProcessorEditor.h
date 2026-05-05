#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <JuceHeader.h>

#include "FFTMatchLabSeed.h"
#include "FFTPianoDetailView.h"
#include "FFTTrackLaneView.h"

namespace fftplugin {
namespace jucewrap {

class FFTJuceAudioProcessor;

class FFTLaneViewport final : public juce::Viewport {
public:
    std::function<void(juce::Rectangle<int>)> on_visible_area_changed;

private:
    void visibleAreaChanged(const juce::Rectangle<int>& newVisibleArea) override {
        juce::Viewport::visibleAreaChanged(newVisibleArea);
        if (on_visible_area_changed) {
            on_visible_area_changed(newVisibleArea);
        }
    }
};

class FFTJuceAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::Button::Listener,
                                          private juce::Timer {
public:
    explicit FFTJuceAudioProcessorEditor(FFTJuceAudioProcessor&);
    ~FFTJuceAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    static constexpr float kDefaultPixelsPerTick = 0.7f;
    static constexpr float kMinPixelsPerTick = 0.15f;
    static constexpr float kMaxPixelsPerTick = 8.0f;

    FFTJuceAudioProcessor& processor_;
    std::unique_ptr<juce::FileChooser> active_chooser_;
    juce::String last_inspected_smd_path_;
    int last_inspector_track_count_ = -1;
    int last_selected_track_index_ = -1;
    FFTSmdPresentationMode presentation_mode_ = FFTSmdPresentationMode::source;
    FFTSmdPresentationMode last_presentation_mode_ = FFTSmdPresentationMode::source;
    float pixels_per_tick_ = kDefaultPixelsPerTick;
    int note_snap_mode_index_ = 6;
    int pan_start_scroll_x_ = 0;
    int pan_start_scroll_y_ = 0;
    int selected_command_track_index_ = -1;
    std::optional<FFTSmdLaneCommandBlock> selected_command_;
    std::vector<int32_t> selected_command_source_event_indices_;
    std::vector<int32_t> selected_command_authored_opcode_indices_;
    int selected_rest_track_index_ = -1;
    int selected_rest_source_event_index_ = -1;
    int selected_rest_authored_span_index_ = -1;
    int32_t selected_time_start_tick_ = -1;
    int32_t selected_time_end_tick_ = -1;
    std::vector<int> selected_source_track_indices_;

    juce::TextButton unwind_mode_button_ {"Unwind"};
    juce::TextButton group_button_ {"GROUP"};
    juce::TextButton ungroup_button_ {"UGRP"};
    juce::TextButton track_transposition_button_ {"Trn: 0"};
    juce::TextButton play_button_ {"PLAY"};
    juce::TextButton stop_button_ {"STOP"};
    juce::TextButton clear_selection_button_ {"CLR"};
    juce::TextButton debug_button_ {"DBG"};
    juce::Label waveset_label_;
    juce::Label waveset_path_label_;
    juce::Label waveset_status_label_;
    juce::TextButton waveset_button_ {"WSET"};
    juce::TextButton new_song_button_ {"NEW"};
    juce::TextButton midi_import_button_ {"MIDI"};
    juce::TextButton export_smd_button_ {"EXP"};
    juce::TextEditor target_bytes_editor_;
    juce::Label target_bytes_label_;
    juce::TextButton match_lab_button_ {"LAB"};
    juce::Label smd_label_;
    juce::Label smd_path_label_;
    juce::Label smd_status_label_;
    juce::TextButton smd_button_ {"SMD"};
    juce::Label status_label_;
    juce::Label playback_label_;
    juce::Label inspector_title_label_;
    juce::Label track_selector_label_;
    juce::ComboBox track_selector_;
    FFTTrackLaneView lane_header_view_ {FFTTrackLaneViewMode::header};
    FFTLaneViewport lane_viewport_;
    FFTTrackLaneView lane_view_ {FFTTrackLaneViewMode::tracks};
    FFTPianoDetailView detail_view_;
    juce::TextEditor metadata_editor_;
    juce::TextEditor track_summary_editor_;
    juce::TextEditor event_list_editor_;

    void buttonClicked(juce::Button* button) override;
    void timerCallback() override;
    void refresh_labels();
    void refresh_lane_presentation();
    void refresh_inspector();
    void configure_text_editor(juce::TextEditor& editor);
    void sync_lane_scroll_state();
    void apply_lane_zoom(float anchor_tick, float wheel_delta);
    void apply_lane_pan(int drag_delta_x, int drag_delta_y);
    void choose_file(bool choose_waveset);
    void choose_new_song();
    void choose_import_midi();
    void choose_export_smd();
    void show_match_lab(const std::optional<FFTMatchLabSeed>& seed = std::nullopt);
    void open_match_lab_for_note(int track_index, int source_event_index);
    void start_local_playback_from_selection();
    void handle_time_selection_changed(int32_t start_tick, int32_t end_tick);
    void handle_detail_command_selected(int track_index, const FFTSmdLaneCommandBlock& command);
    void handle_detail_command_selection_toggled(int track_index, const FFTSmdLaneCommandBlock& command);
    void handle_detail_command_toggle_disabled(int track_index, const FFTSmdLaneCommandBlock& command);
    void handle_detail_command_delete(int track_index, const FFTSmdLaneCommandBlock& command);
    void handle_detail_command_move(
        int track_index,
        const FFTSmdLaneCommandBlock& command,
        const FFTSmdLaneInsertAnchor& anchor);
    void handle_time_lane_note_delete(int track_index, int source_event_index);
    void handle_time_lane_note_insert(
        int track_index,
        int source_event_index,
        int note_relative_key,
        int32_t absolute_start_tick,
        int32_t covered_start_tick,
        int32_t duration_ticks);
    void handle_time_lane_note_resize(
        int track_index,
        int source_event_index,
        int32_t start_tick,
        int32_t base_duration_ticks,
        int32_t extension_ticks);
    void handle_time_lane_note_fermata(
        int track_index,
        int source_event_index,
        int32_t extension_ticks);
    void handle_time_lane_rest_selected(int track_index, int source_event_index, int authored_span_index);
    void handle_time_lane_rest_ripple_resize(
        int track_index,
        int source_event_index,
        int authored_span_index,
        int32_t delta_ticks);
    void handle_insert_anchor_selected(int track_index, const FFTSmdLaneInsertAnchor& anchor);
    std::vector<int32_t> visible_positions_for_authored_tick(
        const FFTSmdTrackLanePresentation& track,
        int32_t authored_tick) const;
    void adjust_note_snap_mode(int delta);
    void show_dynamics_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_pan_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_pitch_lfo_depth_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_pitch_lfo_setup_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_pitch_bend_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_conditional_flag_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_detune_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_unknown_ad_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_attack_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_sustain_rate_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_release_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_decay_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_sustain_level_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_decay_sustain_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_adsr_slide_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_tempo_slide_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_volume_lfo_depth_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_volume_lfo_setup_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_octave_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_bank_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_tempo_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_time_signature_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_instrument_picker(int track_index, const FFTSmdLaneCommandBlock& command);
    void show_generic_value_picker(
        int track_index,
        const FFTSmdLaneCommandBlock& command,
        const juce::String& title,
        const juce::String& label_prefix,
        int expected_opcode,
        int min_value,
        int max_value);
    void show_generic_pair_picker(
        int track_index,
        const FFTSmdLaneCommandBlock& command,
        const juce::String& title,
        const juce::String& label_prefix,
        int expected_opcode,
        const juce::String& first_label,
        const juce::String& second_label);
    void show_generic_unsigned_triple_picker(
        int track_index,
        const FFTSmdLaneCommandBlock& command,
        const juce::String& title,
        const juce::String& label_prefix,
        int expected_opcode,
        const juce::String& first_label,
        const juce::String& second_label,
        const juce::String& third_label);
    void show_generic_signed_middle_triple_picker(
        int track_index,
        const FFTSmdLaneCommandBlock& command,
        const juce::String& title,
        const juce::String& label_prefix,
        int expected_opcode);
    void show_opcode_choice_picker(
        int track_index,
        const FFTSmdLaneCommandBlock& command,
        const juce::String& title,
        const std::vector<std::pair<juce::String, int>>& choices);
    void show_opcode_insert_picker(int track_index, const FFTSmdLaneInsertAnchor& anchor);
    void sync_selected_commands_to_views();
    void sync_selected_rest_to_views();
    void sync_selected_time_to_views();
    void clear_selected_command_selection();
    void clear_selected_rest_selection();
    void refresh_group_buttons();
    bool has_time_selection() const;
    int32_t selected_time_duration_ticks() const;
    bool unwound_selection_crosses_loop_boundary() const;
    int32_t map_visible_tick_to_authored_tick(int track_index, int32_t visible_tick) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FFTJuceAudioProcessorEditor)
};

}  // namespace jucewrap
}  // namespace fftplugin
