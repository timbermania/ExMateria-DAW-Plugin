#include "FFTMatchLabComponent.h"

#include "FFTJuceAudioProcessor.h"
#include "FFTJucePaths.h"
#include "fft_plugin/fft_instrument_catalog.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace fftplugin {
namespace jucewrap {

namespace {

constexpr std::array<const char*, 128> kGeneralMidiProgramNames = {{
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
    "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavi",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
    "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)",
    "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
    "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass",
    "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "SynthStrings 1", "SynthStrings 2",
    "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "SynthBrass 1", "SynthBrass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
    "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute",
    "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)",
    "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)",
    "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)",
    "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)",
    "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
    "Sitar", "Banjo", "Shamisen", "Koto",
    "Kalimba", "Bag pipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot"
}};

int32_t preview_volume_from_velocity(int32_t velocity, int32_t dynamics) {
    const double velocity_scale = std::clamp(static_cast<double>(velocity) / 127.0, 0.0, 1.0);
    const double dynamics_scale = std::clamp(static_cast<double>(dynamics) / 127.0, 0.0, 1.0);
    return std::clamp(static_cast<int32_t>(std::lround(0x3FFF * velocity_scale * dynamics_scale)), 0, 0x3FFF);
}

std::pair<int32_t, int32_t> stereo_volumes_from_pan(int32_t base_volume, int32_t pan) {
    const double normalized_pan = std::clamp(static_cast<double>(pan) / 127.0, 0.0, 1.0);
    const double left_gain = std::cos(normalized_pan * juce::MathConstants<double>::halfPi);
    const double right_gain = std::sin(normalized_pan * juce::MathConstants<double>::halfPi);
    return {
        std::clamp(static_cast<int32_t>(std::lround(base_volume * left_gain)), 0, 0x3FFF),
        std::clamp(static_cast<int32_t>(std::lround(base_volume * right_gain)), 0, 0x3FFF),
    };
}

juce::String first_existing_file(std::initializer_list<const char*> paths) {
    for (const char* path : paths) {
        const juce::File file {juce::String(path)};
        if (file.existsAsFile()) {
            return file.getFullPathName();
        }
    }
    return {};
}

juce::String gm_program_name(int program) {
    if (program < 0 || program >= static_cast<int>(kGeneralMidiProgramNames.size())) {
        return "Unknown GM Program";
    }
    return juce::String(kGeneralMidiProgramNames[static_cast<size_t>(program)]);
}

}  // namespace

FFTMatchLabComponent::FFTMatchLabComponent(
    FFTJuceAudioProcessor& processor,
    const std::optional<FFTMatchLabSeed>& seed)
    : processor_(processor) {
    seed_ = seed;
    title_label_.setText("Match Lab", juce::dontSendNotification);
    title_label_.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(title_label_);

    configure_path_label(fluidsynth_path_label_, "FluidSynth");
    configure_path_label(soundfont_path_label_, "SoundFont");
    configure_path_label(reference_wav_label_, "Reference");

    for (auto* button : {
             &choose_fluidsynth_button_,
             &choose_soundfont_button_,
             &choose_reference_wav_button_,
             &render_reference_button_,
             &play_reference_button_,
             &play_fft_button_,
             &play_ab_button_,
             &stop_button_,
         }) {
        button->addListener(this);
        addAndMakeVisible(*button);
    }

    configure_named_slider(gm_program_label_, gm_program_slider_, "GM Program");
    configure_named_slider(midi_note_label_, midi_note_slider_, "GM Note");
    configure_named_slider(velocity_label_, velocity_slider_, "Velocity");
    configure_named_slider(duration_label_, duration_slider_, "Duration ms");
    configure_named_slider(fft_midi_note_label_, fft_midi_note_slider_, "FFT Note");
    configure_named_slider(octave_shift_label_, octave_shift_slider_, "Semitone");
    configure_named_slider(dynamics_label_, dynamics_slider_, "Dynamics");
    configure_named_slider(pan_label_, pan_slider_, "Pan");
    configure_named_slider(adsr_attack_label_, adsr_attack_slider_, "Atk");
    configure_named_slider(adsr_decay_label_, adsr_decay_slider_, "Dec");
    configure_named_slider(adsr_sustain_rate_label_, adsr_sustain_rate_slider_, "SusRt");
    configure_named_slider(adsr_sustain_level_label_, adsr_sustain_level_slider_, "SusLv");
    configure_named_slider(adsr_release_label_, adsr_release_slider_, "Rel");

    configure_slider(gm_program_slider_, 0, 127, 73);
    configure_slider(midi_note_slider_, 0, 127, 74);
    configure_slider(velocity_slider_, 1, 127, 100);
    configure_slider(duration_slider_, 120, 4000, 1400);
    configure_slider(fft_midi_note_slider_, 0, 127, 74);
    configure_slider(octave_shift_slider_, -36, 36, 0, 1);
    configure_slider(dynamics_slider_, 0, 127, 63);
    configure_slider(pan_slider_, 0, 127, 64);
    configure_slider(adsr_attack_slider_, 0, 127, 64);
    configure_slider(adsr_decay_slider_, 0, 15, 8);
    configure_slider(adsr_sustain_rate_slider_, 0, 127, 80);
    configure_slider(adsr_sustain_level_slider_, 0, 15, 8);
    configure_slider(adsr_release_slider_, 0, 31, 10);

    for (auto* slider : {
             &gm_program_slider_,
             &midi_note_slider_,
             &velocity_slider_,
             &duration_slider_,
             &fft_midi_note_slider_,
             &octave_shift_slider_,
             &dynamics_slider_,
             &pan_slider_,
             &adsr_attack_slider_,
             &adsr_decay_slider_,
             &adsr_sustain_rate_slider_,
             &adsr_sustain_level_slider_,
             &adsr_release_slider_,
         }) {
        slider->onValueChange = [this]() {
            refresh_details();
        };
    }

    instrument_search_editor_.setTextToShowWhenEmpty(
        "Search inst by id, hex, name, or category", juce::Colours::grey);
    instrument_search_editor_.addListener(this);
    addAndMakeVisible(instrument_search_editor_);

    instrument_results_list_.setModel(this);
    instrument_results_list_.setOutlineThickness(0);
    addAndMakeVisible(instrument_results_list_);

    details_editor_.setMultiLine(true);
    details_editor_.setReadOnly(true);
    details_editor_.setScrollbarsShown(true);
    details_editor_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha(0.18f));
    details_editor_.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.12f));
    addAndMakeVisible(details_editor_);

    populate_default_paths();
    if (seed_.has_value()) {
        const auto& seeded = *seed_;
        selected_instrument_id_ = seeded.fft_played_sample_id;
        gm_program_slider_.setValue(std::clamp(seeded.gm_program, 0, 127), juce::dontSendNotification);
        midi_note_slider_.setValue(seeded.gm_midi_note, juce::dontSendNotification);
        velocity_slider_.setValue(seeded.velocity, juce::dontSendNotification);
        duration_slider_.setValue(seeded.duration_ms, juce::dontSendNotification);
        dynamics_slider_.setValue(seeded.dynamics, juce::dontSendNotification);
        pan_slider_.setValue(seeded.pan, juce::dontSendNotification);
        fft_midi_note_slider_.setValue(
            std::clamp(seeded.fft_midi_note, 0, 127), juce::dontSendNotification);
        seed_summary_ =
            "Source: " + juce::String(seeded.source_name) +
            " | Row: " + juce::String(seeded.editor_track_name) +
            " | GM " + juce::String(seeded.gm_program) +
            " | " + note_name_for_midi(seeded.gm_midi_note);
    }
    sync_adsr_from_instrument(selected_instrument_id_);
    refresh_instrument_results();
    refresh_details();
    setSize(980, 640);
}

