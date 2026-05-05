#include "FFTJuceAudioProcessorEditor.h"

#include "FFTJuceAudioProcessor.h"
#include "FFTMatchLabComponent.h"
#include "fft_plugin/fft_instrument_catalog.h"
#include "fft_plugin/fft_smd_presentation_utils.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fftplugin {
namespace jucewrap {

namespace {

struct FFTOpcodeChoiceEntry {
    juce::String name;
    int opcode = -1;
    juce::String description;
};

std::string unwound_insert_debug_log_path() {
    if (const char* temp = std::getenv("TEMP"); temp != nullptr && *temp != '\0') {
        return std::string(temp) + "\\fft_unwound_insert_debug.log";
    }
    return "C:\\Windows\\Temp\\fft_unwound_insert_debug.log";
}

void append_unwound_insert_debug_line(const std::string& line) {
    const std::filesystem::path path(unwound_insert_debug_log_path());
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << line << "\n";
}

class FFTInstrumentPickerContent final : public juce::Component,
                                         private juce::TextEditor::Listener,
                                         private juce::KeyListener,
                                         private juce::ListBoxModel {
public:
    FFTInstrumentPickerContent(
        int32_t current_instrument_id,
        std::function<void(int32_t)> on_pick)
        : current_instrument_id_(current_instrument_id),
          on_pick_(std::move(on_pick)) {
        search_editor_.setTextToShowWhenEmpty("Search by id, hex, name, or category", juce::Colours::grey);
        search_editor_.addListener(this);
        search_editor_.addKeyListener(this);
        addAndMakeVisible(search_editor_);

        results_list_.setModel(this);
        results_list_.setOutlineThickness(0);
        addAndMakeVisible(results_list_);

        hint_label_.setText("Enter or double-click to apply", juce::dontSendNotification);
        hint_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(hint_label_);

        refresh_results();
        if (!results_.empty()) {
            results_list_.selectRow(0);
        }
        search_editor_.grabKeyboardFocus();
        setSize(620, 360);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        search_editor_.setBounds(area.removeFromTop(26));
        area.removeFromTop(6);
        hint_label_.setBounds(area.removeFromBottom(18));
        area.removeFromBottom(6);
        results_list_.setBounds(area);
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
            if (results_.empty()) {
                return true;
            }
            const int direction = (key == juce::KeyPress::upKey) ? -1 : 1;
            int row = results_list_.getSelectedRow();
            if (row < 0) {
                row = 0;
            } else {
                row = juce::jlimit(0, static_cast<int>(results_.size()) - 1, row + direction);
            }
            results_list_.selectRow(row);
            results_list_.scrollToEnsureRowIsOnscreen(row);
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    int getNumRows() override {
        return static_cast<int>(results_.size());
    }

    void paintListBoxItem(int row_number, juce::Graphics& g, int width, int height, bool row_is_selected) override {
        if (row_number < 0 || static_cast<size_t>(row_number) >= results_.size()) {
            return;
        }
        const auto& entry = results_[static_cast<size_t>(row_number)];
        if (row_is_selected) {
            g.fillAll(juce::Colour::fromRGB(67, 92, 132));
        } else if (entry.id == current_instrument_id_) {
            g.fillAll(juce::Colour::fromRGB(52, 60, 74));
        }

        g.setColour(juce::Colours::white.withAlpha(0.96f));
        const int32_t played_sample_id = entry.id;
        const juce::String played_hex = juce::String::toHexString(played_sample_id).toUpperCase();
        const juce::String line =
            juce::String(played_sample_id).paddedRight(' ', 3) + " | " +
            played_hex.paddedRight(' ', 2) + " | " +
            juce::String(entry.name) + " | " +
            juce::String(entry.major_group.empty() ? "-" : entry.major_group.c_str()) +
            juce::String(entry.waveset_dependency.empty() ? "" : (" | " + entry.waveset_dependency).c_str());
        g.drawText(line, 6, 0, width - 12, height, juce::Justification::centredLeft, false);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override {
        results_list_.selectRow(row);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        apply_row(row);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::returnKey) {
            apply_row(results_list_.getSelectedRow());
            return true;
        }
        return juce::Component::keyPressed(key);
    }

    void textEditorTextChanged(juce::TextEditor&) override {
        refresh_results();
    }

    void textEditorReturnKeyPressed(juce::TextEditor&) override {
        apply_row(results_list_.getSelectedRow() >= 0 ? results_list_.getSelectedRow() : 0);
    }

    void textEditorEscapeKeyPressed(juce::TextEditor&) override {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    void refresh_results() {
        results_ = search_fft_instrument_catalog(search_editor_.getText().toStdString(), 256);
        results_list_.updateContent();
        if (!results_.empty()) {
            int selected_row = 0;
            for (size_t i = 0; i < results_.size(); ++i) {
                if (results_[i].id == current_instrument_id_) {
                    selected_row = static_cast<int>(i);
                    break;
                }
            }
            results_list_.selectRow(selected_row);
            results_list_.scrollToEnsureRowIsOnscreen(selected_row);
        }
        repaint();
    }

    void apply_row(int row) {
        if (row < 0 || static_cast<size_t>(row) >= results_.size() || !on_pick_) {
            return;
        }
        on_pick_(results_[static_cast<size_t>(row)].id);
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    int32_t current_instrument_id_ = 0;
    std::function<void(int32_t)> on_pick_;
    juce::TextEditor search_editor_;
    juce::ListBox results_list_;
    juce::Label hint_label_;
    std::vector<FFTInstrumentCatalogEntry> results_;
};

class FFTValuePickerContent final : public juce::Component,
                                    private juce::Slider::Listener,
                                    private juce::KeyListener {
public:
    FFTValuePickerContent(
        const juce::String& title,
        int32_t min_value,
        int32_t max_value,
        int32_t current_value,
        std::function<void(int32_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        title_label_.setText(title, juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(value_label_);

        slider_.setRange(min_value, max_value, 1.0);
        slider_.setValue(current_value, juce::dontSendNotification);
        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider_.addListener(this);
        slider_.addKeyListener(this);
        addAndMakeVisible(slider_);

        apply_button_.setButtonText("Apply");
        apply_button_.onClick = [this]() { apply(); };
        addAndMakeVisible(apply_button_);

        cancel_button_.setButtonText("Cancel");
        cancel_button_.onClick = [this]() {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
        };
        addAndMakeVisible(cancel_button_);

        update_value_label();
        setSize(320, 120);
        slider_.grabKeyboardFocus();
        juce::Desktop::getInstance().addGlobalMouseListener(this);
    }

    ~FFTValuePickerContent() override {
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(20);
        title_label_.setBounds(top.removeFromLeft(area.getWidth() / 2));
        value_label_.setBounds(top);
        area.removeFromTop(10);
        slider_.setBounds(area.removeFromTop(24));
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(24);
        apply_button_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        cancel_button_.setBounds(buttons.removeFromLeft(70));
    }

private:
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override {
        adjust_from_wheel(wheel);
    }

    void adjust_from_wheel(const juce::MouseWheelDetails& wheel) {
        const float delta = std::abs(wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX;
        if (std::abs(delta) <= 0.0f) {
            return;
        }
        const double step = delta > 0.0f ? 1.0 : -1.0;
        slider_.setValue(
            juce::jlimit(slider_.getMinimum(), slider_.getMaximum(), slider_.getValue() + step),
            juce::sendNotificationSync);
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::returnKey) {
            apply();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    void sliderValueChanged(juce::Slider*) override {
        update_value_label();
    }

    void update_value_label() {
        value_label_.setText(juce::String(static_cast<int>(slider_.getValue())), juce::dontSendNotification);
    }

    void apply() {
        if (on_apply_) {
            on_apply_(static_cast<int32_t>(std::lround(slider_.getValue())));
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    std::function<void(int32_t)> on_apply_;
    juce::Label title_label_;
    juce::Label value_label_;
    juce::Slider slider_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class FFTTimeSignaturePickerContent final : public juce::Component,
                                           private juce::Slider::Listener,
                                           private juce::KeyListener {
public:
    FFTTimeSignaturePickerContent(
        int32_t current_numerator,
        int32_t current_denominator,
        std::function<void(int32_t, int32_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        title_label_.setText("Time Signature", juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        numerator_label_.setText("Num", juce::dontSendNotification);
        addAndMakeVisible(numerator_label_);
        denominator_label_.setText("Den", juce::dontSendNotification);
        addAndMakeVisible(denominator_label_);

        numerator_slider_.setRange(1, 16, 1.0);
        numerator_slider_.setValue(current_numerator, juce::dontSendNotification);
        numerator_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        numerator_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        numerator_slider_.addListener(this);
        numerator_slider_.addKeyListener(this);
        addAndMakeVisible(numerator_slider_);

        denominator_slider_.setRange(1, 16, 1.0);
        denominator_slider_.setValue(current_denominator, juce::dontSendNotification);
        denominator_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        denominator_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        denominator_slider_.addListener(this);
        denominator_slider_.addKeyListener(this);
        addAndMakeVisible(denominator_slider_);

        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(value_label_);

        apply_button_.setButtonText("Apply");
        apply_button_.onClick = [this]() { apply(); };
        addAndMakeVisible(apply_button_);

        cancel_button_.setButtonText("Cancel");
        cancel_button_.onClick = [this]() {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
        };
        addAndMakeVisible(cancel_button_);

        update_value_label();
        setSize(340, 150);
        numerator_slider_.grabKeyboardFocus();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(20);
        title_label_.setBounds(top.removeFromLeft(150));
        value_label_.setBounds(top);
        area.removeFromTop(8);
        auto num_row = area.removeFromTop(24);
        numerator_label_.setBounds(num_row.removeFromLeft(36));
        numerator_slider_.setBounds(num_row);
        area.removeFromTop(8);
        auto den_row = area.removeFromTop(24);
        denominator_label_.setBounds(den_row.removeFromLeft(36));
        denominator_slider_.setBounds(den_row);
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(24);
        apply_button_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        cancel_button_.setBounds(buttons.removeFromLeft(70));
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::returnKey) {
            apply();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    void sliderValueChanged(juce::Slider*) override {
        update_value_label();
    }

    void update_value_label() {
        value_label_.setText(
            juce::String(static_cast<int>(numerator_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(denominator_slider_.getValue())),
            juce::dontSendNotification);
    }

    void apply() {
        if (on_apply_) {
            on_apply_(
                static_cast<int32_t>(std::lround(numerator_slider_.getValue())),
                static_cast<int32_t>(std::lround(denominator_slider_.getValue())));
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    std::function<void(int32_t, int32_t)> on_apply_;
    juce::Label title_label_;
    juce::Label numerator_label_;
    juce::Label denominator_label_;
    juce::Label value_label_;
    juce::Slider numerator_slider_;
    juce::Slider denominator_slider_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class FFTADSRC7PickerContent final : public juce::Component,
                                     private juce::Slider::Listener,
                                     private juce::KeyListener {
public:
    FFTADSRC7PickerContent(
        int32_t current_decay,
        int32_t current_sustain_level,
        std::function<void(int32_t, int32_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        title_label_.setText("ADSR Decay + Sustain", juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        decay_label_.setText("Decay", juce::dontSendNotification);
        addAndMakeVisible(decay_label_);
        sustain_label_.setText("Sustain", juce::dontSendNotification);
        addAndMakeVisible(sustain_label_);

        decay_slider_.setRange(0, 15, 1.0);
        decay_slider_.setValue(current_decay, juce::dontSendNotification);
        decay_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        decay_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        decay_slider_.addListener(this);
        decay_slider_.addKeyListener(this);
        addAndMakeVisible(decay_slider_);

        sustain_slider_.setRange(0, 15, 1.0);
        sustain_slider_.setValue(current_sustain_level, juce::dontSendNotification);
        sustain_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        sustain_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        sustain_slider_.addListener(this);
        sustain_slider_.addKeyListener(this);
        addAndMakeVisible(sustain_slider_);

        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(value_label_);

        apply_button_.setButtonText("Apply");
        apply_button_.onClick = [this]() { apply(); };
        addAndMakeVisible(apply_button_);

        cancel_button_.setButtonText("Cancel");
        cancel_button_.onClick = [this]() {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
        };
        addAndMakeVisible(cancel_button_);

        update_value_label();
        setSize(340, 150);
        decay_slider_.grabKeyboardFocus();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(20);
        title_label_.setBounds(top.removeFromLeft(150));
        value_label_.setBounds(top);
        area.removeFromTop(8);
        auto decay_row = area.removeFromTop(24);
        decay_label_.setBounds(decay_row.removeFromLeft(52));
        decay_slider_.setBounds(decay_row);
        area.removeFromTop(8);
        auto sustain_row = area.removeFromTop(24);
        sustain_label_.setBounds(sustain_row.removeFromLeft(52));
        sustain_slider_.setBounds(sustain_row);
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(24);
        apply_button_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        cancel_button_.setBounds(buttons.removeFromLeft(70));
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::returnKey) {
            apply();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    void sliderValueChanged(juce::Slider*) override {
        update_value_label();
    }

    void update_value_label() {
        value_label_.setText(
            juce::String(static_cast<int>(decay_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(sustain_slider_.getValue())),
            juce::dontSendNotification);
    }

    void apply() {
        if (on_apply_) {
            on_apply_(
                static_cast<int32_t>(std::lround(decay_slider_.getValue())),
                static_cast<int32_t>(std::lround(sustain_slider_.getValue())));
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    std::function<void(int32_t, int32_t)> on_apply_;
    juce::Label title_label_;
    juce::Label decay_label_;
    juce::Label sustain_label_;
    juce::Label value_label_;
    juce::Slider decay_slider_;
    juce::Slider sustain_slider_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class FFTRawPairPickerContent final : public juce::Component,
                                      private juce::Slider::Listener,
                                      private juce::KeyListener {
public:
    FFTRawPairPickerContent(
        const juce::String& title,
        const juce::String& first_label,
        const juce::String& second_label,
        int32_t current_first,
        int32_t current_second,
        std::function<void(int32_t, int32_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        title_label_.setText(title, juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        first_value_label_.setText(first_label, juce::dontSendNotification);
        addAndMakeVisible(first_value_label_);
        second_value_label_.setText(second_label, juce::dontSendNotification);
        addAndMakeVisible(second_value_label_);

        first_slider_.setRange(0, 255, 1.0);
        first_slider_.setValue(current_first, juce::dontSendNotification);
        first_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        first_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        first_slider_.addListener(this);
        first_slider_.addKeyListener(this);
        addAndMakeVisible(first_slider_);

        second_slider_.setRange(0, 255, 1.0);
        second_slider_.setValue(current_second, juce::dontSendNotification);
        second_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        second_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        second_slider_.addListener(this);
        second_slider_.addKeyListener(this);
        addAndMakeVisible(second_slider_);

        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(value_label_);

        apply_button_.setButtonText("Apply");
        apply_button_.onClick = [this]() { apply(); };
        addAndMakeVisible(apply_button_);

        cancel_button_.setButtonText("Cancel");
        cancel_button_.onClick = [this]() {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
        };
        addAndMakeVisible(cancel_button_);

        update_value_label();
        setSize(340, 150);
        first_slider_.grabKeyboardFocus();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(20);
        title_label_.setBounds(top.removeFromLeft(160));
        value_label_.setBounds(top);
        area.removeFromTop(8);
        auto first_row = area.removeFromTop(24);
        first_value_label_.setBounds(first_row.removeFromLeft(44));
        first_slider_.setBounds(first_row);
        area.removeFromTop(8);
        auto second_row = area.removeFromTop(24);
        second_value_label_.setBounds(second_row.removeFromLeft(44));
        second_slider_.setBounds(second_row);
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(24);
        apply_button_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        cancel_button_.setBounds(buttons.removeFromLeft(70));
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::returnKey) {
            apply();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    void sliderValueChanged(juce::Slider*) override {
        update_value_label();
    }

    void update_value_label() {
        value_label_.setText(
            juce::String(static_cast<int>(first_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(second_slider_.getValue())),
            juce::dontSendNotification);
    }

    void apply() {
        if (on_apply_) {
            on_apply_(
                static_cast<int32_t>(std::lround(first_slider_.getValue())),
                static_cast<int32_t>(std::lround(second_slider_.getValue())));
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    std::function<void(int32_t, int32_t)> on_apply_;
    juce::Label title_label_;
    juce::Label first_value_label_;
    juce::Label second_value_label_;
    juce::Label value_label_;
    juce::Slider first_slider_;
    juce::Slider second_slider_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class FFTPitchLFOPickerContent final : public juce::Component,
                                       private juce::Slider::Listener,
                                       private juce::KeyListener {
public:
    FFTPitchLFOPickerContent(
        const juce::String& title,
        int32_t current_length,
        int32_t current_shape,
        int32_t current_depth,
        std::function<void(int32_t, int32_t, int32_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        title_label_.setText(title, juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        length_label_.setText("Len", juce::dontSendNotification);
        addAndMakeVisible(length_label_);
        shape_label_.setText("Shape", juce::dontSendNotification);
        addAndMakeVisible(shape_label_);
        depth_label_.setText("Depth", juce::dontSendNotification);
        addAndMakeVisible(depth_label_);

        length_slider_.setRange(0, 255, 1.0);
        length_slider_.setValue(current_length, juce::dontSendNotification);
        length_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        length_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        length_slider_.addListener(this);
        length_slider_.addKeyListener(this);
        addAndMakeVisible(length_slider_);

        shape_slider_.setRange(-128, 127, 1.0);
        shape_slider_.setValue(current_shape, juce::dontSendNotification);
        shape_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        shape_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        shape_slider_.addListener(this);
        shape_slider_.addKeyListener(this);
        addAndMakeVisible(shape_slider_);

        depth_slider_.setRange(0, 255, 1.0);
        depth_slider_.setValue(current_depth, juce::dontSendNotification);
        depth_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        depth_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        depth_slider_.addListener(this);
        depth_slider_.addKeyListener(this);
        addAndMakeVisible(depth_slider_);

        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(value_label_);

        apply_button_.setButtonText("Apply");
        apply_button_.onClick = [this]() { apply(); };
        addAndMakeVisible(apply_button_);

        cancel_button_.setButtonText("Cancel");
        cancel_button_.onClick = [this]() {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
        };
        addAndMakeVisible(cancel_button_);

        update_value_label();
        setSize(360, 180);
        length_slider_.grabKeyboardFocus();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(20);
        title_label_.setBounds(top.removeFromLeft(150));
        value_label_.setBounds(top);
        area.removeFromTop(8);
        auto len_row = area.removeFromTop(24);
        length_label_.setBounds(len_row.removeFromLeft(44));
        length_slider_.setBounds(len_row);
        area.removeFromTop(8);
        auto shape_row = area.removeFromTop(24);
        shape_label_.setBounds(shape_row.removeFromLeft(44));
        shape_slider_.setBounds(shape_row);
        area.removeFromTop(8);
        auto depth_row = area.removeFromTop(24);
        depth_label_.setBounds(depth_row.removeFromLeft(44));
        depth_slider_.setBounds(depth_row);
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(24);
        apply_button_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        cancel_button_.setBounds(buttons.removeFromLeft(70));
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::returnKey) {
            apply();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    void sliderValueChanged(juce::Slider*) override {
        update_value_label();
    }

    void update_value_label() {
        value_label_.setText(
            juce::String(static_cast<int>(length_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(shape_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(depth_slider_.getValue())),
            juce::dontSendNotification);
    }

    void apply() {
        if (on_apply_) {
            on_apply_(
                static_cast<int32_t>(std::lround(length_slider_.getValue())),
                static_cast<int32_t>(std::lround(shape_slider_.getValue())),
                static_cast<int32_t>(std::lround(depth_slider_.getValue())));
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    std::function<void(int32_t, int32_t, int32_t)> on_apply_;
    juce::Label title_label_;
    juce::Label length_label_;
    juce::Label shape_label_;
    juce::Label depth_label_;
    juce::Label value_label_;
    juce::Slider length_slider_;
    juce::Slider shape_slider_;
    juce::Slider depth_slider_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class FFTRawTriplePickerContent final : public juce::Component,
                                        private juce::Slider::Listener,
                                        private juce::KeyListener {
public:
    FFTRawTriplePickerContent(
        const juce::String& title,
        const juce::String& first_label,
        const juce::String& second_label,
        const juce::String& third_label,
        int32_t current_first,
        int32_t current_second,
        int32_t current_third,
        std::function<void(int32_t, int32_t, int32_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        title_label_.setText(title, juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        first_value_label_.setText(first_label, juce::dontSendNotification);
        addAndMakeVisible(first_value_label_);
        second_value_label_.setText(second_label, juce::dontSendNotification);
        addAndMakeVisible(second_value_label_);
        third_value_label_.setText(third_label, juce::dontSendNotification);
        addAndMakeVisible(third_value_label_);

        first_slider_.setRange(0, 255, 1.0);
        first_slider_.setValue(current_first, juce::dontSendNotification);
        first_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        first_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        first_slider_.addListener(this);
        first_slider_.addKeyListener(this);
        addAndMakeVisible(first_slider_);

        second_slider_.setRange(0, 255, 1.0);
        second_slider_.setValue(current_second, juce::dontSendNotification);
        second_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        second_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        second_slider_.addListener(this);
        second_slider_.addKeyListener(this);
        addAndMakeVisible(second_slider_);

        third_slider_.setRange(0, 255, 1.0);
        third_slider_.setValue(current_third, juce::dontSendNotification);
        third_slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        third_slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        third_slider_.addListener(this);
        third_slider_.addKeyListener(this);
        addAndMakeVisible(third_slider_);

        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(value_label_);

        apply_button_.setButtonText("Apply");
        apply_button_.onClick = [this]() { apply(); };
        addAndMakeVisible(apply_button_);

        cancel_button_.setButtonText("Cancel");
        cancel_button_.onClick = [this]() {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
        };
        addAndMakeVisible(cancel_button_);

        update_value_label();
        setSize(360, 180);
        first_slider_.grabKeyboardFocus();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(20);
        title_label_.setBounds(top.removeFromLeft(150));
        value_label_.setBounds(top);
        area.removeFromTop(8);
        auto first_row = area.removeFromTop(24);
        first_value_label_.setBounds(first_row.removeFromLeft(44));
        first_slider_.setBounds(first_row);
        area.removeFromTop(8);
        auto second_row = area.removeFromTop(24);
        second_value_label_.setBounds(second_row.removeFromLeft(44));
        second_slider_.setBounds(second_row);
        area.removeFromTop(8);
        auto third_row = area.removeFromTop(24);
        third_value_label_.setBounds(third_row.removeFromLeft(44));
        third_slider_.setBounds(third_row);
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(24);
        apply_button_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        cancel_button_.setBounds(buttons.removeFromLeft(70));
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::returnKey) {
            apply();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    void sliderValueChanged(juce::Slider*) override {
        update_value_label();
    }

    void update_value_label() {
        value_label_.setText(
            juce::String(static_cast<int>(first_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(second_slider_.getValue())) + "/" +
            juce::String(static_cast<int>(third_slider_.getValue())),
            juce::dontSendNotification);
    }

    void apply() {
        if (on_apply_) {
            on_apply_(
                static_cast<int32_t>(std::lround(first_slider_.getValue())),
                static_cast<int32_t>(std::lround(second_slider_.getValue())),
                static_cast<int32_t>(std::lround(third_slider_.getValue())));
        }
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    std::function<void(int32_t, int32_t, int32_t)> on_apply_;
    juce::Label title_label_;
    juce::Label first_value_label_;
    juce::Label second_value_label_;
    juce::Label third_value_label_;
    juce::Label value_label_;
    juce::Slider first_slider_;
    juce::Slider second_slider_;
    juce::Slider third_slider_;
    juce::TextButton apply_button_;
    juce::TextButton cancel_button_;
};

class FFTOpcodeChoicePickerContent final : public juce::Component,
                                           private juce::TextEditor::Listener,
                                           private juce::KeyListener,
                                           private juce::ListBoxModel {
public:
    FFTOpcodeChoicePickerContent(
        const juce::String& title,
        int current_opcode,
        std::vector<FFTOpcodeChoiceEntry> choices,
        std::function<void(int)> on_pick)
        : current_opcode_(current_opcode),
          choices_(std::move(choices)),
          on_pick_(std::move(on_pick)) {
        title_label_.setText(title, juce::dontSendNotification);
        title_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(title_label_);

        search_editor_.setTextToShowWhenEmpty("Search opcode", juce::Colours::grey);
        search_editor_.addListener(this);
        search_editor_.addKeyListener(this);
        addAndMakeVisible(search_editor_);

        hint_label_.setText("Enter or double-click to apply", juce::dontSendNotification);
        hint_label_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(hint_label_);

        results_list_.setModel(this);
        results_list_.setOutlineThickness(0);
        results_list_.addKeyListener(this);
        addAndMakeVisible(results_list_);

        refresh_results();
        if (!filtered_indices_.empty()) {
            int selected_row = 0;
            for (size_t i = 0; i < filtered_indices_.size(); ++i) {
                if (choices_[filtered_indices_[i]].opcode == current_opcode_) {
                    selected_row = static_cast<int>(i);
                    break;
                }
            }
            results_list_.selectRow(selected_row);
        }

        setSize(280, 210);
        search_editor_.grabKeyboardFocus();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(10);
        title_label_.setBounds(area.removeFromTop(20));
        area.removeFromTop(6);
        search_editor_.setBounds(area.removeFromTop(24));
        area.removeFromTop(6);
        hint_label_.setBounds(area.removeFromBottom(18));
        area.removeFromBottom(6);
        results_list_.setBounds(area);
    }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
            if (filtered_indices_.empty()) {
                return true;
            }
            const int direction = (key == juce::KeyPress::upKey) ? -1 : 1;
            int row = results_list_.getSelectedRow();
            if (row < 0) {
                row = 0;
            } else {
                row = juce::jlimit(0, static_cast<int>(filtered_indices_.size()) - 1, row + direction);
            }
            results_list_.selectRow(row);
            results_list_.scrollToEnsureRowIsOnscreen(row);
            return true;
        }
        if (key == juce::KeyPress::returnKey) {
            apply_row(results_list_.getSelectedRow());
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
                box->dismiss();
            }
            return true;
        }
        return false;
    }

    int getNumRows() override {
        return static_cast<int>(filtered_indices_.size());
    }

    void paintListBoxItem(int row_number, juce::Graphics& g, int width, int height, bool row_is_selected) override {
        if (row_number < 0 || static_cast<size_t>(row_number) >= filtered_indices_.size()) {
            return;
        }
        const auto& choice = choices_[filtered_indices_[static_cast<size_t>(row_number)]];
        if (row_is_selected) {
            g.fillAll(juce::Colour::fromRGB(67, 92, 132));
        } else if (choice.opcode == current_opcode_) {
            g.fillAll(juce::Colour::fromRGB(52, 60, 74));
        }

        g.setColour(juce::Colours::white.withAlpha(0.96f));
        juce::String line = choice.name + "  |  0x" + juce::String::toHexString(choice.opcode).paddedLeft('0', 2).toUpperCase();
        if (choice.description.isNotEmpty()) {
            line << "  |  " << choice.description;
        }
        g.drawText(line, 6, 0, width - 12, height, juce::Justification::centredLeft, false);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent&) override {
        results_list_.selectRow(row);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        apply_row(row);
    }

    void apply_row(int row) {
        if (row < 0 || static_cast<size_t>(row) >= filtered_indices_.size() || !on_pick_) {
            return;
        }
        on_pick_(choices_[filtered_indices_[static_cast<size_t>(row)]].opcode);
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    void textEditorTextChanged(juce::TextEditor&) override {
        refresh_results();
    }

    void textEditorReturnKeyPressed(juce::TextEditor&) override {
        apply_row(results_list_.getSelectedRow() >= 0 ? results_list_.getSelectedRow() : 0);
    }

    void textEditorEscapeKeyPressed(juce::TextEditor&) override {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) {
            box->dismiss();
        }
    }

    void refresh_results() {
        filtered_indices_.clear();
        const juce::String query = search_editor_.getText().trim().toLowerCase();
        std::vector<std::pair<int, size_t>> ranked_indices;
        for (size_t i = 0; i < choices_.size(); ++i) {
            const auto& choice = choices_[i];
            const juce::String priority_haystack =
                (choice.name + " " + juce::String(choice.opcode) + " " +
                 juce::String::toHexString(choice.opcode).toUpperCase()).toLowerCase();
            const juce::String description_haystack = choice.description.toLowerCase();
            if (query.isEmpty()) {
                ranked_indices.push_back({0, i});
            } else if (priority_haystack.contains(query)) {
                ranked_indices.push_back({0, i});
            } else if (description_haystack.contains(query)) {
                ranked_indices.push_back({1, i});
            }
        }
        std::stable_sort(
            ranked_indices.begin(),
            ranked_indices.end(),
            [this](const auto& lhs, const auto& rhs) {
                if (lhs.first != rhs.first) {
                    return lhs.first < rhs.first;
                }
                return choices_[lhs.second].name < choices_[rhs.second].name;
            });
        for (const auto& ranked_index : ranked_indices) {
            filtered_indices_.push_back(ranked_index.second);
        }
        results_list_.updateContent();
        if (!filtered_indices_.empty()) {
            results_list_.selectRow(0);
            results_list_.scrollToEnsureRowIsOnscreen(0);
        }
        repaint();
    }

    int current_opcode_ = -1;
    std::vector<FFTOpcodeChoiceEntry> choices_;
    std::vector<size_t> filtered_indices_;
    std::function<void(int)> on_pick_;
    juce::Label title_label_;
    juce::TextEditor search_editor_;
    juce::Label hint_label_;
    juce::ListBox results_list_;
};

int32_t parse_label_value(const juce::String& label_text, const juce::String& prefix) {
    if (!label_text.startsWithIgnoreCase(prefix + " ")) {
        return 0;
    }
    return label_text.fromFirstOccurrenceOf(prefix + " ", false, false).getIntValue();
}

int32_t command_param_or_label_value(
    const FFTSmdLaneCommandBlock& command,
    size_t param_index,
    int32_t fallback_value,
    const juce::String& prefix
) {
    if (param_index < command.opcode_params.size()) {
        return command.opcode_params[param_index];
    }
    const int32_t parsed = parse_label_value(juce::String(command.label), prefix);
    return parsed != 0 || fallback_value == 0 ? parsed : fallback_value;
}

bool parse_label_pair(
    const juce::String& label_text,
    const juce::String& prefix,
    int32_t& first_value,
    int32_t& second_value
) {
    if (!label_text.startsWithIgnoreCase(prefix + " ")) {
        return false;
    }
    const juce::String values = label_text.fromFirstOccurrenceOf(prefix + " ", false, false);
    const int first_slash = values.indexOfChar('/');
    if (first_slash <= 0) {
        return false;
    }
    first_value = values.substring(0, first_slash).getIntValue();
    second_value = values.substring(first_slash + 1).getIntValue();
    return true;
}

void command_param_or_label_pair(
    const FFTSmdLaneCommandBlock& command,
    int32_t fallback_first,
    int32_t fallback_second,
    const juce::String& prefix,
    int32_t& first_value,
    int32_t& second_value
) {
    if (command.opcode_params.size() >= 2) {
        first_value = command.opcode_params[0];
        second_value = command.opcode_params[1];
        return;
    }
    first_value = fallback_first;
    second_value = fallback_second;
    parse_label_pair(juce::String(command.label), prefix, first_value, second_value);
}

bool parse_label_triple(
    const juce::String& label_text,
    const juce::String& prefix,
    int32_t& first_value,
    int32_t& second_value,
    int32_t& third_value
) {
    if (!label_text.startsWithIgnoreCase(prefix + " ")) {
        return false;
    }
    const juce::String values = label_text.fromFirstOccurrenceOf(prefix + " ", false, false);
    const int first_slash = values.indexOfChar('/');
    const int second_slash = first_slash >= 0 ? values.indexOfChar(first_slash + 1, '/') : -1;
    if (first_slash <= 0 || second_slash <= first_slash) {
        return false;
    }
    first_value = values.substring(0, first_slash).getIntValue();
    second_value = values.substring(first_slash + 1, second_slash).getIntValue();
    third_value = values.substring(second_slash + 1).getIntValue();
    return true;
}

void command_param_or_label_triple(
    const FFTSmdLaneCommandBlock& command,
    int32_t fallback_first,
    int32_t fallback_second,
    int32_t fallback_third,
    const juce::String& prefix,
    int32_t& first_value,
    int32_t& second_value,
    int32_t& third_value
) {
    if (command.opcode_params.size() >= 3) {
        first_value = command.opcode_params[0];
        second_value = command.opcode_params[1];
        third_value = command.opcode_params[2];
        return;
    }
    first_value = fallback_first;
    second_value = fallback_second;
    third_value = fallback_third;
    parse_label_triple(juce::String(command.label), prefix, first_value, second_value, third_value);
}

std::vector<FFTOpcodeChoiceEntry> insertion_opcode_choices() {
    return {
        {"Inst", 0xAC, "Change instrument"},
        {"Dyn", 0xE0, "Set dynamics"},
        {"Pan", 0xE8, "Set pan"},
        {"Tempo", 0xA0, "Set tempo"},
        {"TimeSig", 0x97, "Set time signature"},
        {"Oct", 0x94, "Set octave"},
        {"RaiseO", 0x95, "Raise octave"},
        {"LowerO", 0x96, "Lower octave"},
        {"RptStart", 0x98, "Repeat start"},
        {"RptEnd", 0x99, "Repeat end"},
        {"RptBreak", 0x9A, "Repeat break"},
        {"Perc+", 0xAE, "Enable percussion mode"},
        {"Perc-", 0xAF, "Disable percussion mode"},
        {"Slur+", 0xB0, "Enable slur"},
        {"Slur-", 0xB1, "Disable slur"},
        {"Rev+", 0xBA, "Enable reverb"},
        {"Rev-", 0xBB, "Disable reverb"},
        {"ADSR Rst", 0xC0, "Reset ADSR envelope"},
        {"Atk", 0xC2, "ADSR attack"},
        {"SusRt", 0xC4, "ADSR sustain rate"},
        {"Rel", 0xC5, "ADSR release"},
        {"Slide", 0xC6, "ADSR slide target"},
        {"Dec/Sus", 0xC7, "ADSR decay and sustain"},
        {"Dec", 0xC9, "ADSR decay"},
        {"SusLv", 0xCA, "ADSR sustain level"},
        {"Bend", 0xD0, "Pitch bend"},
        {"Bend+", 0xD1, "Add pitch bend"},
        {"Cond", 0xD2, "Conditional sequence flag"},
        {"Port", 0xD4, "Portamento"},
        {"Detune", 0xD6, "Detune"},
        {"LFO", 0xD7, "Pitch LFO depth"},
        {"LFOlen", 0xD8, "Pitch LFO setup"},
        {"LFOcmd", 0xD9, "Pitch LFO command"},
        {"FlgFE+", 0xDA, "Set FE flag"},
        {"FlgFE-", 0xDB, "Clear FE flag"},
        {"Dyn+", 0xE1, "Add dynamics"},
        {"Expr", 0xE2, "Expression"},
        {"VolLFO", 0xE3, "Volume LFO depth"},
        {"VolLFOlen", 0xE4, "Volume LFO setup"},
        {"VolLFOcmd", 0xE5, "Volume LFO command"},
        {"FlgE6", 0xE6, "Set E6 flag"},
        {"Pan?", 0xE9, "Unknown pan opcode"},
        {"PanSl", 0xEA, "Pan slide"},
        {"PanLFO", 0xEB, "Pan LFO depth"},
        {"PanLFOLn", 0xEC, "Pan LFO setup"},
        {"PanLFO3", 0xED, "Pan LFO command"},
        {"Bank", 0xFE, "Select bank"},
    };
}

std::vector<int32_t> default_params_for_opcode(int opcode) {
    switch (opcode) {
    case 0x94: return {4};
    case 0x97: return {4, 4};
    case 0x98: return {2};
    case 0xA0: return {102};
    case 0xA2: return {0, 0};
    case 0xAC: return {0};
    case 0xC7: return {0, 0};
    case 0xD4: return {0, 0};
    case 0xE0: return {96};
    case 0xE8: return {64};
    default: {
        const int32_t param_count = smd_opcode_param_count(static_cast<uint8_t>(opcode));
        if (param_count <= 0) {
            return {};
        }
        return std::vector<int32_t>(static_cast<size_t>(param_count), 0);
    }
    }
}

std::optional<FFTSmdLaneCommandBlock> find_source_command(
    const FFTSmdSongPresentation& presentation,
    int track_index,
    int source_event_index
) {
    for (const auto& track : presentation.tracks) {
        if (track.track_index != track_index) {
            continue;
        }
        for (const auto& command : track.commands) {
            if (command.source_event_index == source_event_index) {
                return command;
            }
        }
        break;
    }
    return std::nullopt;
}

std::optional<FFTSmdLaneCommandBlock> find_authored_command(
    const FFTSmdSongPresentation& presentation,
    int track_index,
    int authored_opcode_index
) {
    for (const auto& track : presentation.tracks) {
        if (track.track_index != track_index) {
            continue;
        }
        for (const auto& command : track.commands) {
            if (command.authored_opcode_index == authored_opcode_index) {
                return command;
            }
        }
        break;
    }
    return std::nullopt;
}

int32_t command_identity(const FFTSmdLaneCommandBlock& command) {
    return command.authored_opcode_index >= 0 ? command.authored_opcode_index : command.source_event_index;
}

}  // namespace

FFTJuceAudioProcessorEditor::FFTJuceAudioProcessorEditor(FFTJuceAudioProcessor& processor)
    : juce::AudioProcessorEditor(&processor),
      processor_(processor) {
    append_unwound_insert_debug_line("=== EDITOR_OPEN ===");
    setResizable(true, true);
    setResizeLimits(1040, 900, 2200, 1800);

    unwind_mode_button_.setClickingTogglesState(true);
    unwind_mode_button_.setToggleState(false, juce::dontSendNotification);
    unwind_mode_button_.addListener(this);
    addAndMakeVisible(unwind_mode_button_);
    group_button_.addListener(this);
    addAndMakeVisible(group_button_);
    ungroup_button_.addListener(this);
    addAndMakeVisible(ungroup_button_);
    track_transposition_button_.addListener(this);
    track_transposition_button_.setTooltip("Track transposition in semitones (−36 to +36). Applied on top of authored octave opcodes.");
    addAndMakeVisible(track_transposition_button_);
    play_button_.addListener(this);
    addAndMakeVisible(play_button_);
    stop_button_.addListener(this);
    addAndMakeVisible(stop_button_);
    clear_selection_button_.addListener(this);
    addAndMakeVisible(clear_selection_button_);
    debug_button_.addListener(this);
    addAndMakeVisible(debug_button_);

    waveset_label_.setText("WAVESET", juce::dontSendNotification);
    addAndMakeVisible(waveset_label_);

    waveset_path_label_.setJustificationType(juce::Justification::centredLeft);
    waveset_path_label_.setColour(juce::Label::backgroundColourId, juce::Colours::white.withAlpha(0.08f));
    waveset_path_label_.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.18f));
    addAndMakeVisible(waveset_path_label_);

    waveset_status_label_.setJustificationType(juce::Justification::centredLeft);
    waveset_status_label_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(waveset_status_label_);

    waveset_button_.addListener(this);
    addAndMakeVisible(waveset_button_);

    new_song_button_.addListener(this);
    addAndMakeVisible(new_song_button_);

    midi_import_button_.addListener(this);
    addAndMakeVisible(midi_import_button_);

    export_smd_button_.addListener(this);
    addAndMakeVisible(export_smd_button_);

    target_bytes_label_.setText("bytes:", juce::dontSendNotification);
    target_bytes_label_.setJustificationType(juce::Justification::centredRight);
    target_bytes_label_.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(target_bytes_label_);

    target_bytes_editor_.setMultiLine(false);
    target_bytes_editor_.setReturnKeyStartsNewLine(false);
    target_bytes_editor_.setInputRestrictions(8, "0123456789");
    target_bytes_editor_.setTextToShowWhenEmpty("0=auto", juce::Colours::grey);
    target_bytes_editor_.setTooltip(
        "Trim the song so the encoded SMD fits this many bytes. Empty / 0 = "
        "trim to the engine cap (20480 B / 10 sectors). Any value above the "
        "engine cap is silently clamped: the FFT engine won't load larger "
        "SMDs (binary-searched 2026-05-03; 10 sectors plays, 13 sectors "
        "silences). Set to 2048 to fit MUSIC_41's vanilla 1-sector slot "
        "without the patcher's relocation step.");
    target_bytes_editor_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::white);
    target_bytes_editor_.setColour(juce::TextEditor::textColourId, juce::Colours::black);
    target_bytes_editor_.setColour(juce::TextEditor::outlineColourId, juce::Colours::grey);
    addAndMakeVisible(target_bytes_editor_);

    match_lab_button_.addListener(this);
    addAndMakeVisible(match_lab_button_);

    smd_label_.setText("SMD", juce::dontSendNotification);
    addAndMakeVisible(smd_label_);

    smd_path_label_.setJustificationType(juce::Justification::centredLeft);
    smd_path_label_.setColour(juce::Label::backgroundColourId, juce::Colours::white.withAlpha(0.08f));
    smd_path_label_.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.18f));
    addAndMakeVisible(smd_path_label_);

    smd_status_label_.setJustificationType(juce::Justification::centredLeft);
    smd_status_label_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(smd_status_label_);

    smd_button_.addListener(this);
    addAndMakeVisible(smd_button_);

    status_label_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(status_label_);

    playback_label_.setJustificationType(juce::Justification::centredLeft);
    playback_label_.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
    addAndMakeVisible(playback_label_);

    inspector_title_label_.setText("SMD Inspector", juce::dontSendNotification);
    inspector_title_label_.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(inspector_title_label_);

    track_selector_label_.setText("Track", juce::dontSendNotification);
    addAndMakeVisible(track_selector_label_);

    track_selector_.onChange = [this]() { refresh_inspector(); };
    addAndMakeVisible(track_selector_);

    addAndMakeVisible(lane_header_view_);
    lane_viewport_.setViewedComponent(&lane_view_, false);
    lane_viewport_.setScrollBarsShown(true, true);
    lane_viewport_.on_visible_area_changed = [this](juce::Rectangle<int>) {
        sync_lane_scroll_state();
    };
    addAndMakeVisible(lane_viewport_);
    addAndMakeVisible(detail_view_);

    const auto zoom_handler = [this](float anchor_tick, float wheel_delta) {
        apply_lane_zoom(anchor_tick, wheel_delta);
    };
    const auto pan_handler = [this](int delta_x, int delta_y) {
        apply_lane_pan(delta_x, delta_y);
    };
    lane_header_view_.set_zoom_callback(zoom_handler);
    lane_view_.set_zoom_callback(zoom_handler);
    detail_view_.set_zoom_callback(zoom_handler);
    lane_header_view_.set_pan_callback(pan_handler);
    lane_view_.set_pan_callback(pan_handler);
    detail_view_.set_pan_callback(pan_handler);
    lane_view_.set_command_selected_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_selected(track_index, command);
    });
    lane_view_.set_command_selection_toggled_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_selection_toggled(track_index, command);
    });
    lane_view_.set_command_secondary_click_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_toggle_disabled(track_index, command);
    });
    lane_view_.set_command_delete_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_delete(track_index, command);
    });
    lane_view_.set_command_move_callback([this](int track_index, FFTSmdLaneCommandBlock command, FFTSmdLaneInsertAnchor anchor) {
        handle_detail_command_move(track_index, command, anchor);
    });
    lane_view_.set_note_delete_callback([this](int track_index, int source_event_index) {
        handle_time_lane_note_delete(track_index, source_event_index);
    });
    lane_view_.set_note_fermata_delete_callback([this](int track_index, int source_event_index) {
        handle_time_lane_note_fermata(track_index, source_event_index, 0);
    });
    lane_view_.set_note_match_lab_callback([this](int track_index, int source_event_index) {
        open_match_lab_for_note(track_index, source_event_index);
    });
    lane_view_.set_insert_anchor_callback([this](int track_index, FFTSmdLaneInsertAnchor anchor) {
        handle_insert_anchor_selected(track_index, anchor);
    });
    lane_header_view_.set_time_selection_callback([this](int32_t start_tick, int32_t end_tick) {
        handle_time_selection_changed(start_tick, end_tick);
    });
    detail_view_.set_command_selected_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_selected(track_index, command);
    });
    detail_view_.set_command_selection_toggled_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_selection_toggled(track_index, command);
    });
    detail_view_.set_command_secondary_click_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_toggle_disabled(track_index, command);
    });
    detail_view_.set_command_delete_callback([this](int track_index, FFTSmdLaneCommandBlock command) {
        handle_detail_command_delete(track_index, command);
    });
    detail_view_.set_command_move_callback([this](int track_index, FFTSmdLaneCommandBlock command, FFTSmdLaneInsertAnchor anchor) {
        handle_detail_command_move(track_index, command, anchor);
    });
    detail_view_.set_note_delete_callback([this](int track_index, int source_event_index) {
        handle_time_lane_note_delete(track_index, source_event_index);
    });
    detail_view_.set_note_fermata_delete_callback([this](int track_index, int source_event_index) {
        handle_time_lane_note_fermata(track_index, source_event_index, 0);
    });
    detail_view_.set_note_match_lab_callback([this](int track_index, int source_event_index) {
        open_match_lab_for_note(track_index, source_event_index);
    });
    detail_view_.set_note_resize_callback([this](
        int track_index,
        int source_event_index,
        int32_t start_tick,
        int32_t base_duration_ticks,
        int32_t extension_ticks
    ) {
        handle_time_lane_note_resize(
            track_index,
            source_event_index,
            start_tick,
            base_duration_ticks,
            extension_ticks);
    });
    detail_view_.set_note_insert_callback([this](
        int track_index,
        int source_event_index,
        int note_relative_key,
        int32_t absolute_start_tick,
        int32_t covered_start_tick,
        int32_t duration_ticks
    ) {
        handle_time_lane_note_insert(
            track_index,
            source_event_index,
            note_relative_key,
            absolute_start_tick,
            covered_start_tick,
            duration_ticks);
    });
    detail_view_.set_note_fermata_callback([this](
        int track_index,
        int source_event_index,
        int32_t extension_ticks
    ) {
        handle_time_lane_note_fermata(
            track_index,
            source_event_index,
            extension_ticks);
    });
    detail_view_.set_rest_selected_callback([this](int track_index, int source_event_index, int authored_span_index) {
        handle_time_lane_rest_selected(track_index, source_event_index, authored_span_index);
    });
    detail_view_.set_rest_ripple_resize_callback(
        [this](int track_index, int source_event_index, int authored_span_index, int32_t delta_ticks) {
            handle_time_lane_rest_ripple_resize(track_index, source_event_index, authored_span_index, delta_ticks);
        });
    detail_view_.set_insert_anchor_callback([this](int track_index, FFTSmdLaneInsertAnchor anchor) {
        handle_insert_anchor_selected(track_index, anchor);
    });
    detail_view_.set_note_snap_mode_index(note_snap_mode_index_);
    detail_view_.set_note_snap_adjust_callback([this](int delta) {
        adjust_note_snap_mode(delta);
    });
    detail_view_.set_filter_changed_callback([this](const FFTOpcodeFilterState& state) {
        lane_view_.set_command_filter_state(state);
    });
    lane_header_view_.set_presentation_mode(presentation_mode_);
    lane_view_.set_presentation_mode(presentation_mode_);
    lane_header_view_.set_pixels_per_tick(pixels_per_tick_);
    lane_view_.set_pixels_per_tick(pixels_per_tick_);
    detail_view_.set_pixels_per_tick(pixels_per_tick_);
    lane_view_.set_note_snap_mode_index(note_snap_mode_index_);
    lane_view_.set_command_filter_state(detail_view_.filter_state());
    sync_lane_scroll_state();

    lane_view_.set_track_selected_callback([this](int track_index, bool additive) {
        processor_.set_selected_track_id(track_index);
        if (!additive) {
            selected_source_track_indices_.clear();
        }
        if (additive) {
            const auto existing = std::find(selected_source_track_indices_.begin(), selected_source_track_indices_.end(), track_index);
            if (existing == selected_source_track_indices_.end()) {
                selected_source_track_indices_.push_back(track_index);
            } else {
                selected_source_track_indices_.erase(existing);
            }
        } else {
            selected_source_track_indices_ = {track_index};
        }
        std::sort(selected_source_track_indices_.begin(), selected_source_track_indices_.end());
        lane_view_.set_multi_selected_tracks(selected_source_track_indices_);
        track_selector_.setSelectedItemIndex(track_index, juce::sendNotificationSync);
        refresh_group_buttons();
    });
    lane_view_.set_track_mute_callback([this](int track_index) {
        processor_.set_track_muted(track_index, !processor_.track_muted(track_index));
        refresh_lane_presentation();
        refresh_inspector();
    });
    lane_view_.set_track_solo_callback([this](int track_index) {
        processor_.set_track_soloed(track_index, !processor_.track_soloed(track_index));
        refresh_lane_presentation();
        refresh_inspector();
    });

    configure_text_editor(metadata_editor_);
    configure_text_editor(track_summary_editor_);
    configure_text_editor(event_list_editor_);
    metadata_editor_.setVisible(false);
    track_summary_editor_.setVisible(false);
    event_list_editor_.setVisible(false);
    waveset_label_.setVisible(false);
    waveset_path_label_.setVisible(false);
    waveset_status_label_.setVisible(false);
    smd_label_.setVisible(false);
    smd_path_label_.setVisible(false);
    smd_status_label_.setVisible(false);
    track_selector_label_.setVisible(false);
    track_selector_.setVisible(false);

    refresh_labels();
    refresh_inspector();
    refresh_group_buttons();
    startTimerHz(8);
    setSize(1180, 720);
}

FFTJuceAudioProcessorEditor::~FFTJuceAudioProcessorEditor() {
    stopTimer();
    unwind_mode_button_.removeListener(this);
    group_button_.removeListener(this);
    ungroup_button_.removeListener(this);
    track_transposition_button_.removeListener(this);
    play_button_.removeListener(this);
    stop_button_.removeListener(this);
    clear_selection_button_.removeListener(this);
    debug_button_.removeListener(this);
    waveset_button_.removeListener(this);
    new_song_button_.removeListener(this);
    midi_import_button_.removeListener(this);
    export_smd_button_.removeListener(this);
    match_lab_button_.removeListener(this);
    smd_button_.removeListener(this);
}

void FFTJuceAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour::fromRGB(26, 28, 33));
    g.setColour(juce::Colour::fromRGB(54, 57, 66));
    g.drawRect(getLocalBounds(), 1);
}

void FFTJuceAudioProcessorEditor::resized() {
    constexpr int kDetailHeight = 336;
    constexpr int kStickyHeaderHeight = 42;
    constexpr int kHeaderWidth = 124;

    auto bounds = getLocalBounds().reduced(16);
    auto status_bar = bounds.removeFromBottom(20);
    auto transport_area = status_bar.removeFromRight(212);
    debug_button_.setBounds(transport_area.removeFromRight(42));
    transport_area.removeFromRight(4);
    clear_selection_button_.setBounds(transport_area.removeFromRight(42));
    transport_area.removeFromRight(4);
    stop_button_.setBounds(transport_area.removeFromRight(56));
    transport_area.removeFromRight(4);
    play_button_.setBounds(transport_area.removeFromRight(56));
    status_label_.setBounds(status_bar.removeFromLeft((status_bar.getWidth() * 2) / 3));
    playback_label_.setBounds(status_bar);

    lane_header_view_.setBounds(bounds.removeFromTop(kStickyHeaderHeight));
    auto button_area = juce::Rectangle<int>(bounds.getX(), lane_header_view_.getY(), 294, kStickyHeaderHeight).reduced(4, 4);
    auto left_column = button_area.removeFromLeft(54);
    waveset_button_.setBounds(left_column.removeFromTop(16));
    left_column.removeFromTop(4);
    smd_button_.setBounds(left_column.removeFromTop(16));
    button_area.removeFromLeft(4);
    auto middle_column = button_area.removeFromLeft(54);
    new_song_button_.setBounds(middle_column.removeFromTop(16));
    middle_column.removeFromTop(4);
    midi_import_button_.setBounds(middle_column.removeFromTop(16));
    button_area.removeFromLeft(4);
    auto right_column = button_area.removeFromLeft(54);
    export_smd_button_.setBounds(right_column.removeFromTop(16));
    right_column.removeFromTop(4);
    auto target_row = right_column.removeFromTop(16);
    target_bytes_label_.setBounds(target_row.removeFromLeft(22));
    target_bytes_editor_.setBounds(target_row);
    target_bytes_label_.toFront(false);
    target_bytes_editor_.toFront(false);
    button_area.removeFromLeft(4);
    auto far_right_column = button_area.removeFromLeft(54);
    match_lab_button_.setBounds(far_right_column.removeFromTop(16));
    far_right_column.removeFromTop(4);
    unwind_mode_button_.setBounds(far_right_column.removeFromTop(16));
    waveset_button_.toFront(false);
    smd_button_.toFront(false);
    new_song_button_.toFront(false);
    midi_import_button_.toFront(false);
    export_smd_button_.toFront(false);
    unwind_mode_button_.toFront(false);
    match_lab_button_.toFront(false);
    inspector_title_label_.setBounds(0, 0, 0, 0);

    bounds.removeFromTop(8);

    auto lane_bounds = bounds.removeFromTop(bounds.getHeight());
    detail_view_.setBounds(lane_bounds.removeFromTop(std::min(kDetailHeight, lane_bounds.getHeight())));
    auto detail_button_area = juce::Rectangle<int>(
        detail_view_.getX() + 6,
        detail_view_.getY() + 6,
        56,
        62);
    group_button_.setBounds(detail_button_area.removeFromTop(18));
    detail_button_area.removeFromTop(4);
    ungroup_button_.setBounds(detail_button_area.removeFromTop(18));
    detail_button_area.removeFromTop(4);
    track_transposition_button_.setBounds(detail_button_area.removeFromTop(18));
    group_button_.toFront(false);
    ungroup_button_.toFront(false);
    track_transposition_button_.toFront(false);
    lane_bounds.removeFromTop(6);
    lane_bounds.removeFromTop(6);
    lane_viewport_.setBounds(lane_bounds);
    sync_lane_scroll_state();
}

void FFTJuceAudioProcessorEditor::buttonClicked(juce::Button* button) {
    if (button == &unwind_mode_button_) {
        presentation_mode_ = unwind_mode_button_.getToggleState()
            ? FFTSmdPresentationMode::playback
            : FFTSmdPresentationMode::source;
        lane_header_view_.set_presentation_mode(presentation_mode_);
        lane_view_.set_presentation_mode(presentation_mode_);
        refresh_inspector();
        return;
    }
    if (button == &waveset_button_) {
        choose_file(true);
        return;
    }
    if (button == &new_song_button_) {
        choose_new_song();
        return;
    }
    if (button == &midi_import_button_) {
        choose_import_midi();
        return;
    }
    if (button == &match_lab_button_) {
        show_match_lab();
        return;
    }
    if (button == &group_button_) {
        if (processor_.convert_tracks_to_poly_track(selected_source_track_indices_)) {
            if (!selected_source_track_indices_.empty()) {
                selected_source_track_indices_ = {selected_source_track_indices_.front()};
            }
            lane_view_.set_multi_selected_tracks(selected_source_track_indices_);
            refresh_lane_presentation();
            refresh_labels();
            refresh_inspector();
        } else {
            refresh_labels();
        }
        refresh_group_buttons();
        return;
    }
    if (button == &ungroup_button_) {
        if (processor_.ungroup_poly_track(processor_.selected_track_id())) {
            selected_source_track_indices_ = {processor_.selected_track_id()};
            lane_view_.set_multi_selected_tracks(selected_source_track_indices_);
            refresh_lane_presentation();
            refresh_labels();
            refresh_inspector();
        } else {
            refresh_labels();
        }
        refresh_group_buttons();
        return;
    }
    if (button == &track_transposition_button_) {
        const int track_index = processor_.selected_track_id();
        if (track_index < 0) {
            return;
        }
        const int current = processor_.track_transposition(track_index);
        auto picker = std::make_unique<FFTValuePickerContent>(
            "Transposition",
            -36,
            36,
            current,
            [this, track_index](int32_t value) {
                if (processor_.set_track_transposition(track_index, value)) {
                    refresh_lane_presentation();
                    refresh_labels();
                    refresh_inspector();
                    refresh_group_buttons();
                }
            });
        juce::CallOutBox::launchAsynchronously(
            std::move(picker),
            track_transposition_button_.getBoundsInParent(),
            this);
        return;
    }
    if (button == &export_smd_button_) {
        choose_export_smd();
        return;
    }
    if (button == &play_button_) {
        start_local_playback_from_selection();
        return;
    }
    if (button == &stop_button_) {
        processor_.stop_local_playback();
        refresh_labels();
        return;
    }
    if (button == &clear_selection_button_) {
        selected_time_start_tick_ = -1;
        selected_time_end_tick_ = -1;
        sync_selected_time_to_views();
        refresh_labels();
        return;
    }
    if (button == &smd_button_) {
        choose_file(false);
    }
    if (button == &debug_button_) {
        auto* window = new juce::AlertWindow("FFT Plugin Diagnostics", {}, juce::MessageBoxIconType::NoIcon);
        auto* editor = new juce::TextEditor();
        editor->setMultiLine(true);
        editor->setReadOnly(true);
        editor->setScrollbarsShown(true);
        editor->setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        editor->setText(processor_.diagnostic_text());
        editor->setSize(560, 320);
        window->addCustomComponent(editor);
        window->addButton("Close", 0);
        window->setSize(580, 420);
        window->enterModalState(true, juce::ModalCallbackFunction::create([window](int) {
            window->exitModalState(0);
            delete window;
        }), true);
    }
}

void FFTJuceAudioProcessorEditor::refresh_labels() {
    static constexpr const char* kSnapModeNames[] = {
        "Bar",
        "Beat",
        "1/2",
        "1/4",
        "1/8",
        "1/12",
        "1/16",
        "1/24",
        "Tick",
    };
    const auto compact_name = [](const juce::String& full_path) {
        const juce::File file(full_path);
        return file.existsAsFile() ? file.getFileName() : (full_path.isNotEmpty() ? full_path : juce::String("(none)"));
    };
    const juce::String waveset_name = compact_name(processor_.waveset_path());
    const juce::String smd_name = compact_name(processor_.music_document_path());
    waveset_path_label_.setText(waveset_name, juce::dontSendNotification);
    smd_path_label_.setText(smd_name, juce::dontSendNotification);
    waveset_path_label_.setTooltip(processor_.waveset_path());
    smd_path_label_.setTooltip(processor_.music_document_path());
    waveset_status_label_.setText(processor_.waveset_status_text(), juce::dontSendNotification);
    smd_status_label_.setText(processor_.smd_status_text(), juce::dontSendNotification);
    const int snap_mode_index = juce::jlimit(0, 8, note_snap_mode_index_);
    const juce::String time_selection_text = has_time_selection()
        ? " | Sel " + juce::String(selected_time_start_tick_) + "-" + juce::String(selected_time_end_tick_)
        : juce::String();
    const juce::String loop_boundary_warning =
        (presentation_mode_ == FFTSmdPresentationMode::playback && unwound_selection_crosses_loop_boundary())
        ? " | crosses loop boundary"
        : juce::String();
    status_label_.setText(
        smd_name + " | " + waveset_name + " | Snap " + kSnapModeNames[snap_mode_index] + time_selection_text + loop_boundary_warning +
            " | " + processor_.status_text(),
        juce::dontSendNotification);
    playback_label_.setText(processor_.playback_summary(), juce::dontSendNotification);
    unwind_mode_button_.setToggleState(
        presentation_mode_ == FFTSmdPresentationMode::playback,
        juce::dontSendNotification);

    const auto set_status_colour = [](juce::Label& label) {
        const juce::String text = label.getText().toLowerCase();
        juce::Colour colour = juce::Colours::lightgrey;
        if (text.contains("loaded")) {
            colour = juce::Colours::lightgreen;
        } else if (text.contains("no ") || text.contains("failed") || text.contains("missing") || text.contains("invalid")) {
            colour = juce::Colours::salmon;
        }
        label.setColour(juce::Label::textColourId, colour);
    };

    set_status_colour(waveset_status_label_);
    set_status_colour(smd_status_label_);
}

void FFTJuceAudioProcessorEditor::refresh_lane_presentation() {
    const auto header_presentation = processor_.inspector_song_presentation(FFTSmdPresentationMode::playback);
    const auto presentation = processor_.inspector_song_presentation(presentation_mode_);
    lane_header_view_.set_presentation(header_presentation.tracks.empty() ? presentation : header_presentation);
    lane_view_.set_presentation(presentation);
    lane_view_.set_multi_selected_tracks(selected_source_track_indices_);
    lane_header_view_.set_pixels_per_tick(pixels_per_tick_);
    lane_view_.set_pixels_per_tick(pixels_per_tick_);
    detail_view_.set_presentation(presentation, processor_.selected_track_id());
    detail_view_.set_pixels_per_tick(pixels_per_tick_);
    sync_selected_commands_to_views();
    sync_selected_rest_to_views();
    sync_selected_time_to_views();
    refresh_group_buttons();
}

void FFTJuceAudioProcessorEditor::refresh_inspector() {
    const juce::String current_smd_path = processor_.music_document_path();
    const auto current_presentation = processor_.inspector_song_presentation(presentation_mode_);
    const int track_count = static_cast<int>(current_presentation.tracks.size());
    const bool model_changed =
        current_smd_path != last_inspected_smd_path_ ||
        track_count != last_inspector_track_count_ ||
        presentation_mode_ != last_presentation_mode_;
    if (model_changed) {
        refresh_lane_presentation();
        track_selector_.clear(juce::dontSendNotification);
        for (int track_index = 0; track_index < track_count; ++track_index) {
            track_selector_.addItem("Track " + juce::String(track_index), track_index + 1);
        }
        if (track_count > 0) {
            const int clamped_index = juce::jlimit(0, track_count - 1, processor_.selected_track_id());
            track_selector_.setSelectedItemIndex(clamped_index, juce::dontSendNotification);
        }
        last_inspected_smd_path_ = current_smd_path;
        last_inspector_track_count_ = track_count;
        last_presentation_mode_ = presentation_mode_;
        metadata_editor_.setText(processor_.inspector_metadata_text(), juce::dontSendNotification);
        track_summary_editor_.setText(processor_.inspector_track_summary_text(), juce::dontSendNotification);
    }

    if (track_count > 0) {
        const int selected_track = juce::jlimit(0, track_count - 1, track_selector_.getSelectedItemIndex());
        if (model_changed || selected_track != last_selected_track_index_) {
            lane_view_.set_selected_track(selected_track);
            processor_.set_selected_track_id(selected_track);
            detail_view_.set_presentation(current_presentation, selected_track);
            sync_selected_commands_to_views();
            sync_selected_rest_to_views();
            event_list_editor_.setText(processor_.inspector_track_events_text(selected_track), juce::dontSendNotification);
            last_selected_track_index_ = selected_track;
            if (selected_source_track_indices_.empty()) {
                selected_source_track_indices_ = {selected_track};
            }
            lane_view_.set_multi_selected_tracks(selected_source_track_indices_);
        }
    } else {
        if (model_changed || last_selected_track_index_ != -1) {
            event_list_editor_.setText("No loaded track events available.\n", juce::dontSendNotification);
            detail_view_.set_presentation({}, 0);
            clear_selected_command_selection();
            clear_selected_rest_selection();
            last_selected_track_index_ = -1;
        }
    }
    refresh_group_buttons();
}

void FFTJuceAudioProcessorEditor::configure_text_editor(juce::TextEditor& editor) {
    editor.setMultiLine(true);
    editor.setReadOnly(true);
    editor.setScrollbarsShown(true);
    editor.setCaretVisible(false);
    editor.setPopupMenuEnabled(true);
    editor.setFont(juce::FontOptions("Consolas", 13.0f, juce::Font::plain));
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha(0.16f));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha(0.18f));
}

void FFTJuceAudioProcessorEditor::sync_lane_scroll_state() {
    const int scroll_x = lane_viewport_.getViewPositionX();
    lane_header_view_.set_horizontal_scroll(scroll_x);
    lane_view_.set_horizontal_scroll(scroll_x);
    detail_view_.set_horizontal_scroll(scroll_x);
}

void FFTJuceAudioProcessorEditor::apply_lane_zoom(float anchor_tick, float wheel_delta) {
    if (std::abs(wheel_delta) <= 0.0f) {
        return;
    }

    const float old_pixels = pixels_per_tick_;
    const float zoom_scale = wheel_delta > 0.0f ? 1.15f : (1.0f / 1.15f);
    pixels_per_tick_ = juce::jlimit(kMinPixelsPerTick, kMaxPixelsPerTick, pixels_per_tick_ * zoom_scale);
    if (std::abs(pixels_per_tick_ - old_pixels) < 0.0001f) {
        return;
    }

    const int old_scroll = lane_viewport_.getViewPositionX();
    lane_header_view_.set_pixels_per_tick(pixels_per_tick_);
    lane_view_.set_pixels_per_tick(pixels_per_tick_);
    detail_view_.set_pixels_per_tick(pixels_per_tick_);

    const float old_anchor_x = (anchor_tick * old_pixels);
    const float new_anchor_x = (anchor_tick * pixels_per_tick_);
    const int new_scroll = std::max(0, static_cast<int>(std::round(old_scroll + (new_anchor_x - old_anchor_x))));

    lane_viewport_.setViewPosition(new_scroll, lane_viewport_.getViewPositionY());
    sync_lane_scroll_state();
}

void FFTJuceAudioProcessorEditor::apply_lane_pan(int drag_delta_x, int drag_delta_y) {
    const int new_x = std::max(0, lane_viewport_.getViewPositionX() + drag_delta_x);
    const int new_y = std::max(0, lane_viewport_.getViewPositionY() + drag_delta_y);
    lane_viewport_.setViewPosition(new_x, new_y);
    sync_lane_scroll_state();
}

void FFTJuceAudioProcessorEditor::timerCallback() {
    refresh_labels();
    refresh_inspector();
    lane_header_view_.set_global_playhead_tick(processor_.current_playback_tick());
    lane_view_.set_global_playhead_tick(processor_.current_playback_tick());
    auto source_ticks = processor_.current_source_track_ticks();
    lane_header_view_.set_source_track_cursor_ticks(source_ticks);
    lane_view_.set_source_track_cursor_ticks(std::move(source_ticks));
}

void FFTJuceAudioProcessorEditor::choose_file(bool choose_waveset) {
    const auto current_path = choose_waveset ? processor_.waveset_path() : processor_.music_document_path();
    juce::File current_file(current_path);
    juce::File start_dir = current_file.exists() ? current_file.getParentDirectory()
                                                 : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    const juce::String pattern = choose_waveset ? "*.wd;*.WD" : "*.smd;*.SMD;*.fftauth;*.FFTAUTH";
    const juce::String title = choose_waveset ? "Select WAVESET.WD" : "Select SMD or authoring document";
    active_chooser_ = std::make_unique<juce::FileChooser>(title, start_dir, pattern);
    active_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, choose_waveset](const juce::FileChooser& chooser) {
            const juce::File selected = chooser.getResult();
            if (selected.existsAsFile()) {
                const bool ok = choose_waveset ? processor_.set_waveset_path(selected.getFullPathName())
                                               : processor_.set_music_document_path(selected.getFullPathName());
                juce::ignoreUnused(ok);
                refresh_labels();
                refresh_inspector();
            }
            active_chooser_.reset();
        });
}

void FFTJuceAudioProcessorEditor::choose_new_song() {
    juce::File current_file(processor_.music_document_path());
    if (!current_file.existsAsFile()) {
        current_file = juce::File(processor_.waveset_path());
    }
    juce::File start_dir = current_file.exists()
        ? current_file.getParentDirectory()
        : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    active_chooser_ = std::make_unique<juce::FileChooser>(
        "Create new authoring document",
        start_dir.getChildFile("new_song.fftauth"),
        "*.fftauth;*.FFTAUTH");
    active_chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& chooser) {
            juce::File selected = chooser.getResult();
            if (selected != juce::File{}) {
                if (selected.getFileExtension().isEmpty()) {
                    selected = selected.withFileExtension(".fftauth");
                }
                const bool ok = processor_.create_new_music_document_path(selected.getFullPathName());
                juce::ignoreUnused(ok);
                clear_selected_command_selection();
                clear_selected_rest_selection();
                selected_time_start_tick_ = -1;
                selected_time_end_tick_ = -1;
                selected_source_track_indices_ = {1};
                refresh_labels();
                refresh_inspector();
            }
            active_chooser_.reset();
        });
}

void FFTJuceAudioProcessorEditor::choose_import_midi() {
    juce::File current_file(processor_.music_document_path());
    if (!current_file.existsAsFile()) {
        current_file = juce::File(processor_.waveset_path());
    }
    juce::File start_dir = current_file.existsAsFile()
        ? current_file.getParentDirectory()
        : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    active_chooser_ = std::make_unique<juce::FileChooser>(
        "Select MIDI file",
        start_dir,
        "*.mid;*.midi;*.MID;*.MIDI");
    active_chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser) {
            const juce::File midi_file = chooser.getResult();
            active_chooser_.reset();
            if (!midi_file.existsAsFile()) {
                return;
            }

            const juce::String stem = midi_file.getFileNameWithoutExtension();
            juce::File target = midi_file.getSiblingFile(stem + ".fftauth");
            for (int suffix = 1; target.exists() && suffix < 10000; ++suffix) {
                target = midi_file.getSiblingFile(stem + "." + juce::String(suffix) + ".fftauth");
            }

            const bool ok = processor_.import_midi_path(
                midi_file.getFullPathName(),
                target.getFullPathName());
            juce::ignoreUnused(ok);
            clear_selected_command_selection();
            clear_selected_rest_selection();
            selected_time_start_tick_ = -1;
            selected_time_end_tick_ = -1;
            selected_source_track_indices_ = {1};
            refresh_labels();
            refresh_inspector();
        });
}

void FFTJuceAudioProcessorEditor::choose_export_smd() {
    juce::File current_file(processor_.smd_path());
    if (!current_file.existsAsFile()) {
        current_file = juce::File(processor_.music_document_path());
    }
    juce::File start_dir = current_file.existsAsFile()
        ? current_file.getParentDirectory()
        : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    juce::String suggested = current_file.existsAsFile()
        ? current_file.getFileNameWithoutExtension() + ".smd"
        : "export.smd";
    active_chooser_ = std::make_unique<juce::FileChooser>(
        "Export compiled SMD",
        start_dir.getChildFile(suggested),
        "*.smd;*.SMD");
    active_chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& chooser) {
            const juce::File selected = chooser.getResult();
            if (selected != juce::File{}) {
                const auto target_text = target_bytes_editor_.getText().trim();
                size_t target_bytes = 0;
                if (target_text.isNotEmpty()) {
                    target_bytes = static_cast<size_t>(std::max(0, target_text.getIntValue()));
                }
                processor_.export_smd_path_with_report(
                    selected.getFullPathName(), target_bytes);
                // No post-export warning needed: the compile path now clamps
                // every export to the engine cap (kEngineMaxSmdBytes, 20480),
                // so over-cap output is structurally impossible.
                refresh_labels();
                refresh_inspector();
            }
            active_chooser_.reset();
        });
}

void FFTJuceAudioProcessorEditor::show_match_lab(const std::optional<FFTMatchLabSeed>& seed) {
    auto component = std::make_unique<FFTMatchLabComponent>(processor_, seed);
    juce::CallOutBox::launchAsynchronously(
        std::move(component),
        getLocalBounds().withTrimmedTop(46).reduced(24),
        this);
}

void FFTJuceAudioProcessorEditor::open_match_lab_for_note(int track_index, int source_event_index) {
    const auto seed = processor_.match_lab_seed_for_note(track_index, source_event_index);
    if (!seed.has_value()) {
        processor_.set_status_text_for_debug("No imported MIDI provenance for selected note");
        refresh_labels();
        show_match_lab();
        return;
    }
    show_match_lab(seed);
}

void FFTJuceAudioProcessorEditor::start_local_playback_from_selection() {
    const int32_t start_tick = has_time_selection() ? selected_time_start_tick_ : 0;
    const int32_t end_tick = has_time_selection() ? selected_time_end_tick_ : -1;
    if (!processor_.start_local_playback(start_tick, end_tick)) {
        refresh_labels();
        return;
    }
    refresh_labels();
}

void FFTJuceAudioProcessorEditor::sync_selected_commands_to_views() {
    lane_view_.set_selected_commands(
        selected_command_track_index_,
        selected_command_source_event_indices_,
        selected_command_authored_opcode_indices_);
    detail_view_.set_selected_commands(
        selected_command_track_index_,
        selected_command_source_event_indices_,
        selected_command_authored_opcode_indices_);
    if (selected_command_.has_value()) {
        lane_view_.set_selected_command(
            selected_command_track_index_,
            selected_command_->source_event_index,
            selected_command_->authored_opcode_index,
            selected_command_->opcode);
        detail_view_.set_selected_command(
            selected_command_track_index_,
            selected_command_->source_event_index,
            selected_command_->authored_opcode_index,
            selected_command_->opcode);
    } else {
        lane_view_.set_selected_command(-1, -1, -1, -1);
        detail_view_.set_selected_command(-1, -1, -1, -1);
    }
}

void FFTJuceAudioProcessorEditor::sync_selected_rest_to_views() {
    detail_view_.set_selected_rest(
        selected_rest_track_index_,
        selected_rest_source_event_index_,
        selected_rest_authored_span_index_);
}

void FFTJuceAudioProcessorEditor::sync_selected_time_to_views() {
    lane_header_view_.set_time_selection(selected_time_start_tick_, selected_time_end_tick_);
    if (presentation_mode_ == FFTSmdPresentationMode::playback) {
        lane_view_.set_time_selection(selected_time_start_tick_, selected_time_end_tick_);
        detail_view_.set_time_selection(selected_time_start_tick_, selected_time_end_tick_);
    } else {
        lane_view_.set_time_selection(-1, -1);
        detail_view_.set_time_selection(-1, -1);
    }
}

bool FFTJuceAudioProcessorEditor::has_time_selection() const {
    return selected_time_end_tick_ > selected_time_start_tick_;
}

int32_t FFTJuceAudioProcessorEditor::selected_time_duration_ticks() const {
    return has_time_selection() ? (selected_time_end_tick_ - selected_time_start_tick_) : 0;
}

bool FFTJuceAudioProcessorEditor::unwound_selection_crosses_loop_boundary() const {
    if (!has_time_selection()) {
        return false;
    }

    const auto presentation = processor_.inspector_song_presentation(FFTSmdPresentationMode::playback);
    const int selected_track = processor_.selected_track_id();
    const auto track_it = std::find_if(
        presentation.tracks.begin(),
        presentation.tracks.end(),
        [selected_track](const FFTSmdTrackLanePresentation& track) {
            return track.track_index == selected_track;
        });
    if (track_it == presentation.tracks.end()) {
        return false;
    }

    return std::any_of(
        track_it->loop_boundaries.begin(),
        track_it->loop_boundaries.end(),
        [this](const FFTSmdLaneLoopBoundary& boundary) {
            return boundary.tick > selected_time_start_tick_ && boundary.tick < selected_time_end_tick_;
        });
}

int32_t FFTJuceAudioProcessorEditor::map_visible_tick_to_authored_tick(int track_index, int32_t visible_tick) const {
    if (presentation_mode_ != FFTSmdPresentationMode::playback) {
        return visible_tick;
    }

    const auto presentation = processor_.inspector_song_presentation(presentation_mode_);
    const auto track_it = std::find_if(
        presentation.tracks.begin(),
        presentation.tracks.end(),
        [track_index](const FFTSmdTrackLanePresentation& track) {
            return track.track_index == track_index;
        });
    if (track_it == presentation.tracks.end()) {
        return visible_tick;
    }
    const auto* segment = smd_find_visible_time_map_segment(*track_it, visible_tick);
    if (segment == nullptr || segment->authored_start_tick < 0) {
        return visible_tick;
    }
    return segment->authored_start_tick + (visible_tick - segment->start_tick);
}

std::vector<int32_t> FFTJuceAudioProcessorEditor::visible_positions_for_authored_tick(
    const FFTSmdTrackLanePresentation& track,
    int32_t authored_tick
) const {
    std::vector<int32_t> positions;
    for (const auto& segment : track.time_map_segments) {
        if (segment.authored_start_tick < 0 || segment.duration_ticks <= 0) {
            continue;
        }
        const int32_t authored_end = segment.authored_start_tick + segment.duration_ticks;
        if (authored_tick < segment.authored_start_tick || authored_tick >= authored_end) {
            continue;
        }
        positions.push_back(segment.start_tick + (authored_tick - segment.authored_start_tick));
    }
    std::stable_sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    return positions;
}

void FFTJuceAudioProcessorEditor::clear_selected_command_selection() {
    selected_command_.reset();
    selected_command_track_index_ = -1;
    selected_command_source_event_indices_.clear();
    selected_command_authored_opcode_indices_.clear();
    sync_selected_commands_to_views();
}

void FFTJuceAudioProcessorEditor::clear_selected_rest_selection() {
    selected_rest_track_index_ = -1;
    selected_rest_source_event_index_ = -1;
    selected_rest_authored_span_index_ = -1;
    sync_selected_rest_to_views();
}

void FFTJuceAudioProcessorEditor::refresh_group_buttons() {
    const bool source_mode = presentation_mode_ == FFTSmdPresentationMode::source;
    bool can_group = source_mode && !selected_source_track_indices_.empty();
    for (const int track_index : selected_source_track_indices_) {
        if (processor_.authored_track_is_poly_track(track_index)) {
            can_group = false;
            break;
        }
    }
    const bool can_ungroup = source_mode && processor_.authored_track_is_poly_track(processor_.selected_track_id());
    group_button_.setEnabled(can_group);
    ungroup_button_.setEnabled(can_ungroup);

    const int selected_track = processor_.selected_track_id();
    const bool track_valid = selected_track >= 0;
    const int transposition = track_valid ? processor_.track_transposition(selected_track) : 0;
    juce::String label("Trn: ");
    if (transposition > 0) label << "+";
    label << transposition;
    track_transposition_button_.setButtonText(label);
    track_transposition_button_.setEnabled(track_valid);
}

void FFTJuceAudioProcessorEditor::handle_time_selection_changed(int32_t start_tick, int32_t end_tick) {
    if (end_tick > start_tick) {
        selected_time_start_tick_ = start_tick;
        selected_time_end_tick_ = end_tick;
    } else {
        selected_time_start_tick_ = -1;
        selected_time_end_tick_ = -1;
    }
    sync_selected_time_to_views();
    refresh_labels();
}

void FFTJuceAudioProcessorEditor::handle_detail_command_selected(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    clear_selected_rest_selection();
    selected_command_track_index_ = track_index;
    selected_command_ = command;
    selected_command_source_event_indices_.clear();
    selected_command_authored_opcode_indices_.clear();
    if (command.authored_opcode_index >= 0) {
        selected_command_authored_opcode_indices_.push_back(command.authored_opcode_index);
    }
    if (command.source_event_index >= 0) {
        selected_command_source_event_indices_.push_back(command.source_event_index);
    }
    sync_selected_commands_to_views();

    if (command.opcode != 0xAC ||
        command.source_event_index < 0) {
        if (command.opcode == 0xE0 &&
            command.source_event_index >= 0) {
            show_dynamics_picker(track_index, command);
        } else if (command.opcode == 0xE8 &&
            command.source_event_index >= 0) {
            show_pan_picker(track_index, command);
        } else if (command.opcode == 0xA2 &&
            command.source_event_index >= 0) {
            show_tempo_slide_picker(track_index, command);
        } else if (command.opcode == 0xA3 &&
            command.source_event_index >= 0) {
            show_generic_pair_picker(track_index, command, "Opcode A3", "A3", 0xA3, "A", "B");
        } else if (command.opcode == 0xA4 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Opcode A4", "A4", 0xA4, 0, 255);
        } else if (command.opcode == 0xD7 &&
            command.source_event_index >= 0) {
            show_pitch_lfo_depth_picker(track_index, command);
        } else if (command.opcode == 0xE3 &&
            command.source_event_index >= 0) {
            show_volume_lfo_depth_picker(track_index, command);
        } else if (command.opcode == 0xD8 &&
            command.source_event_index >= 0) {
            show_pitch_lfo_setup_picker(track_index, command);
        } else if (command.opcode == 0xE4 &&
            command.source_event_index >= 0) {
            show_volume_lfo_setup_picker(track_index, command);
        } else if (command.opcode == 0xD9 &&
            command.source_event_index >= 0) {
            show_generic_signed_middle_triple_picker(track_index, command, "Pitch LFO Command", "LFOcmd", 0xD9);
        } else if (command.opcode == 0xE5 &&
            command.source_event_index >= 0) {
            show_generic_signed_middle_triple_picker(track_index, command, "Volume LFO Command", "VolLFOcmd", 0xE5);
        } else if (command.opcode == 0xD0 &&
            command.source_event_index >= 0) {
            show_pitch_bend_picker(track_index, command);
        } else if (command.opcode == 0xD1 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Add Pitch Bend", "Bend+", 0xD1, 0, 255);
        } else if (command.opcode == 0xD2 &&
            command.source_event_index >= 0) {
            show_conditional_flag_picker(track_index, command);
        } else if (command.opcode == 0xD4 &&
            command.source_event_index >= 0) {
            show_generic_pair_picker(track_index, command, "Portamento", "Port", 0xD4, "A", "B");
        } else if (command.opcode == 0xD6 &&
            command.source_event_index >= 0) {
            show_detune_picker(track_index, command);
        } else if (command.opcode == 0xA9 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Unknown A9", "A9", 0xA9, 0, 255);
        } else if (command.opcode == 0xAD &&
            command.source_event_index >= 0) {
            show_unknown_ad_picker(track_index, command);
        } else if (command.opcode == 0xB8 &&
            command.source_event_index >= 0) {
            show_generic_unsigned_triple_picker(track_index, command, "Unknown B8", "B8", 0xB8, "A", "B", "C");
        } else if (command.opcode == 0xB9 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Opcode B9", "B9", 0xB9, 0, 255);
        } else if (command.opcode == 0xC2 &&
            command.source_event_index >= 0) {
            show_adsr_attack_picker(track_index, command);
        } else if (command.opcode == 0xC4 &&
            command.source_event_index >= 0) {
            show_adsr_sustain_rate_picker(track_index, command);
        } else if (command.opcode == 0xC5 &&
            command.source_event_index >= 0) {
            show_adsr_release_picker(track_index, command);
        } else if (command.opcode == 0xC6 &&
            command.source_event_index >= 0) {
            show_adsr_slide_picker(track_index, command);
        } else if (command.opcode == 0xC9 &&
            command.source_event_index >= 0) {
            show_adsr_decay_picker(track_index, command);
        } else if (command.opcode == 0xCA &&
            command.source_event_index >= 0) {
            show_adsr_sustain_level_picker(track_index, command);
        } else if (command.opcode == 0xC7 &&
            command.source_event_index >= 0) {
            show_adsr_decay_sustain_picker(track_index, command);
        } else if (command.opcode == 0xC8 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Unknown C8", "C8", 0xC8, 0, 255);
        } else if (command.opcode == 0xC1 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Opcode C1", "C1", 0xC1, 0, 255);
        } else if (command.opcode == 0xA0 &&
            command.source_event_index >= 0) {
            show_tempo_picker(track_index, command);
        } else if (command.opcode == 0x97 &&
            command.source_event_index >= 0) {
            show_time_signature_picker(track_index, command);
        } else if (command.opcode == 0x94 &&
            command.source_event_index >= 0) {
            show_octave_picker(track_index, command);
        } else if (command.opcode == 0xFE &&
            command.source_event_index >= 0) {
            show_bank_picker(track_index, command);
        } else if (command.opcode == 0xE1 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Add Volume", "Dyn+", 0xE1, 0, 255);
        } else if (command.opcode == 0xE2 &&
            command.source_event_index >= 0) {
            show_generic_pair_picker(track_index, command, "Expression", "Expr", 0xE2, "A", "B");
        } else if (command.opcode == 0xE9 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Unknown E9", "Pan?", 0xE9, 0, 255);
        } else if (command.opcode == 0xEA &&
            command.source_event_index >= 0) {
            show_generic_pair_picker(track_index, command, "Pan Slide", "PanSl", 0xEA, "A", "B");
        } else if (command.opcode == 0xEB &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Pan LFO Depth", "PanLFO", 0xEB, 0, 255);
        } else if (command.opcode == 0xEC &&
            command.source_event_index >= 0) {
            show_generic_signed_middle_triple_picker(track_index, command, "Pan LFO Setup", "PanLFOLn", 0xEC);
        } else if (command.opcode == 0xED &&
            command.source_event_index >= 0) {
            show_generic_signed_middle_triple_picker(track_index, command, "Pan LFO Command", "PanLFO3", 0xED);
        } else if (command.opcode == 0xF8 &&
            command.source_event_index >= 0) {
            show_generic_unsigned_triple_picker(track_index, command, "Unknown F8", "F8", 0xF8, "A", "B", "C");
        } else if (command.opcode == 0xF9 &&
            command.source_event_index >= 0) {
            show_generic_pair_picker(track_index, command, "Unknown F9", "F9", 0xF9, "A", "B");
        } else if (command.opcode == 0xFB &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Unknown FB", "FB", 0xFB, 0, 255);
        } else if (command.opcode == 0xFC &&
            command.source_event_index >= 0) {
            show_generic_pair_picker(track_index, command, "Unknown FC", "FC", 0xFC, "A", "B");
        } else if (command.opcode == 0xFD &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Unknown FD", "FD", 0xFD, 0, 255);
        } else if (command.opcode == 0xF4 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Opcode F4", "F4", 0xF4, 0, 255);
        } else if (command.opcode == 0xF7 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Opcode F7", "F7", 0xF7, 0, 255);
        } else if (command.opcode == 0x98 &&
            command.source_event_index >= 0) {
            show_generic_value_picker(track_index, command, "Repeat Count", "RptStart", 0x98, 1, 255);
        } else if ((command.opcode == 0x95 || command.opcode == 0x96) &&
            command.source_event_index >= 0) {
            show_opcode_choice_picker(
                track_index,
                command,
                "Octave Step",
                {{"RaiseO", 0x95}, {"LowerO", 0x96}});
        } else if ((command.opcode == 0xAE || command.opcode == 0xAF) &&
            command.source_event_index >= 0) {
            show_opcode_choice_picker(
                track_index,
                command,
                "Percussion",
                {{"Perc+", 0xAE}, {"Perc-", 0xAF}});
        } else if ((command.opcode == 0xB0 || command.opcode == 0xB1) &&
            command.source_event_index >= 0) {
            show_opcode_choice_picker(
                track_index,
                command,
                "Slur",
                {{"Slur+", 0xB0}, {"Slur-", 0xB1}});
        } else if ((command.opcode == 0xBA || command.opcode == 0xBB) &&
            command.source_event_index >= 0) {
            show_opcode_choice_picker(
                track_index,
                command,
                "Reverb",
                {{"Rev+", 0xBA}, {"Rev-", 0xBB}});
        } else if ((command.opcode == 0xDA || command.opcode == 0xDB) &&
            command.source_event_index >= 0) {
            show_opcode_choice_picker(
                track_index,
                command,
                "Flag FE",
                {{"FlgFE+", 0xDA}, {"FlgFE-", 0xDB}});
        }
        return;
    }

    show_instrument_picker(track_index, command);
}

void FFTJuceAudioProcessorEditor::handle_detail_command_selection_toggled(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    clear_selected_rest_selection();
    if (command.source_event_index < 0 && command.authored_opcode_index < 0) {
        return;
    }

    if (selected_command_track_index_ != track_index) {
        selected_command_track_index_ = track_index;
        selected_command_source_event_indices_.clear();
        selected_command_authored_opcode_indices_.clear();
    }

    const bool use_authored_identity = command.authored_opcode_index >= 0;
    auto& selected_identities = use_authored_identity
        ? selected_command_authored_opcode_indices_
        : selected_command_source_event_indices_;
    const int32_t command_id = command_identity(command);

    const auto existing_it = std::find(selected_identities.begin(), selected_identities.end(), command_id);
    if (existing_it != selected_identities.end()) {
        selected_identities.erase(existing_it);
        if (selected_command_.has_value() &&
            selected_command_track_index_ == track_index &&
            command_identity(*selected_command_) == command_id) {
            selected_command_.reset();
            if (!selected_identities.empty()) {
                const auto presentation = processor_.inspector_song_presentation(presentation_mode_);
                const auto replacement = use_authored_identity
                    ? find_authored_command(presentation, track_index, selected_identities.front())
                    : find_source_command(presentation, track_index, selected_identities.front());
                if (replacement.has_value()) {
                    selected_command_ = *replacement;
                }
            }
        }
    } else {
        selected_identities.push_back(command_id);
        selected_command_ = command;
    }

    if (selected_command_source_event_indices_.empty() &&
        selected_command_authored_opcode_indices_.empty()) {
        selected_command_track_index_ = -1;
    } else {
        std::sort(selected_command_source_event_indices_.begin(), selected_command_source_event_indices_.end());
        std::sort(selected_command_authored_opcode_indices_.begin(), selected_command_authored_opcode_indices_.end());
    }
    sync_selected_commands_to_views();
}

void FFTJuceAudioProcessorEditor::handle_detail_command_toggle_disabled(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    clear_selected_rest_selection();
    if (presentation_mode_ != FFTSmdPresentationMode::source ||
        command.source_event_index < 0 ||
        command.opcode < 0) {
        return;
    }

    selected_command_track_index_ = track_index;
    selected_command_ = command;
    selected_command_source_event_indices_.clear();
    selected_command_authored_opcode_indices_.clear();
    if (command.authored_opcode_index >= 0) {
        selected_command_authored_opcode_indices_.push_back(command.authored_opcode_index);
    }
    if (command.source_event_index >= 0) {
        selected_command_source_event_indices_.push_back(command.source_event_index);
    }
    sync_selected_commands_to_views();

    const bool disable = command.enabled;
    if (processor_.set_track_source_opcode_disabled(track_index, command.source_event_index, disable)) {
        if (selected_command_.has_value()) {
            selected_command_->enabled = !disable;
        }
        refresh_lane_presentation();
        refresh_labels();
        refresh_inspector();
    }
}

void FFTJuceAudioProcessorEditor::handle_detail_command_delete(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    clear_selected_rest_selection();
    if (command.source_event_index < 0 && command.authored_opcode_index < 0) {
        return;
    }

    const bool use_authored_identity =
        command.authored_opcode_index >= 0 &&
        (presentation_mode_ == FFTSmdPresentationMode::playback ||
         (presentation_mode_ == FFTSmdPresentationMode::source && processor_.authored_track_is_poly_track(track_index)));
    const bool delete_ok =
        use_authored_identity
            ? processor_.delete_authored_opcode(track_index, command.authored_opcode_index, command.opcode)
            : processor_.delete_track_opcode(track_index, command.source_event_index, command.opcode);
    if (!delete_ok) {
        refresh_labels();
        return;
    }

    if (selected_command_.has_value() &&
        selected_command_track_index_ == track_index &&
        command_identity(*selected_command_) == command_identity(command)) {
        clear_selected_command_selection();
    } else {
        if (command.authored_opcode_index >= 0) {
            selected_command_authored_opcode_indices_.erase(
                std::remove(
                    selected_command_authored_opcode_indices_.begin(),
                    selected_command_authored_opcode_indices_.end(),
                    command.authored_opcode_index),
                selected_command_authored_opcode_indices_.end());
        }
        if (command.source_event_index >= 0) {
            selected_command_source_event_indices_.erase(
                std::remove(
                    selected_command_source_event_indices_.begin(),
                    selected_command_source_event_indices_.end(),
                    command.source_event_index),
                selected_command_source_event_indices_.end());
        }
        sync_selected_commands_to_views();
    }

    refresh_lane_presentation();
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_detail_command_move(
    int track_index,
    const FFTSmdLaneCommandBlock& command,
    const FFTSmdLaneInsertAnchor& anchor
) {
    clear_selected_rest_selection();
    if ((command.source_event_index < 0 && command.authored_opcode_index < 0) || command.opcode < 0) {
        return;
    }

    const bool use_authored_identity =
        command.authored_opcode_index >= 0 &&
        (presentation_mode_ == FFTSmdPresentationMode::playback ||
         (presentation_mode_ == FFTSmdPresentationMode::source && processor_.authored_track_is_poly_track(track_index)));
    const int32_t target_tick =
        presentation_mode_ == FFTSmdPresentationMode::playback
            ? map_visible_tick_to_authored_tick(track_index, anchor.tick)
            : anchor.tick;
    const auto& selected_identities = use_authored_identity
        ? selected_command_authored_opcode_indices_
        : selected_command_source_event_indices_;
    const bool move_selection =
        selected_command_track_index_ == track_index &&
        std::find(selected_identities.begin(), selected_identities.end(), command_identity(command)) !=
            selected_identities.end() &&
        selected_identities.size() > 1;

    int moved_source_event_index = -1;
    int moved_authored_opcode_index = -1;
    std::vector<int32_t> moved_source_event_indices;
    std::vector<int32_t> moved_authored_opcode_indices;
    const bool move_ok = use_authored_identity
        ? (move_selection
            ? processor_.move_authored_opcodes(
                track_index,
                selected_command_authored_opcode_indices_,
                command.authored_opcode_index,
                target_tick,
                anchor.insertion_sequence_index,
                &moved_authored_opcode_indices,
                &moved_authored_opcode_index)
            : processor_.move_authored_opcode(
                track_index,
                command.authored_opcode_index,
                command.opcode,
                target_tick,
                anchor.insertion_sequence_index,
                &moved_authored_opcode_index))
        : (move_selection
            ? processor_.move_track_opcodes(
                track_index,
                selected_command_source_event_indices_,
                command.source_event_index,
                target_tick,
                anchor.insertion_sequence_index,
                &moved_source_event_indices,
                &moved_source_event_index)
            : processor_.move_track_opcode(
                track_index,
                command.source_event_index,
                command.opcode,
                target_tick,
                anchor.insertion_sequence_index,
                &moved_source_event_index));
    if (!move_ok) {
        refresh_labels();
        return;
    }

    selected_command_track_index_ = track_index;
    selected_command_ = command;
    if (use_authored_identity) {
        selected_command_->authored_opcode_index = moved_authored_opcode_index;
    } else {
        selected_command_->source_event_index = moved_source_event_index;
    }
    if (move_selection && use_authored_identity) {
        selected_command_authored_opcode_indices_ = std::move(moved_authored_opcode_indices);
        selected_command_source_event_indices_.clear();
    } else if (move_selection) {
        selected_command_source_event_indices_ = std::move(moved_source_event_indices);
    } else {
        if (use_authored_identity) {
            selected_command_authored_opcode_indices_.clear();
            if (moved_authored_opcode_index >= 0) {
                selected_command_authored_opcode_indices_.push_back(moved_authored_opcode_index);
            }
            selected_command_source_event_indices_.clear();
        } else {
            selected_command_source_event_indices_.clear();
            if (moved_source_event_index >= 0) {
                selected_command_source_event_indices_.push_back(moved_source_event_index);
            }
        }
    }
    sync_selected_commands_to_views();

    refresh_lane_presentation();
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_time_lane_note_delete(int track_index, int source_event_index) {
    if (source_event_index < 0) {
        return;
    }

    const bool source_poly =
        presentation_mode_ == FFTSmdPresentationMode::source && processor_.authored_track_is_poly_track(track_index);
    const bool ok = presentation_mode_ == FFTSmdPresentationMode::playback || source_poly
        ? processor_.replace_authored_note_with_rest(track_index, source_event_index)
        : processor_.replace_track_note_with_rest(track_index, source_event_index);
    if (!ok) {
        refresh_labels();
        return;
    }

    clear_selected_command_selection();
    clear_selected_rest_selection();

    refresh_lane_presentation();
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_time_lane_note_insert(
    int track_index,
    int source_event_index,
    int note_relative_key,
    int32_t absolute_start_tick,
    int32_t covered_start_tick,
    int32_t duration_ticks
) {
    const bool playback_poly =
        presentation_mode_ == FFTSmdPresentationMode::playback && processor_.authored_track_is_poly_track(track_index);
    const bool source_poly =
        presentation_mode_ == FFTSmdPresentationMode::source && processor_.authored_track_is_poly_track(track_index);
    if (source_event_index < 0 && !source_poly && !playback_poly) {
        return;
    }

    const int32_t authored_start_tick =
        playback_poly ? map_visible_tick_to_authored_tick(track_index, absolute_start_tick) : absolute_start_tick;
    const int32_t start_offset_ticks = std::max(0, absolute_start_tick - covered_start_tick);
    const bool ok = (source_poly || playback_poly)
        ? (note_relative_key == 13
            ? processor_.insert_authored_poly_rest(
                track_index,
                authored_start_tick,
                duration_ticks)
            : processor_.insert_authored_poly_note(
                track_index,
                note_relative_key,
                authored_start_tick,
                duration_ticks))
        : (presentation_mode_ == FFTSmdPresentationMode::playback
            ? processor_.replace_authored_rest_with_note(
            track_index,
            source_event_index,
                note_relative_key,
                start_offset_ticks,
                duration_ticks)
        : processor_.replace_track_rest_with_note(
            track_index,
            source_event_index,
            note_relative_key,
            start_offset_ticks,
            duration_ticks));
    if (!ok) {
        refresh_labels();
        return;
    }

    clear_selected_command_selection();
    clear_selected_rest_selection();

    refresh_lane_presentation();
    if (playback_poly) {
        juce::String debug_text =
            "DBG vis=" + juce::String(absolute_start_tick) +
            " cov=" + juce::String(covered_start_tick) +
            " auth=" + juce::String(authored_start_tick) +
            " dur=" + juce::String(duration_ticks);
        const auto refreshed = processor_.inspector_song_presentation(presentation_mode_);
        const auto track_it = std::find_if(
            refreshed.tracks.begin(),
            refreshed.tracks.end(),
            [track_index](const FFTSmdTrackLanePresentation& track) {
                return track.track_index == track_index;
            });
        if (track_it != refreshed.tracks.end()) {
            int shown = 0;
            int same_auth_count = 0;
            for (const auto& note : track_it->notes) {
                if (note.relative_key < 0 || note.relative_key >= 12) {
                    continue;
                }
                if (note.authored_start_tick == authored_start_tick &&
                    note.relative_key == note_relative_key) {
                    debug_text +=
                        " | copy" + juce::String(same_auth_count) +
                        "=" + juce::String(note.start_tick);
                    same_auth_count += 1;
                    if (same_auth_count >= 8) {
                        break;
                    }
                }
            }
            debug_text += " | copies=" + juce::String(same_auth_count);

            shown = 0;
            for (const auto& note : track_it->notes) {
                if (note.relative_key < 0 || note.relative_key >= 12) {
                    continue;
                }
                if (std::abs(note.start_tick - absolute_start_tick) > std::max(192, duration_ticks * 4)) {
                    continue;
                }
                debug_text +=
                    " | n" + juce::String(shown) +
                    "=" + juce::String(note.start_tick) +
                    "->" + juce::String(note.authored_start_tick);
                shown += 1;
                if (shown >= 4) {
                    break;
                }
            }
            shown = 0;
            for (const auto& boundary : track_it->loop_boundaries) {
                if (std::abs(boundary.tick - absolute_start_tick) > std::max(192, duration_ticks * 4)) {
                    continue;
                }
                debug_text +=
                    " | b" + juce::String(shown) +
                    "=" + juce::String(boundary.tick) +
                    "->" + juce::String(boundary.authored_tick);
                shown += 1;
                if (shown >= 4) {
                    break;
                }
            }
            shown = 0;
            for (const auto& segment : track_it->time_map_segments) {
                if (std::abs(segment.start_tick - absolute_start_tick) > std::max(192, duration_ticks * 4)) {
                    continue;
                }
                debug_text +=
                    " | s" + juce::String(shown) +
                    "=" + juce::String(segment.start_tick) +
                    "->" + juce::String(segment.authored_start_tick) +
                    "+" + juce::String(segment.duration_ticks) +
                    " g" + juce::String(segment.root_group_id) +
                    " i" + juce::String(segment.loop_instance_id) +
                    " o" + juce::String(segment.occurrence_index) +
                    "/" + juce::String(segment.occurrence_count);
                shown += 1;
                if (shown >= 4) {
                    break;
                }
            }
            shown = 0;
            for (const auto& segment : track_it->time_map_segments) {
                const int32_t end_tick = segment.start_tick + segment.duration_ticks;
                const bool contains_tick =
                    (absolute_start_tick >= segment.start_tick && absolute_start_tick < end_tick) ||
                    absolute_start_tick == segment.start_tick;
                if (!contains_tick) {
                    continue;
                }
                debug_text +=
                    " | c" + juce::String(shown) +
                    "=" + juce::String(segment.start_tick) +
                    "->" + juce::String(segment.authored_start_tick) +
                    "+" + juce::String(segment.duration_ticks) +
                    " g" + juce::String(segment.root_group_id) +
                    " i" + juce::String(segment.loop_instance_id) +
                    " o" + juce::String(segment.occurrence_index) +
                    "/" + juce::String(segment.occurrence_count);
                shown += 1;
                if (shown >= 8) {
                    break;
                }
            }
        }
        if (track_it != refreshed.tracks.end()) {
            const auto* mapped_segment = smd_find_visible_time_map_segment(*track_it, absolute_start_tick);
            if (mapped_segment != nullptr) {
                debug_text +=
                    " | seg=" + juce::String(mapped_segment->start_tick) +
                    "->" + juce::String(mapped_segment->authored_start_tick) +
                    "+" + juce::String(mapped_segment->duration_ticks) +
                    " g" + juce::String(mapped_segment->root_group_id) +
                    " i" + juce::String(mapped_segment->loop_instance_id) +
                    " o" + juce::String(mapped_segment->occurrence_index) +
                    "/" + juce::String(mapped_segment->occurrence_count);
            } else {
                debug_text += " | seg=NONE";
            }
        }
        processor_.set_status_text_for_debug(debug_text);
        append_unwound_insert_debug_line(debug_text.toStdString());
    }
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_time_lane_note_resize(
    int track_index,
    int source_event_index,
    int32_t start_tick,
    int32_t base_duration_ticks,
    int32_t extension_ticks
) {
    if (source_event_index < 0) {
        return;
    }

    const bool source_poly =
        presentation_mode_ == FFTSmdPresentationMode::source && processor_.authored_track_is_poly_track(track_index);
    const bool ok = presentation_mode_ == FFTSmdPresentationMode::playback || source_poly
        ? processor_.set_authored_note_geometry(
            track_index,
            source_event_index,
            start_tick,
            base_duration_ticks,
            extension_ticks)
        : processor_.set_track_note_geometry(
            track_index,
            source_event_index,
            start_tick,
            base_duration_ticks,
            extension_ticks);
    if (!ok) {
        refresh_labels();
        return;
    }

    clear_selected_command_selection();
    clear_selected_rest_selection();

    refresh_lane_presentation();
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_time_lane_note_fermata(
    int track_index,
    int source_event_index,
    int32_t extension_ticks
) {
    if (source_event_index < 0) {
        return;
    }

    const bool source_poly =
        presentation_mode_ == FFTSmdPresentationMode::source && processor_.authored_track_is_poly_track(track_index);
    const bool ok = presentation_mode_ == FFTSmdPresentationMode::playback || source_poly
        ? processor_.set_authored_note_fermata_extension(
            track_index,
            source_event_index,
            extension_ticks)
        : processor_.set_track_note_fermata_extension(
            track_index,
            source_event_index,
            extension_ticks);
    if (!ok) {
        refresh_labels();
        return;
    }

    clear_selected_command_selection();
    clear_selected_rest_selection();

    refresh_lane_presentation();
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_time_lane_rest_selected(
    int track_index,
    int source_event_index,
    int authored_span_index
) {
    selected_rest_track_index_ = track_index;
    selected_rest_source_event_index_ = source_event_index;
    selected_rest_authored_span_index_ = authored_span_index;
    clear_selected_command_selection();
    sync_selected_rest_to_views();
    refresh_labels();
}

void FFTJuceAudioProcessorEditor::handle_time_lane_rest_ripple_resize(
    int track_index,
    int source_event_index,
    int authored_span_index,
    int32_t delta_ticks
) {
    if ((source_event_index < 0 && authored_span_index < 0) || delta_ticks == 0) {
        return;
    }

    const bool use_authored = presentation_mode_ == FFTSmdPresentationMode::playback || authored_span_index >= 0;
    const bool ok = use_authored
        ? processor_.resize_authored_rest_duration(
            track_index,
            authored_span_index >= 0 ? authored_span_index : source_event_index,
            delta_ticks)
        : processor_.resize_track_rest_duration(track_index, source_event_index, delta_ticks);
    if (!ok) {
        refresh_labels();
        return;
    }

    selected_rest_track_index_ = track_index;
    selected_rest_source_event_index_ = source_event_index;
    selected_rest_authored_span_index_ = authored_span_index;
    clear_selected_command_selection();

    refresh_lane_presentation();
    refresh_labels();
    refresh_inspector();
}

void FFTJuceAudioProcessorEditor::handle_insert_anchor_selected(
    int track_index,
    const FFTSmdLaneInsertAnchor& anchor
) {
    show_opcode_insert_picker(track_index, anchor);
}

void FFTJuceAudioProcessorEditor::show_opcode_insert_picker(
    int track_index,
    const FFTSmdLaneInsertAnchor& anchor
) {
    auto picker = std::make_unique<FFTOpcodeChoicePickerContent>(
        "Insert Opcode at " + juce::String(anchor.label),
        -1,
        insertion_opcode_choices(),
        [this, track_index, anchor](int opcode) {
            int inserted_source_event_index = -1;
            const int32_t target_tick =
                presentation_mode_ == FFTSmdPresentationMode::playback
                    ? map_visible_tick_to_authored_tick(track_index, anchor.tick)
                    : anchor.tick;
            if (!processor_.insert_track_opcode(
                    track_index,
                    target_tick,
                    anchor.insertion_sequence_index,
                    opcode,
                    default_params_for_opcode(opcode),
                    &inserted_source_event_index)) {
                refresh_labels();
                return;
            }

            refresh_lane_presentation();
            refresh_labels();
            refresh_inspector();

            const auto inserted_command = find_source_command(
                processor_.inspector_song_presentation(presentation_mode_),
                track_index,
                inserted_source_event_index);
            if (!inserted_command.has_value()) {
                return;
            }

            selected_command_track_index_ = track_index;
            selected_command_ = inserted_command;
            selected_command_source_event_indices_.clear();
            selected_command_source_event_indices_.push_back(inserted_command->source_event_index);
            sync_selected_commands_to_views();

            if (smd_opcode_param_count(static_cast<uint8_t>(opcode)) > 0) {
                handle_detail_command_selected(track_index, *inserted_command);
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::adjust_note_snap_mode(int delta) {
    if (delta == 0) {
        return;
    }

    note_snap_mode_index_ = juce::jlimit(0, 8, note_snap_mode_index_ + delta);
    detail_view_.set_note_snap_mode_index(note_snap_mode_index_);
    lane_view_.set_note_snap_mode_index(note_snap_mode_index_);
    refresh_labels();
}

void FFTJuceAudioProcessorEditor::show_dynamics_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Dyn");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Dynamics",
        0,
        127,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_dynamics_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Dyn " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_pan_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Pan");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Pan",
        0,
        127,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_pan_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Pan " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_pitch_lfo_depth_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "LFO");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Pitch LFO Depth",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_pitch_lfo_depth_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "LFO " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_pitch_lfo_setup_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    int32_t current_length = 0;
    int32_t current_shape = 0;
    int32_t current_depth = 0;
    command_param_or_label_triple(command, 0, 0, 0, "LFOlen", current_length, current_shape, current_depth);

    auto picker = std::make_unique<FFTPitchLFOPickerContent>(
        "Pitch LFO",
        current_length,
        current_shape,
        current_depth,
        [this, track_index, command](int32_t new_length, int32_t new_shape, int32_t new_depth) {
            if (processor_.set_track_pitch_lfo_opcode_values(
                    track_index,
                    command.source_event_index,
                    new_length,
                    new_shape,
                    new_depth)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "LFOlen " + std::to_string(new_length) + "/" +
                        std::to_string(new_shape) + "/" + std::to_string(new_depth);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_volume_lfo_depth_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "VolLFO");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Volume LFO Depth",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_volume_lfo_depth_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "VolLFO " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_volume_lfo_setup_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    int32_t current_length = 0;
    int32_t current_shape = 0;
    int32_t current_depth = 0;
    command_param_or_label_triple(command, 0, 0, 0, "VolLFOlen", current_length, current_shape, current_depth);

    auto picker = std::make_unique<FFTPitchLFOPickerContent>(
        "Volume LFO",
        current_length,
        current_shape,
        current_depth,
        [this, track_index, command](int32_t new_length, int32_t new_shape, int32_t new_depth) {
            if (processor_.set_track_volume_lfo_opcode_values(
                    track_index,
                    command.source_event_index,
                    new_length,
                    new_shape,
                    new_depth)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "VolLFOlen " + std::to_string(new_length) + "/" +
                        std::to_string(new_shape) + "/" + std::to_string(new_depth);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_pitch_bend_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Bend");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Pitch Bend",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_pitch_bend_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Bend " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_conditional_flag_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Cond");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Conditional Flag",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_conditional_seq_flag_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Cond " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_detune_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Detune");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Detune",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_detune_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Detune " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_unknown_ad_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "AD?");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Opcode AD",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_unknown_ad_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "AD? " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_attack_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Atk");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "ADSR Attack",
        0,
        127,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_adsr_attack_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Atk " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_sustain_rate_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "SusRt");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "ADSR Sustain Rate",
        0,
        127,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_adsr_sustain_rate_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "SusRt " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_release_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Rel");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "ADSR Release",
        0,
        31,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_adsr_release_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Rel " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_slide_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Slide");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "ADSR Slide",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_adsr_slide_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Slide " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_decay_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Dec");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "ADSR Decay",
        0,
        15,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_adsr_decay_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Dec " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_sustain_level_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "SusLv");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "ADSR Sustain Level",
        0,
        15,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_adsr_sustain_level_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "SusLv " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_adsr_decay_sustain_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    int32_t decay_value = 0;
    int32_t sustain_level_value = 0;
    command_param_or_label_pair(command, 0, 0, "Dec/Sus", decay_value, sustain_level_value);
    if (command.opcode_params.size() < 2 && decay_value == 0 && sustain_level_value == 0) {
        command_param_or_label_pair(command, 0, 0, "ADSR17", decay_value, sustain_level_value);
    }

    auto picker = std::make_unique<FFTADSRC7PickerContent>(
        decay_value,
        sustain_level_value,
        [this, track_index, command](int32_t new_decay, int32_t new_sustain_level) {
            if (processor_.set_track_adsr_decay_sustain_opcode_values(
                    track_index,
                    command.source_event_index,
                    new_decay,
                    new_sustain_level)) {
                if (selected_command_.has_value()) {
                    selected_command_->label =
                        "Dec/Sus " + std::to_string(new_decay) + "/" + std::to_string(new_sustain_level);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_octave_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 4, "Oct");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Octave",
        0,
        8,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_octave_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Oct " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_bank_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, "Bank");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Bank",
        0,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_bank_select_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Bank " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_tempo_slide_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    int32_t first_value = 0;
    int32_t second_value = 0;
    command_param_or_label_pair(command, 0, 0, "TmpSl", first_value, second_value);

    auto picker = std::make_unique<FFTRawPairPickerContent>(
        "Tempo Slide",
        "A",
        "B",
        first_value,
        second_value,
        [this, track_index, command](int32_t new_first, int32_t new_second) {
            if (processor_.set_track_tempo_slide_opcode_values(
                    track_index,
                    command.source_event_index,
                    new_first,
                    new_second)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "TmpSl " + std::to_string(new_first) + "/" + std::to_string(new_second);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_generic_value_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command,
    const juce::String& title,
    const juce::String& label_prefix,
    int expected_opcode,
    int min_value,
    int max_value
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 0, label_prefix);
    auto picker = std::make_unique<FFTValuePickerContent>(
        title,
        min_value,
        max_value,
        current_value,
        [this, track_index, command, label_prefix, expected_opcode, min_value, max_value](int32_t value) {
            if (processor_.set_track_generic_opcode_param_value(
                    track_index,
                    command.source_event_index,
                    expected_opcode,
                    value,
                    min_value,
                    max_value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = label_prefix.toStdString() + " " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_generic_pair_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command,
    const juce::String& title,
    const juce::String& label_prefix,
    int expected_opcode,
    const juce::String& first_label,
    const juce::String& second_label
) {
    int32_t current_first = 0;
    int32_t current_second = 0;
    command_param_or_label_pair(command, 0, 0, label_prefix, current_first, current_second);

    auto picker = std::make_unique<FFTRawPairPickerContent>(
        title,
        first_label,
        second_label,
        current_first,
        current_second,
        [this, track_index, command, label_prefix, expected_opcode](int32_t first_value, int32_t second_value) {
            if (processor_.set_track_generic_opcode_param_values(
                    track_index,
                    command.source_event_index,
                    expected_opcode,
                    {first_value, second_value})) {
                if (selected_command_.has_value()) {
                    selected_command_->label = label_prefix.toStdString() + " " +
                        std::to_string(first_value) + "/" + std::to_string(second_value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_generic_unsigned_triple_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command,
    const juce::String& title,
    const juce::String& label_prefix,
    int expected_opcode,
    const juce::String& first_label,
    const juce::String& second_label,
    const juce::String& third_label
) {
    int32_t current_first = 0;
    int32_t current_second = 0;
    int32_t current_third = 0;
    command_param_or_label_triple(command, 0, 0, 0, label_prefix, current_first, current_second, current_third);

    auto picker = std::make_unique<FFTRawTriplePickerContent>(
        title,
        first_label,
        second_label,
        third_label,
        current_first,
        current_second,
        current_third,
        [this, track_index, command, label_prefix, expected_opcode](
            int32_t first_value,
            int32_t second_value,
            int32_t third_value) {
            if (processor_.set_track_generic_opcode_param_values(
                    track_index,
                    command.source_event_index,
                    expected_opcode,
                    {first_value, second_value, third_value})) {
                if (selected_command_.has_value()) {
                    selected_command_->label = label_prefix.toStdString() + " " +
                        std::to_string(first_value) + "/" +
                        std::to_string(second_value) + "/" +
                        std::to_string(third_value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_generic_signed_middle_triple_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command,
    const juce::String& title,
    const juce::String& label_prefix,
    int expected_opcode
) {
    int32_t current_first = 0;
    int32_t current_second = 0;
    int32_t current_third = 0;
    command_param_or_label_triple(command, 0, 0, 0, label_prefix, current_first, current_second, current_third);

    auto picker = std::make_unique<FFTPitchLFOPickerContent>(
        title,
        current_first,
        current_second,
        current_third,
        [this, track_index, command, label_prefix, expected_opcode](
            int32_t first_value,
            int32_t second_value,
            int32_t third_value) {
            const int32_t raw_second_value = second_value < 0 ? (256 + second_value) : second_value;
            if (processor_.set_track_generic_opcode_param_values(
                    track_index,
                    command.source_event_index,
                    expected_opcode,
                    {first_value, raw_second_value, third_value})) {
                if (selected_command_.has_value()) {
                    selected_command_->label = label_prefix.toStdString() + " " +
                        std::to_string(first_value) + "/" +
                        std::to_string(second_value) + "/" +
                        std::to_string(third_value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_opcode_choice_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command,
    const juce::String& title,
    const std::vector<std::pair<juce::String, int>>& choices
) {
    std::vector<FFTOpcodeChoiceEntry> picker_choices;
    picker_choices.reserve(choices.size());
    for (const auto& choice : choices) {
        picker_choices.push_back(FFTOpcodeChoiceEntry {
            .name = choice.first,
            .opcode = choice.second,
            .description = {},
        });
    }
    auto picker = std::make_unique<FFTOpcodeChoicePickerContent>(
        title,
        command.opcode,
        std::move(picker_choices),
        [this, track_index, command, choices](int new_opcode) {
            if (processor_.set_track_opcode_code(track_index, command.source_event_index, command.opcode, new_opcode)) {
                if (selected_command_.has_value()) {
                    selected_command_->opcode = new_opcode;
                    for (const auto& choice : choices) {
                        if (choice.second == new_opcode) {
                            selected_command_->label = choice.first.toStdString();
                            break;
                        }
                    }
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_tempo_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_value = command_param_or_label_value(command, 0, 102, "Tempo");

    auto picker = std::make_unique<FFTValuePickerContent>(
        "Tempo",
        1,
        255,
        current_value,
        [this, track_index, command](int32_t value) {
            if (processor_.set_track_tempo_opcode_value(track_index, command.source_event_index, value)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = "Tempo " + std::to_string(value);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_time_signature_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    int32_t numerator = 4;
    int32_t denominator = 4;
    command_param_or_label_pair(command, 4, 4, "TimeSig", numerator, denominator);

    auto picker = std::make_unique<FFTTimeSignaturePickerContent>(
        numerator,
        denominator,
        [this, track_index, command](int32_t new_numerator, int32_t new_denominator) {
            if (processor_.set_track_time_signature_opcode_values(
                    track_index,
                    command.source_event_index,
                    new_numerator,
                    new_denominator)) {
                if (selected_command_.has_value()) {
                    selected_command_->label =
                        "TimeSig " + std::to_string(new_numerator) + "/" + std::to_string(new_denominator);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

void FFTJuceAudioProcessorEditor::show_instrument_picker(
    int track_index,
    const FFTSmdLaneCommandBlock& command
) {
    const int32_t current_instrument_id =
        smd_instrument_opcode_param_to_played_sample_id(command_param_or_label_value(command, 0, 0, "Inst"));

    auto picker = std::make_unique<FFTInstrumentPickerContent>(
        current_instrument_id,
        [this, track_index, command](int32_t played_sample_id) {
            const int32_t instrument_id =
                smd_played_sample_id_to_instrument_opcode_param(played_sample_id);
            if (processor_.set_track_instrument_opcode_value(track_index, command.source_event_index, instrument_id)) {
                if (selected_command_.has_value()) {
                    selected_command_->label = smd_format_instrument_label_from_opcode_param(instrument_id);
                }
                refresh_lane_presentation();
                refresh_labels();
                refresh_inspector();
            }
        });

    juce::CallOutBox::launchAsynchronously(
        std::move(picker),
        detail_view_.getBoundsInParent(),
        this);
}

}  // namespace jucewrap
}  // namespace fftplugin
