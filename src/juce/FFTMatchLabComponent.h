#pragma once

#include <memory>
#include <vector>

#include <JuceHeader.h>

#include "FFTMatchLabSeed.h"
#include "fft_plugin/fft_audio_backend.h"
#include "fft_plugin/fft_instrument_catalog.h"

namespace fftplugin {
namespace jucewrap {

class FFTJuceAudioProcessor;

class FFTMatchLabComponent final : public juce::Component,
                                   private juce::Button::Listener,
                                   private juce::TextEditor::Listener,
                                   private juce::ListBoxModel,
                                   private juce::Timer {
public:
    explicit FFTMatchLabComponent(
        FFTJuceAudioProcessor& processor,
        const std::optional<FFTMatchLabSeed>& seed = std::nullopt);
    ~FFTMatchLabComponent() override;

    void resized() override;

private:
    FFTJuceAudioProcessor& processor_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::vector<FFTInstrumentCatalogEntry> instrument_results_;
    int32_t selected_instrument_id_ = 0;
    juce::String fluidsynth_path_;
    juce::String soundfont_path_;
    juce::String reference_wav_path_;
    std::optional<FFTMatchLabSeed> seed_;
    juce::String seed_summary_;
    bool waiting_to_play_fft_ = false;
    FFTPreviewNoteRequest pending_fft_request_;
    int pending_fft_duration_ms_ = 600;
    int pending_fft_release_ms_ = 220;

    juce::Label title_label_;
    juce::Label fluidsynth_path_label_;
    juce::Label soundfont_path_label_;
    juce::Label reference_wav_label_;
    juce::TextButton choose_fluidsynth_button_ {"EXE"};
    juce::TextButton choose_soundfont_button_ {"SF2"};
    juce::TextButton choose_reference_wav_button_ {"REF WAV"};
    juce::TextButton render_reference_button_ {"GM RENDER"};
    juce::TextButton play_reference_button_ {"PLAY GM"};
    juce::TextButton play_fft_button_ {"PLAY FFT"};
    juce::TextButton play_ab_button_ {"A/B"};
    juce::TextButton stop_button_ {"STOP"};

    juce::Slider gm_program_slider_;
    juce::Slider midi_note_slider_;
    juce::Slider velocity_slider_;
    juce::Slider duration_slider_;
    juce::Slider fft_midi_note_slider_;
    juce::Slider octave_shift_slider_;
    juce::Slider dynamics_slider_;
    juce::Slider pan_slider_;
    juce::Slider adsr_attack_slider_;
    juce::Slider adsr_decay_slider_;
    juce::Slider adsr_sustain_rate_slider_;
    juce::Slider adsr_sustain_level_slider_;
    juce::Slider adsr_release_slider_;

    juce::Label gm_program_label_;
    juce::Label midi_note_label_;
    juce::Label velocity_label_;
    juce::Label duration_label_;
    juce::Label fft_midi_note_label_;
    juce::Label octave_shift_label_;
    juce::Label dynamics_label_;
    juce::Label pan_label_;
    juce::Label adsr_attack_label_;
    juce::Label adsr_decay_label_;
    juce::Label adsr_sustain_rate_label_;
    juce::Label adsr_sustain_level_label_;
    juce::Label adsr_release_label_;

    juce::TextEditor instrument_search_editor_;
    juce::ListBox instrument_results_list_;
    juce::TextEditor details_editor_;

    void buttonClicked(juce::Button* button) override;
    void textEditorTextChanged(juce::TextEditor& editor) override;
    int getNumRows() override;
    void paintListBoxItem(int row_number, juce::Graphics& g, int width, int height, bool row_is_selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void timerCallback() override;

    void configure_slider(juce::Slider& slider, double min, double max, double value, double step = 1.0);
    void configure_path_label(juce::Label& label, const juce::String& prefix);
    void configure_named_slider(juce::Label& label, juce::Slider& slider, const juce::String& name);
    void populate_default_paths();
    void launch_file_chooser(
        const juce::String& title,
        const juce::String& pattern,
        std::function<void(const juce::File&)> on_pick);
    void render_gm_reference();
    void refresh_instrument_results();
    void refresh_details();
    void apply_instrument_id(int32_t id);
    void sync_adsr_from_instrument(int32_t id);
    FFTPreviewNoteRequest build_fft_request() const;
    int preview_release_ms() const;
    bool play_fft_candidate();
    bool play_reference();
    juce::String note_name_for_midi(int midi_note) const;
    juce::String value_text(const juce::Slider& slider) const;
    int32_t packed_adsr1() const;
    int32_t packed_adsr2() const;
};

}  // namespace jucewrap
}  // namespace fftplugin