FFTMatchLabComponent::~FFTMatchLabComponent() {
    stopTimer();
    instrument_search_editor_.removeListener(this);
    for (auto* button : {
             &choose_fluidsynth_button_,
             &choose_soundfont_button_,
             &choose_reference_wav_button_,
             &render_reference_button_,
             &play_reference_button_,
             &play_fft_button_,
             &play_ab_button_,
             &stop_button_,
         }) {
        button->removeListener(this);
    }
}

void FFTMatchLabComponent::configure_slider(juce::Slider& slider, double min, double max, double value, double step) {
    slider.setRange(min, max, step);
    slider.setValue(value, juce::dontSendNotification);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 20);
    addAndMakeVisible(slider);
}

void FFTMatchLabComponent::configure_path_label(juce::Label& label, const juce::String& prefix) {
    label.setText(prefix + ": <not set>", juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setColour(juce::Label::backgroundColourId, juce::Colours::white.withAlpha(0.05f));
    label.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.12f));
    addAndMakeVisible(label);
}

void FFTMatchLabComponent::populate_default_paths() {
    // Config file or well-known install locations; empty = use PATH.
    const juce::String cfg_fluidsynth(default_fluidsynth_path());
    fluidsynth_path_ = cfg_fluidsynth.isNotEmpty()
        ? cfg_fluidsynth
        : first_existing_file({
            "C:\\Tools\\FluidSynth\\bin\\fluidsynth.exe",
            "C:\\Tools\\Fluidsynth\\bin\\fluidsynth.exe",
            "C:\\Tools\\FluidSynth\\fluidsynth.exe",
            "C:\\Tools\\Fluidsynth\\fluidsynth.exe",
            "C:\\Program Files\\FluidSynth\\bin\\fluidsynth.exe",
            "C:\\Program Files (x86)\\FluidSynth\\bin\\fluidsynth.exe",
            "C:\\tools\\fluidsynth\\bin\\fluidsynth.exe",
        });
    if (fluidsynth_path_.isNotEmpty()) {
        fluidsynth_path_label_.setText("FluidSynth: " + fluidsynth_path_, juce::dontSendNotification);
    } else {
        // Leave the executable path empty so ChildProcess uses PATH lookup.
        fluidsynth_path_label_.setText("FluidSynth: fluidsynth (PATH)", juce::dontSendNotification);
    }

    const juce::String cfg_soundfont(default_soundfont_path());
    soundfont_path_ = cfg_soundfont.isNotEmpty()
        ? cfg_soundfont
        : first_existing_file({
            // Linux (Arch, Debian/Ubuntu)
            "/usr/share/soundfonts/FluidR3_GM.sf2",
            "/usr/share/sounds/sf2/FluidR3_GM.sf2",
            "/usr/share/soundfonts/FluidR3Mono_GM.sf3",
            // Windows — native
            "C:\\Tools\\FluidSynth\\share\\soundfonts\\default.sf2",
            // Windows — via WSL
            "\\\\wsl.localhost\\Ubuntu\\usr\\share\\sounds\\sf2\\FluidR3_GM.sf2",
            "\\\\wsl$\\Ubuntu\\usr\\share\\sounds\\sf2\\FluidR3_GM.sf2",
        });
    if (soundfont_path_.isNotEmpty()) {
        soundfont_path_label_.setText("SoundFont: " + soundfont_path_, juce::dontSendNotification);
    } else {
        soundfont_path_label_.setText("SoundFont: <choose neutral GM .sf2>", juce::dontSendNotification);
    }

    reference_wav_label_.setText("Reference: <auto-generated by GM RENDER>", juce::dontSendNotification);
}

void FFTMatchLabComponent::configure_named_slider(juce::Label& label, juce::Slider& slider, const juce::String& name) {
    label.setText(name, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(label);
    juce::ignoreUnused(slider);
}

void FFTMatchLabComponent::resized() {
    auto area = getLocalBounds().reduced(10);
    title_label_.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);

    auto path_row = area.removeFromTop(24);
    choose_fluidsynth_button_.setBounds(path_row.removeFromLeft(54));
    fluidsynth_path_label_.setBounds(path_row.removeFromLeft((path_row.getWidth() - 8) / 3));
    path_row.removeFromLeft(4);
    choose_soundfont_button_.setBounds(path_row.removeFromLeft(54));
    soundfont_path_label_.setBounds(path_row.removeFromLeft((path_row.getWidth() - 8) / 2));
    path_row.removeFromLeft(4);
    choose_reference_wav_button_.setBounds(path_row.removeFromLeft(72));
    reference_wav_label_.setBounds(path_row);

    area.removeFromTop(8);
    auto actions = area.removeFromTop(24);
    render_reference_button_.setBounds(actions.removeFromLeft(96));
    actions.removeFromLeft(6);
    play_reference_button_.setBounds(actions.removeFromLeft(82));
    actions.removeFromLeft(6);
    play_fft_button_.setBounds(actions.removeFromLeft(82));
    actions.removeFromLeft(6);
    play_ab_button_.setBounds(actions.removeFromLeft(56));
    actions.removeFromLeft(6);
    stop_button_.setBounds(actions.removeFromLeft(56));

    area.removeFromTop(10);
    auto top_half = area.removeFromTop(248);
    auto controls_left = top_half.removeFromLeft(top_half.getWidth() / 2);
    auto controls_right = top_half;

    auto layout_slider = [](juce::Rectangle<int>& column, juce::Label& label, juce::Slider& slider) {
        auto row = column.removeFromTop(34);
        label.setBounds(row.removeFromLeft(96));
        slider.setBounds(row);
        column.removeFromTop(4);
    };

    layout_slider(controls_left, gm_program_label_, gm_program_slider_);
    layout_slider(controls_left, midi_note_label_, midi_note_slider_);
    layout_slider(controls_left, velocity_label_, velocity_slider_);
    layout_slider(controls_left, duration_label_, duration_slider_);
    layout_slider(controls_left, fft_midi_note_label_, fft_midi_note_slider_);
    layout_slider(controls_left, octave_shift_label_, octave_shift_slider_);
    layout_slider(controls_left, dynamics_label_, dynamics_slider_);
    layout_slider(controls_right, pan_label_, pan_slider_);
    layout_slider(controls_right, adsr_attack_label_, adsr_attack_slider_);
    layout_slider(controls_right, adsr_decay_label_, adsr_decay_slider_);
    layout_slider(controls_right, adsr_sustain_rate_label_, adsr_sustain_rate_slider_);
    layout_slider(controls_right, adsr_sustain_level_label_, adsr_sustain_level_slider_);
    layout_slider(controls_right, adsr_release_label_, adsr_release_slider_);

    area.removeFromTop(8);
    auto bottom = area;
    auto list_area = bottom.removeFromLeft(bottom.getWidth() / 2);
    instrument_search_editor_.setBounds(list_area.removeFromTop(24));
    list_area.removeFromTop(4);
    instrument_results_list_.setBounds(list_area);
    bottom.removeFromLeft(8);
    details_editor_.setBounds(bottom);
}

void FFTMatchLabComponent::launch_file_chooser(
    const juce::String& title,
    const juce::String& pattern,
    std::function<void(const juce::File&)> on_pick
) {
    chooser_ = std::make_unique<juce::FileChooser>(
        title,
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        pattern);
    chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, on_pick = std::move(on_pick)](const juce::FileChooser& chooser) mutable {
            const juce::File file = chooser.getResult();
            if (file.existsAsFile()) {
                on_pick(file);
            }
            chooser_.reset();
        });
}

void FFTMatchLabComponent::buttonClicked(juce::Button* button) {
    if (button == &choose_fluidsynth_button_) {
        launch_file_chooser("Choose fluidsynth executable", "*.exe;*", [this](const juce::File& file) {
            fluidsynth_path_ = file.getFullPathName();
            fluidsynth_path_label_.setText("FluidSynth: " + fluidsynth_path_, juce::dontSendNotification);
        });
        return;
    }
    if (button == &choose_soundfont_button_) {
        launch_file_chooser("Choose GM soundfont", "*.sf2;*.SF2", [this](const juce::File& file) {
            soundfont_path_ = file.getFullPathName();
            soundfont_path_label_.setText("SoundFont: " + soundfont_path_, juce::dontSendNotification);
        });
        return;
    }
    if (button == &choose_reference_wav_button_) {
        launch_file_chooser("Choose reference WAV", "*.wav;*.WAV", [this](const juce::File& file) {
            reference_wav_path_ = file.getFullPathName();
            reference_wav_label_.setText("Reference: " + reference_wav_path_, juce::dontSendNotification);
            processor_.load_match_lab_reference_wav(reference_wav_path_);
        });
        return;
    }
    if (button == &render_reference_button_) {
        render_gm_reference();
        return;
    }
    if (button == &play_reference_button_) {
        play_reference();
        return;
    }
    if (button == &play_fft_button_) {
        play_fft_candidate();
        return;
    }
    if (button == &play_ab_button_) {
        if (play_reference()) {
            waiting_to_play_fft_ = true;
            pending_fft_request_ = build_fft_request();
            pending_fft_duration_ms_ = static_cast<int>(std::lround(duration_slider_.getValue()));
            pending_fft_release_ms_ = preview_release_ms();
            startTimer(pending_fft_duration_ms_ + 180);
        } else {
            play_fft_candidate();
        }
        return;
    }
    if (button == &stop_button_) {
        stopTimer();
        waiting_to_play_fft_ = false;
        processor_.stop_match_lab_reference();
        return;
    }
}

int FFTMatchLabComponent::getNumRows() {
    return static_cast<int>(instrument_results_.size());
}

void FFTMatchLabComponent::paintListBoxItem(
    int row_number,
    juce::Graphics& g,
    int width,
    int height,
    bool row_is_selected
) {
    if (row_number < 0 || static_cast<size_t>(row_number) >= instrument_results_.size()) {
        return;
    }
    const auto& entry = instrument_results_[static_cast<size_t>(row_number)];
    if (row_is_selected) {
        g.fillAll(juce::Colour::fromRGB(67, 92, 132));
    } else if (entry.id == selected_instrument_id_) {
        g.fillAll(juce::Colour::fromRGB(52, 60, 74));
    }

    g.setColour(juce::Colours::white.withAlpha(0.96f));
    const juce::String hex = juce::String::toHexString(entry.id).toUpperCase();
    const juce::String line =
        juce::String(entry.id).paddedRight(' ', 3) + " | " +
        hex.paddedRight(' ', 2) + " | " +
        juce::String(entry.name) + " | " +
        juce::String(entry.major_group.empty() ? "-" : entry.major_group.c_str()) +
        juce::String(entry.waveset_dependency.empty() ? "" : (" | " + entry.waveset_dependency).c_str());
    g.drawText(line, 6, 0, width - 12, height, juce::Justification::centredLeft, false);
}

void FFTMatchLabComponent::listBoxItemClicked(int row, const juce::MouseEvent&) {
    if (row < 0 || static_cast<size_t>(row) >= instrument_results_.size()) {
        return;
    }
    apply_instrument_id(instrument_results_[static_cast<size_t>(row)].id);
}

void FFTMatchLabComponent::textEditorTextChanged(juce::TextEditor& editor) {
    if (&editor == &instrument_search_editor_) {
        refresh_instrument_results();
    }
}

void FFTMatchLabComponent::timerCallback() {
    stopTimer();
    if (waiting_to_play_fft_) {
        waiting_to_play_fft_ = false;
        processor_.play_match_lab_fft_note(pending_fft_request_, pending_fft_duration_ms_, pending_fft_release_ms_);
    }
}

void FFTMatchLabComponent::render_gm_reference() {
    const juce::String executable = fluidsynth_path_.isNotEmpty() ? fluidsynth_path_ : "fluidsynth";
    if (soundfont_path_.isEmpty()) {
        details_editor_.setText("Choose a GM soundfont (.sf2) first.", juce::dontSendNotification);
        return;
    }

    const juce::File temp_dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("fft_match_lab");
    temp_dir.createDirectory();
    const juce::String render_id = juce::Uuid().toString().retainCharacters("0123456789abcdefABCDEF");
    const juce::File midi_file = temp_dir.getChildFile("match_note_" + render_id + ".mid");
    const juce::File wav_file = temp_dir.getChildFile("match_note_" + render_id + ".wav");

    juce::MidiMessageSequence sequence;
    sequence.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0.0);
    sequence.addEvent(juce::MidiMessage::programChange(1, static_cast<int>(std::lround(gm_program_slider_.getValue()))), 0.0);
    const int gm_volume = seed_.has_value() ? std::clamp(seed_->gm_volume, 0, 127) : 100;
    const int gm_pan = seed_.has_value() ? std::clamp(seed_->gm_pan, 0, 127) : 64;
    const int gm_expression = seed_.has_value() ? std::clamp(seed_->gm_expression, 0, 127) : 127;
    sequence.addEvent(juce::MidiMessage::controllerEvent(1, 7, gm_volume), 0.0);
    sequence.addEvent(juce::MidiMessage::controllerEvent(1, 10, gm_pan), 0.0);
    sequence.addEvent(juce::MidiMessage::controllerEvent(1, 11, gm_expression), 0.0);
    sequence.addEvent(
        juce::MidiMessage::noteOn(
            1,
            static_cast<int>(std::lround(midi_note_slider_.getValue())),
            static_cast<juce::uint8>(std::lround(velocity_slider_.getValue()))),
        0.0);
    const double note_off_tick =
        (static_cast<double>(duration_slider_.getValue()) / 1000.0) * 960.0;
    sequence.addEvent(
        juce::MidiMessage::noteOff(1, static_cast<int>(std::lround(midi_note_slider_.getValue()))),
        note_off_tick);
    sequence.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(480);
    midi.addTrack(sequence);
    {
        juce::FileOutputStream stream(midi_file);
        if (!stream.openedOk() || !midi.writeTo(stream)) {
            details_editor_.setText("Failed to write temporary MIDI file.", juce::dontSendNotification);
            return;
        }
    }

    juce::ChildProcess process;
    juce::StringArray args;
    args.add(executable);
    args.add("-q");
    args.add("-ni");
    args.add("-F");
    args.add(wav_file.getFullPathName());
    args.add("-T");
    args.add("wav");
    args.add("-O");
    args.add("s16");
    args.add("-r");
    args.add("44100");
    args.add(soundfont_path_);
    args.add(midi_file.getFullPathName());

    if (!process.start(args)) {
        details_editor_.setText("Failed to start fluidsynth. Choose the executable explicitly if it is not in PATH.", juce::dontSendNotification);
        return;
    }
    const bool finished = process.waitForProcessToFinish(20000);
    const auto exit_code = process.getExitCode();
    if (!finished) {
        details_editor_.setText("FluidSynth timed out while rendering the GM reference.", juce::dontSendNotification);
        return;
    }
    if (exit_code != 0) {
        details_editor_.setText(
            "FluidSynth failed while rendering the GM reference (exit code " + juce::String(static_cast<int>(exit_code)) + ").",
            juce::dontSendNotification);
        return;
    }
    if (!wav_file.existsAsFile() || wav_file.getSize() <= 0) {
        details_editor_.setText("FluidSynth did not produce a usable reference WAV.", juce::dontSendNotification);
        return;
    }

    reference_wav_path_ = wav_file.getFullPathName();
    reference_wav_label_.setText("Reference: " + reference_wav_path_, juce::dontSendNotification);
    processor_.load_match_lab_reference_wav(reference_wav_path_);
    processor_.play_match_lab_reference();
}

void FFTMatchLabComponent::refresh_instrument_results() {
    instrument_results_ = search_fft_instrument_catalog(
        instrument_search_editor_.getText().toStdString(), 256);
    instrument_results_list_.updateContent();
    int selected_row = -1;
    for (size_t i = 0; i < instrument_results_.size(); ++i) {
        if (instrument_results_[i].id == selected_instrument_id_) {
            selected_row = static_cast<int>(i);
            break;
        }
    }
    if (selected_row >= 0) {
        instrument_results_list_.selectRow(selected_row);
        instrument_results_list_.scrollToEnsureRowIsOnscreen(selected_row);
    } else {
        instrument_results_list_.deselectAllRows();
    }
    repaint();
}

void FFTMatchLabComponent::apply_instrument_id(int32_t id) {
    selected_instrument_id_ = id;
    refresh_instrument_results();
    refresh_details();
}

void FFTMatchLabComponent::refresh_details() {
    std::ostringstream out;
    if (seed_summary_.isNotEmpty()) {
        out << seed_summary_.toStdString() << "\n";
    }
    out << "GM program: " << static_cast<int>(std::lround(gm_program_slider_.getValue()))
        << " (" << gm_program_name(static_cast<int>(std::lround(gm_program_slider_.getValue()))).toStdString() << ")"
        << "\nGM note: " << note_name_for_midi(static_cast<int>(std::lround(midi_note_slider_.getValue()))).toStdString()
        << " (" << static_cast<int>(std::lround(midi_note_slider_.getValue())) << ")"
        << "\nFFT note: " << note_name_for_midi(static_cast<int>(std::lround(fft_midi_note_slider_.getValue()))).toStdString()
        << " (" << static_cast<int>(std::lround(fft_midi_note_slider_.getValue())) << ")"
        << "\nVelocity: " << value_text(velocity_slider_).toStdString()
        << "\nDuration ms: " << value_text(duration_slider_).toStdString();
    if (seed_.has_value()) {
        out << "\nGM volume/pan/expression: "
            << seed_->gm_volume << " / "
            << seed_->gm_pan << " / "
            << seed_->gm_expression;
    }

    out << "\n\nCurrent FFT match:";
    const auto* entry = find_fft_instrument_catalog_entry(selected_instrument_id_);
    if (entry != nullptr) {
        const juce::String hex = juce::String::toHexString(entry->id).toUpperCase();
        out << "\nID: " << entry->id << " (0x" << hex.toStdString() << ")"
            << "\nName: " << entry->name
            << "\nGroup: " << (entry->major_group.empty() ? "-" : entry->major_group);
        if (!entry->minor_group.empty()) {
            out << " / " << entry->minor_group;
        }
        if (!entry->waveset_dependency.empty()) {
            out << "\nWaveset: " << entry->waveset_dependency;
        }
        if (entry->root_midi_note >= 0) {
            out << "\nRoot: " << entry->root_note_name
                << " (" << entry->root_midi_note << ")";
        }
    } else {
        out << "\nID: " << selected_instrument_id_ << " (no catalog entry)";
    }

    out << "\n\nFFT controls:"
        << "\nDynamics: " << value_text(dynamics_slider_).toStdString()
        << "\nPan: " << value_text(pan_slider_).toStdString()
        << "\nPreview release ms: " << preview_release_ms()
        << "\nAtk/Dec/SusRt/SusLv/Rel: "
        << value_text(adsr_attack_slider_).toStdString() << " / "
        << value_text(adsr_decay_slider_).toStdString() << " / "
        << value_text(adsr_sustain_rate_slider_).toStdString() << " / "
        << value_text(adsr_sustain_level_slider_).toStdString() << " / "
        << value_text(adsr_release_slider_).toStdString();
    details_editor_.setText(juce::String(out.str()), juce::dontSendNotification);
}

void FFTMatchLabComponent::sync_adsr_from_instrument(int32_t id) {
    const auto info = processor_.loaded_instrument_info(id);
    if (!info.has_value()) {
        return;
    }
    adsr_attack_slider_.setValue((info->adsr1 >> 8) & 0x7F, juce::dontSendNotification);
    adsr_decay_slider_.setValue((info->adsr1 >> 4) & 0x0F, juce::dontSendNotification);
    adsr_sustain_level_slider_.setValue(info->adsr1 & 0x0F, juce::dontSendNotification);
    adsr_sustain_rate_slider_.setValue((info->adsr2 >> 6) & 0x7F, juce::dontSendNotification);
    adsr_release_slider_.setValue(info->adsr2 & 0x1F, juce::dontSendNotification);
}

FFTPreviewNoteRequest FFTMatchLabComponent::build_fft_request() const {
    FFTPreviewNoteRequest request;
    request.instrument_id = selected_instrument_id_;
    request.midi_note = static_cast<int16_t>(std::clamp(
        static_cast<int>(std::lround(fft_midi_note_slider_.getValue())),
        0,
        127));
    request.velocity = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(velocity_slider_.getValue())), 1, 127));
    request.octave_shift = static_cast<int32_t>(std::lround(octave_shift_slider_.getValue()));
    const int32_t base_volume = preview_volume_from_velocity(
        static_cast<int>(std::lround(velocity_slider_.getValue())),
        static_cast<int>(std::lround(dynamics_slider_.getValue())));
    const auto [left_volume, right_volume] = stereo_volumes_from_pan(
        base_volume,
        static_cast<int>(std::lround(pan_slider_.getValue())));
    request.left_volume_override = left_volume;
    request.right_volume_override = right_volume;
    request.adsr1_override = packed_adsr1();
    request.adsr2_override = packed_adsr2();
    return request;
}

int FFTMatchLabComponent::preview_release_ms() const {
    return std::clamp(
        60 + static_cast<int>(std::lround(adsr_release_slider_.getValue())) * 64,
        60,
        2200);
}

bool FFTMatchLabComponent::play_fft_candidate() {
    return processor_.play_match_lab_fft_note(
        build_fft_request(),
        static_cast<int>(std::lround(duration_slider_.getValue())),
        preview_release_ms());
}

bool FFTMatchLabComponent::play_reference() {
    if (reference_wav_path_.isEmpty()) {
        return false;
    }
    processor_.play_match_lab_reference();
    return true;
}

juce::String FFTMatchLabComponent::note_name_for_midi(int midi_note) const {
    static const std::array<const char*, 12> kNames = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int clamped = std::clamp(midi_note, 0, 127);
    return juce::String(kNames[static_cast<size_t>(clamped % 12)]) + juce::String(clamped / 12);
}

juce::String FFTMatchLabComponent::value_text(const juce::Slider& slider) const {
    return juce::String(static_cast<int>(std::lround(slider.getValue())));
}

int32_t FFTMatchLabComponent::packed_adsr1() const {
    return (static_cast<int32_t>(std::lround(adsr_attack_slider_.getValue())) << 8) |
        (static_cast<int32_t>(std::lround(adsr_decay_slider_.getValue())) << 4) |
        static_cast<int32_t>(std::lround(adsr_sustain_level_slider_.getValue()));
}

int32_t FFTMatchLabComponent::packed_adsr2() const {
    int32_t base_adsr2 = (1 << 14);
    if (const auto info = processor_.loaded_instrument_info(selected_instrument_id_); info.has_value()) {
        base_adsr2 = info->adsr2;
    }
    const int32_t preserved_mode_bits = base_adsr2 & ((1 << 15) | (1 << 14) | (1 << 5));
    return preserved_mode_bits |
        (static_cast<int32_t>(std::lround(adsr_sustain_rate_slider_.getValue())) << 6) |
        static_cast<int32_t>(std::lround(adsr_release_slider_.getValue()));
}

}  // namespace jucewrap
}  // namespace fftplugin
