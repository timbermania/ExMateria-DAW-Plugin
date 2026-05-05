#include "FFTPianoDetailView.h"
#include "FFTOpcodeStackLayout.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace fftplugin {
namespace jucewrap {

namespace {

constexpr const char* kRelativeKeyNames[] = {
    "B", "A#", "A", "G#", "G", "F#", "F", "E", "D#", "D", "C#", "C",
};
constexpr int kPitchRowCount = 12;
constexpr int kRestRowCount = 1;
constexpr int kTotalRowCount = kPitchRowCount + kRestRowCount;
constexpr int kNoteRowHeight = 14;
constexpr int kSectionGap = 8;
constexpr int kOpcodeRowMinHeight = 16;
constexpr int kHoverTickRadiusPx = 14;

juce::Colour diagnostic_colour(FFTSmdLaneDiagnosticSeverity severity) {
    return severity == FFTSmdLaneDiagnosticSeverity::error
        ? juce::Colour::fromRGB(224, 88, 88)
        : juce::Colour::fromRGB(230, 178, 74);
}

juce::Colour loop_boundary_colour(int32_t depth) {
    switch (depth % 4) {
    case 1: return juce::Colour::fromRGB(214, 124, 82);
    case 2: return juce::Colour::fromRGB(120, 184, 112);
    case 3: return juce::Colour::fromRGB(102, 156, 214);
    default: return juce::Colour::fromRGB(196, 150, 74);
    }
}

struct GroupedDiagnostic {
    int32_t tick = 0;
    FFTSmdLaneDiagnosticSeverity severity = FFTSmdLaneDiagnosticSeverity::warning;
    juce::String message;
};

std::vector<GroupedDiagnostic> group_diagnostics(const std::vector<FFTSmdLaneDiagnostic>& diagnostics) {
    std::map<int32_t, GroupedDiagnostic> grouped;
    for (const auto& diagnostic : diagnostics) {
        auto& entry = grouped[diagnostic.tick];
        entry.tick = diagnostic.tick;
        if (diagnostic.severity == FFTSmdLaneDiagnosticSeverity::error) {
            entry.severity = FFTSmdLaneDiagnosticSeverity::error;
        }
        if (entry.message.isNotEmpty()) {
            entry.message << "\n";
        }
        entry.message << juce::String(diagnostic.short_label) << ": " << juce::String(diagnostic.message);
    }

    std::vector<GroupedDiagnostic> result;
    result.reserve(grouped.size());
    for (const auto& [tick, diagnostic] : grouped) {
        juce::ignoreUnused(tick);
        result.push_back(diagnostic);
    }
    return result;
}

juce::Colour grid_colour(bool is_bar) {
    return is_bar ? juce::Colour::fromRGB(82, 86, 98)
                  : juce::Colour::fromRGB(48, 52, 62);
}

juce::Colour command_fill_colour(FFTSmdLaneCommandKind kind) {
    switch (kind) {
    case FFTSmdLaneCommandKind::note:
        return juce::Colour::fromRGB(80, 132, 212);
    case FFTSmdLaneCommandKind::rest:
        return juce::Colour::fromRGB(108, 114, 126);
    case FFTSmdLaneCommandKind::hold:
        return juce::Colour::fromRGB(196, 150, 74);
    case FFTSmdLaneCommandKind::structure:
        return juce::Colour::fromRGB(127, 82, 84);
    case FFTSmdLaneCommandKind::tempo:
        return juce::Colour::fromRGB(86, 118, 96);
    case FFTSmdLaneCommandKind::opcode:
    default:
        return juce::Colour::fromRGB(78, 82, 92);
    }
}

int32_t round_tick_to_grid_segment(int32_t raw_tick, const FFTSmdGridSegment* segment, int32_t step) {
    if (segment == nullptr) {
        return std::max(0, raw_tick);
    }
    const int32_t origin = segment->start_tick;
    const int32_t clamped_tick = std::max(origin, raw_tick);
    const int32_t delta = clamped_tick - origin;
    int32_t snapped = origin;
    if (step > 0) {
        const int32_t rounded_steps = (delta + (step / 2)) / step;
        snapped = origin + rounded_steps * step;
    }
    return std::clamp(snapped, segment->start_tick, segment->end_tick);
}

int32_t command_identity(const FFTSmdLaneCommandBlock& command) {
    return command.authored_opcode_index >= 0 ? command.authored_opcode_index : command.source_event_index;
}

bool command_uses_authored_identity(const FFTSmdLaneCommandBlock& command) {
    return command.authored_opcode_index >= 0;
}

}  // namespace

FFTPianoDetailView::FFTPianoDetailView() {
    setInterceptsMouseClicks(true, false);
}

void FFTPianoDetailView::set_presentation(FFTSmdSongPresentation presentation, int selected_track_index) {
    presentation_ = std::move(presentation);
    selected_track_index_ = selected_track_index;
    note_preview_.reset();
    active_note_draft_.reset();
    fermata_preview_.reset();
    active_fermata_draft_.reset();
    resize_preview_.reset();
    active_resize_draft_.reset();
    insert_preview_anchor_.reset();
    hovered_diagnostic_marker_.reset();
    repaint();
}

void FFTPianoDetailView::set_selected_command(
    int track_index,
    int source_event_index,
    int authored_opcode_index,
    int opcode
) {
    selected_command_track_index_ = track_index;
    selected_command_source_event_index_ = source_event_index;
    selected_command_authored_opcode_index_ = authored_opcode_index;
    selected_command_opcode_ = opcode;
    repaint();
}

void FFTPianoDetailView::set_selected_commands(
    int track_index,
    const std::vector<int32_t>& source_event_indices,
    const std::vector<int32_t>& authored_opcode_indices
) {
    if (track_index == selected_track_index_) {
        selected_command_source_event_indices_ = source_event_indices;
        selected_command_authored_opcode_indices_ = authored_opcode_indices;
    } else {
        selected_command_source_event_indices_.clear();
        selected_command_authored_opcode_indices_.clear();
    }
    repaint();
}

void FFTPianoDetailView::set_horizontal_scroll(int horizontal_scroll) {
    horizontal_scroll_ = std::max(0, horizontal_scroll);
    repaint();
}

void FFTPianoDetailView::set_pixels_per_tick(float pixels_per_tick) {
    pixels_per_tick_ = juce::jlimit(0.1f, 12.0f, pixels_per_tick);
    repaint();
}

void FFTPianoDetailView::set_zoom_callback(std::function<void(float, float)> callback) {
    zoom_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_pan_callback(std::function<void(int, int)> callback) {
    pan_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_command_selected_callback(
    std::function<void(int, FFTSmdLaneCommandBlock)> callback
) {
    command_selected_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_command_selection_toggled_callback(
    std::function<void(int, FFTSmdLaneCommandBlock)> callback
) {
    command_selection_toggled_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_command_secondary_click_callback(
    std::function<void(int, FFTSmdLaneCommandBlock)> callback
) {
    command_secondary_click_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_command_delete_callback(
    std::function<void(int, FFTSmdLaneCommandBlock)> callback
) {
    command_delete_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_command_move_callback(
    std::function<void(int, FFTSmdLaneCommandBlock, FFTSmdLaneInsertAnchor)> callback
) {
    command_move_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_delete_callback(std::function<void(int, int)> callback) {
    note_delete_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_fermata_delete_callback(std::function<void(int, int)> callback) {
    note_fermata_delete_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_match_lab_callback(std::function<void(int, int)> callback) {
    note_match_lab_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_resize_callback(
    std::function<void(int, int, int32_t, int32_t, int32_t)> callback
) {
    note_resize_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_insert_callback(
    std::function<void(int, int, int, int32_t, int32_t, int32_t)> callback
) {
    note_insert_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_fermata_callback(std::function<void(int, int, int32_t)> callback) {
    note_fermata_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_rest_selected_callback(std::function<void(int, int, int)> callback) {
    rest_selected_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_rest_ripple_resize_callback(std::function<void(int, int, int, int32_t)> callback) {
    rest_ripple_resize_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_selected_rest(int track_index, int source_event_index, int authored_span_index) {
    selected_rest_track_index_ = track_index;
    selected_rest_source_event_index_ = source_event_index;
    selected_rest_authored_span_index_ = authored_span_index;
    repaint();
}

void FFTPianoDetailView::set_time_selection(int32_t start_tick, int32_t end_tick) {
    selected_time_start_tick_ = start_tick;
    selected_time_end_tick_ = end_tick;
    repaint();
}

void FFTPianoDetailView::set_insert_anchor_callback(
    std::function<void(int, FFTSmdLaneInsertAnchor)> callback
) {
    insert_anchor_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_filter_changed_callback(
    std::function<void(const FFTOpcodeFilterState&)> callback
) {
    filter_changed_callback_ = std::move(callback);
}

void FFTPianoDetailView::set_note_snap_mode_index(int mode_index) {
    note_snap_mode_index_ = std::clamp(mode_index, 0, 8);
    repaint();
}

void FFTPianoDetailView::set_note_snap_adjust_callback(std::function<void(int)> callback) {
    note_snap_adjust_callback_ = std::move(callback);
}

FFTOpcodeFilterState FFTPianoDetailView::filter_state() const {
    return FFTOpcodeFilterState {
        .show_note_commands = show_note_commands_,
        .show_rest_commands = show_rest_commands_,
        .show_hold_commands = show_hold_commands_,
        .show_instrument_commands = show_instrument_commands_,
        .show_dynamics_commands = show_dynamics_commands_,
        .show_pan_commands = show_pan_commands_,
        .show_tempo_commands = show_tempo_commands_,
        .show_adsr_commands = show_adsr_commands_,
        .show_octave_commands = show_octave_commands_,
        .show_lfo_commands = show_lfo_commands_,
        .show_bend_commands = show_bend_commands_,
        .show_detune_commands = show_detune_commands_,
        .show_reverb_commands = show_reverb_commands_,
        .show_slur_commands = show_slur_commands_,
        .show_percussion_commands = show_percussion_commands_,
        .show_structure_commands = show_structure_commands_,
    };
}

float FFTPianoDetailView::tick_to_x(int32_t tick) const {
    return static_cast<float>(kHeaderWidth + kTimelinePadding - horizontal_scroll_) +
        (static_cast<float>(tick) * pixels_per_tick_);
}

float FFTPianoDetailView::x_to_tick(float x) const {
    const float content_x = x + static_cast<float>(horizontal_scroll_ - kHeaderWidth - kTimelinePadding);
    return std::max(0.0f, content_x / pixels_per_tick_);
}

const FFTSmdTrackLanePresentation* FFTPianoDetailView::selected_track() const {
    for (const auto& track : presentation_.tracks) {
        if (track.track_index == selected_track_index_) {
            return &track;
        }
    }
    return nullptr;
}

bool FFTPianoDetailView::opcode_filter_active() const {
    return fft_opcode_filter_active(filter_state());
}

bool FFTPianoDetailView::opcode_filter_enabled(OpcodeFilterCategory category) const {
    switch (category) {
    case OpcodeFilterCategory::instrument:
        return show_instrument_commands_;
    case OpcodeFilterCategory::dynamics:
        return show_dynamics_commands_;
    case OpcodeFilterCategory::pan:
        return show_pan_commands_;
    case OpcodeFilterCategory::tempo:
        return show_tempo_commands_;
    case OpcodeFilterCategory::adsr:
        return show_adsr_commands_;
    case OpcodeFilterCategory::octave:
        return show_octave_commands_;
    case OpcodeFilterCategory::lfo:
        return show_lfo_commands_;
    case OpcodeFilterCategory::bend:
        return show_bend_commands_;
    case OpcodeFilterCategory::detune:
        return show_detune_commands_;
    case OpcodeFilterCategory::reverb:
        return show_reverb_commands_;
    case OpcodeFilterCategory::slur:
        return show_slur_commands_;
    case OpcodeFilterCategory::percussion:
        return show_percussion_commands_;
    case OpcodeFilterCategory::structure:
        return show_structure_commands_;
    }
    return false;
}

bool FFTPianoDetailView::should_draw_command(const FFTSmdLaneCommandBlock& command) const {
    return insert_mode_active_ || fft_should_draw_filtered_command(command, filter_state());
}

void FFTPianoDetailView::toggle_filter_button(FilterButtonTarget::Kind kind) {
    switch (kind) {
    case FilterButtonTarget::Kind::notes:
        show_note_commands_ = !show_note_commands_;
        break;
    case FilterButtonTarget::Kind::rests:
        show_rest_commands_ = !show_rest_commands_;
        break;
    case FilterButtonTarget::Kind::holds:
        show_hold_commands_ = !show_hold_commands_;
        break;
    case FilterButtonTarget::Kind::instrument:
        show_instrument_commands_ = !show_instrument_commands_;
        break;
    case FilterButtonTarget::Kind::dynamics:
        show_dynamics_commands_ = !show_dynamics_commands_;
        break;
    case FilterButtonTarget::Kind::pan:
        show_pan_commands_ = !show_pan_commands_;
        break;
    case FilterButtonTarget::Kind::tempo:
        show_tempo_commands_ = !show_tempo_commands_;
        break;
    case FilterButtonTarget::Kind::adsr:
        show_adsr_commands_ = !show_adsr_commands_;
        break;
    case FilterButtonTarget::Kind::octave:
        show_octave_commands_ = !show_octave_commands_;
        break;
    case FilterButtonTarget::Kind::lfo:
        show_lfo_commands_ = !show_lfo_commands_;
        break;
    case FilterButtonTarget::Kind::bend:
        show_bend_commands_ = !show_bend_commands_;
        break;
    case FilterButtonTarget::Kind::detune:
        show_detune_commands_ = !show_detune_commands_;
        break;
    case FilterButtonTarget::Kind::reverb:
        show_reverb_commands_ = !show_reverb_commands_;
        break;
    case FilterButtonTarget::Kind::slur:
        show_slur_commands_ = !show_slur_commands_;
        break;
    case FilterButtonTarget::Kind::percussion:
        show_percussion_commands_ = !show_percussion_commands_;
        break;
    case FilterButtonTarget::Kind::structure:
        show_structure_commands_ = !show_structure_commands_;
        break;
    }
    if (filter_changed_callback_) {
        filter_changed_callback_(filter_state());
    }
    repaint();
}

void FFTPianoDetailView::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour::fromRGB(20, 22, 27));
    visible_note_blocks_.clear();
    visible_command_chips_.clear();
    hover_overlay_command_chips_.clear();
    visible_diagnostic_markers_.clear();
    visible_insert_anchors_.clear();
    filter_button_targets_.clear();

    auto bounds = getLocalBounds();
    g.setColour(juce::Colour::fromRGB(50, 54, 64));
    g.drawRect(bounds);

    auto header = bounds.removeFromLeft(kHeaderWidth);
    auto timeline = bounds.reduced(kTimelinePadding, 0);

    const int note_top = timeline.getY() + 4;
    const int note_bottom = note_top + (kTotalRowCount * kNoteRowHeight);
    const int opcode_top = note_bottom + kSectionGap;
    const int opcode_bottom = timeline.getBottom() - 4;
    const int opcode_available_height = std::max(0, opcode_bottom - opcode_top);
    const int opcode_row_count = std::max(1, opcode_available_height / kOpcodeRowMinHeight);
    const int opcode_row_h = opcode_row_count > 0
        ? std::max(kOpcodeRowMinHeight, opcode_available_height / opcode_row_count)
        : kOpcodeRowMinHeight;
    const int grid_top = note_top;
    const int grid_bottom = std::max(note_bottom, opcode_top + (opcode_row_count * opcode_row_h));

    for (int row = 0; row < kTotalRowCount; ++row) {
        const int y = note_top + row * kNoteRowHeight;
        g.setColour(row == kPitchRowCount ? juce::Colour::fromRGB(38, 41, 49)
                              : ((row == 1 || row == 3 || row == 6 || row == 8 || row == 10)
                                    ? juce::Colour::fromRGB(40, 43, 52)
                                    : juce::Colour::fromRGB(46, 50, 60)));
        g.fillRect(timeline.withY(y).withHeight(kNoteRowHeight));
        g.setColour(juce::Colour::fromRGB(84, 88, 100));
        g.drawHorizontalLine(y + kNoteRowHeight, static_cast<float>(timeline.getX()), static_cast<float>(timeline.getRight()));

        g.setColour(juce::Colour::fromRGB(205, 208, 216));
        if (row < kPitchRowCount) {
            g.drawText(kRelativeKeyNames[row], header.getX() + 8, y, header.getWidth() - 16, kNoteRowHeight, juce::Justification::centredRight);
        } else {
            g.drawText("Rest", header.getX() + 8, y, header.getWidth() - 16, kNoteRowHeight, juce::Justification::centredRight);
        }
    }

    g.setColour(juce::Colour::fromRGB(84, 88, 100));
    g.drawHorizontalLine(opcode_top - (kSectionGap / 2), static_cast<float>(timeline.getX()), static_cast<float>(timeline.getRight()));
    const auto draw_filter_button = [this, &g](juce::Rectangle<int> bounds,
                                               const juce::String& label,
                                               bool active,
                                               FilterButtonTarget::Kind kind) {
        filter_button_targets_.push_back(FilterButtonTarget {
            .bounds = bounds,
            .kind = kind,
        });
        g.setColour(active ? juce::Colour::fromRGB(73, 112, 178) : juce::Colour::fromRGB(55, 58, 69));
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
        g.setColour(active ? juce::Colour::fromRGB(238, 242, 248) : juce::Colour::fromRGB(188, 193, 204));
        g.drawRoundedRectangle(bounds.toFloat(), 3.0f, 1.0f);
        g.drawText(label, bounds, juce::Justification::centred, false);
    };

    const int filter_panel_width = 78;
    auto filter_area = juce::Rectangle<int>(header.getX(), opcode_top, filter_panel_width, opcode_row_count * opcode_row_h)
        .reduced(6, 6);
    auto row_label_area = juce::Rectangle<int>(
        header.getX() + filter_panel_width,
        opcode_top,
        header.getWidth() - filter_panel_width,
        opcode_row_count * opcode_row_h);

    const int toggle_w = 18;
    const int toggle_h = 14;
    auto note_filter_row = filter_area.removeFromTop(toggle_h);
    draw_filter_button(note_filter_row.removeFromLeft(toggle_w), "N", show_note_commands_, FilterButtonTarget::Kind::notes);
    note_filter_row.removeFromLeft(4);
    draw_filter_button(note_filter_row.removeFromLeft(toggle_w), "R", show_rest_commands_, FilterButtonTarget::Kind::rests);
    note_filter_row.removeFromLeft(4);
    draw_filter_button(note_filter_row.removeFromLeft(toggle_w), "H", show_hold_commands_, FilterButtonTarget::Kind::holds);
    filter_area.removeFromTop(6);

    const int wide_button_h = 14;
    auto filter_row_one = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_one.removeFromLeft(34), "Inst", show_instrument_commands_, FilterButtonTarget::Kind::instrument);
    filter_row_one.removeFromLeft(4);
    draw_filter_button(filter_row_one.removeFromLeft(34), "Dyn", show_dynamics_commands_, FilterButtonTarget::Kind::dynamics);
    filter_area.removeFromTop(4);
    auto filter_row_two = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_two.removeFromLeft(34), "Pan", show_pan_commands_, FilterButtonTarget::Kind::pan);
    filter_row_two.removeFromLeft(4);
    draw_filter_button(filter_row_two.removeFromLeft(34), "Tmp", show_tempo_commands_, FilterButtonTarget::Kind::tempo);
    filter_area.removeFromTop(4);
    auto filter_row_three = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_three.removeFromLeft(34), "ADSR", show_adsr_commands_, FilterButtonTarget::Kind::adsr);
    filter_row_three.removeFromLeft(4);
    draw_filter_button(filter_row_three.removeFromLeft(34), "Oct", show_octave_commands_, FilterButtonTarget::Kind::octave);
    filter_area.removeFromTop(4);
    auto filter_row_four = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_four.removeFromLeft(34), "LFO", show_lfo_commands_, FilterButtonTarget::Kind::lfo);
    filter_row_four.removeFromLeft(4);
    draw_filter_button(filter_row_four.removeFromLeft(34), "Bnd", show_bend_commands_, FilterButtonTarget::Kind::bend);
    filter_area.removeFromTop(4);
    auto filter_row_five = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_five.removeFromLeft(34), "Det", show_detune_commands_, FilterButtonTarget::Kind::detune);
    filter_row_five.removeFromLeft(4);
    draw_filter_button(filter_row_five.removeFromLeft(34), "Rev", show_reverb_commands_, FilterButtonTarget::Kind::reverb);
    filter_area.removeFromTop(4);
    auto filter_row_six = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_six.removeFromLeft(34), "Slr", show_slur_commands_, FilterButtonTarget::Kind::slur);
    filter_row_six.removeFromLeft(4);
    draw_filter_button(filter_row_six.removeFromLeft(34), "Prc", show_percussion_commands_, FilterButtonTarget::Kind::percussion);
    filter_area.removeFromTop(4);
    auto filter_row_seven = filter_area.removeFromTop(wide_button_h);
    draw_filter_button(filter_row_seven.removeFromLeft(34), "Str", show_structure_commands_, FilterButtonTarget::Kind::structure);

    for (int row = 0; row < opcode_row_count; ++row) {
        const int y = opcode_top + row * opcode_row_h;
        g.setColour((row % 2) == 0 ? juce::Colour::fromRGB(34, 37, 45) : juce::Colour::fromRGB(31, 34, 41));
        g.fillRect(timeline.withY(y).withHeight(opcode_row_h));
        g.setColour(juce::Colour::fromRGB(84, 88, 100));
        g.drawHorizontalLine(y + opcode_row_h, static_cast<float>(timeline.getX()), static_cast<float>(timeline.getRight()));
        g.setColour(juce::Colour::fromRGB(170, 174, 184));
        g.drawText(juce::String(row + 1), row_label_area.getX() + 4, y, row_label_area.getWidth() - 8, opcode_row_h, juce::Justification::centredRight);
    }

    g.saveState();
    g.reduceClipRegion(timeline);

        for (const auto& segment : presentation_.grid_segments) {
            for (int32_t beat_tick = segment.start_tick; beat_tick <= segment.end_tick; beat_tick += segment.ticks_per_beat) {
                const bool is_bar = ((beat_tick - segment.start_tick) % segment.ticks_per_bar) == 0;
                const float x = tick_to_x(beat_tick);
                if (x < static_cast<float>(kHeaderWidth) - 8.0f || x > static_cast<float>(getWidth() + 8)) {
                    continue;
                }
                g.setColour(grid_colour(is_bar));
                g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(grid_top), static_cast<float>(grid_bottom));
            }
        }

    if (const auto* track = selected_track()) {
        if (!track->loop_boundaries.empty()) {
            for (const auto& boundary : track->loop_boundaries) {
                const float x = tick_to_x(boundary.tick);
                if (x < static_cast<float>(kHeaderWidth) - 12.0f || x > static_cast<float>(getWidth() + 12)) {
                    continue;
                }
                const auto colour = loop_boundary_colour(boundary.loop_depth);
                g.setColour(colour.withAlpha(0.30f));
                g.fillRect(juce::Rectangle<int>(
                    static_cast<int>(std::round(x)) - 1,
                    grid_top,
                    3,
                    std::max(0, grid_bottom - grid_top)));
                g.setColour(colour);
                g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(grid_top), static_cast<float>(grid_bottom));
                g.drawText(
                    juce::String(boundary.label),
                    static_cast<int>(std::round(x)) + 4,
                    grid_top + 1,
                    64,
                    12,
                    juce::Justification::left,
                    false);
            }
        }

        const auto is_selected_command = [this, track](const FFTSmdLaneCommandBlock& command) {
            if (track->track_index != selected_command_track_index_) {
                return false;
            }
            if (command.authored_opcode_index >= 0 &&
                std::find(
                    selected_command_authored_opcode_indices_.begin(),
                    selected_command_authored_opcode_indices_.end(),
                    command.authored_opcode_index) != selected_command_authored_opcode_indices_.end()) {
                return true;
            }
            if (command.source_event_index >= 0 &&
                std::find(
                    selected_command_source_event_indices_.begin(),
                    selected_command_source_event_indices_.end(),
                    command.source_event_index) != selected_command_source_event_indices_.end()) {
                return true;
            }
            return track->track_index == selected_command_track_index_ &&
                ((command.authored_opcode_index >= 0 &&
                  command.authored_opcode_index == selected_command_authored_opcode_index_) ||
                 (command.source_event_index >= 0 &&
                  command.source_event_index == selected_command_source_event_index_)) &&
                command.opcode == selected_command_opcode_;
        };

        if (selected_time_end_tick_ > selected_time_start_tick_) {
            const int left = static_cast<int>(std::round(tick_to_x(selected_time_start_tick_)));
            const int right = static_cast<int>(std::round(tick_to_x(selected_time_end_tick_)));
            g.setColour(juce::Colour::fromRGBA(245, 232, 150, 26));
            g.fillRect(juce::Rectangle<int>(left, grid_top, std::max(1, right - left), std::max(0, grid_bottom - grid_top)));
            g.setColour(juce::Colour::fromRGB(245, 232, 150));
            g.drawVerticalLine(left, static_cast<float>(grid_top), static_cast<float>(grid_bottom));
            g.drawVerticalLine(right, static_cast<float>(grid_top), static_cast<float>(grid_bottom));
        }

        for (const auto& diagnostic : group_diagnostics(track->diagnostics)) {
            const int x = static_cast<int>(std::round(tick_to_x(diagnostic.tick)));
            const juce::Rectangle<int> marker_bounds(x - 4, grid_top, 8, std::max(0, grid_bottom - grid_top));
            visible_diagnostic_markers_.push_back(DisplayedDiagnosticMarker {
                .bounds = marker_bounds,
                .tick = diagnostic.tick,
                .severity = diagnostic.severity,
                .message = diagnostic.message,
            });
            g.setColour(diagnostic_colour(diagnostic.severity).withAlpha(0.40f));
            g.drawVerticalLine(x, static_cast<float>(grid_top), static_cast<float>(grid_bottom));
            juce::Path triangle;
            triangle.addTriangle(
                static_cast<float>(x), static_cast<float>(grid_top + 2),
                static_cast<float>(x - 5), static_cast<float>(grid_top + 10),
                static_cast<float>(x + 5), static_cast<float>(grid_top + 10));
            g.setColour(diagnostic_colour(diagnostic.severity));
            g.fillPath(triangle);
        }

        for (const auto& note : track->notes) {
            const int x = static_cast<int>(std::round(tick_to_x(note.start_tick)));
            const int w = std::max(2, static_cast<int>(std::round(note.duration_ticks * pixels_per_tick_)));
            const bool is_rest = note.relative_key == 13;
            const int row = is_rest ? kPitchRowCount : (11 - std::clamp(note.relative_key, 0, 11));
            const int y = note_top + row * kNoteRowHeight + 1;
            const int h = std::max(8, kNoteRowHeight - 2);
            const juce::Rectangle<int> note_bounds(x, y, w, h);
            visible_note_blocks_.push_back(DisplayedNoteBlock {
                .bounds = note_bounds,
                .note = note,
            });
            if (note.source_event_index >= 0) {
                visible_insert_anchors_.push_back(DisplayedInsertAnchor {
                    .bounds = note_bounds,
                    .anchor = FFTSmdLaneInsertAnchor {
                        .tick = note.start_tick,
                        .insertion_sequence_index = note.source_event_index,
                        .source_event_index = note.source_event_index,
                        .opcode = -1,
                        .kind = is_rest ? FFTSmdLaneInsertAnchorKind::rest_start : FFTSmdLaneInsertAnchorKind::note_start,
                        .label = is_rest ? "Rest Start" : "Note Start",
                    },
                });
            }
            if (is_rest) {
                g.setColour(juce::Colour::fromRGB(110, 116, 128));
                g.fillRoundedRectangle(note_bounds.toFloat(), 2.0f);
                if (selected_track_index_ == selected_rest_track_index_ &&
                    note.source_event_index >= 0 &&
                    note.source_event_index == selected_rest_source_event_index_) {
                    g.setColour(juce::Colour::fromRGB(245, 232, 150));
                    g.drawRoundedRectangle(note_bounds.toFloat(), 2.0f, 1.5f);
                }
                continue;
            }

            const int extension_w = std::clamp(
                static_cast<int>(std::round(note.fermata_extension_ticks * pixels_per_tick_)),
                0,
                w);
            const int base_w = std::max(0, w - extension_w);
            if (base_w > 0) {
                g.setColour(juce::Colour::fromRGB(114, 178, 255));
                g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(base_w), static_cast<float>(h)), 2.0f);
            }
            if (extension_w > 0) {
                g.setColour(juce::Colour::fromRGB(255, 196, 92));
                g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(x + base_w), static_cast<float>(y), static_cast<float>(extension_w), static_cast<float>(h)), 2.0f);
            }
            if (base_w == 0 && extension_w == 0) {
                g.setColour(juce::Colour::fromRGB(114, 178, 255));
                g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)), 2.0f);
            }
        }

        if (note_preview_.has_value()) {
            g.setColour(note_preview_->note_relative_key == 13
                ? juce::Colour::fromRGBA(160, 168, 182, 88)
                : juce::Colour::fromRGBA(114, 178, 255, 72));
            g.fillRoundedRectangle(note_preview_->bounds.toFloat(), 2.0f);
            g.setColour(juce::Colour::fromRGB(245, 232, 150));
            g.drawRoundedRectangle(note_preview_->bounds.toFloat(), 2.0f, 1.5f);
        }
        if (fermata_preview_.has_value()) {
            g.setColour(juce::Colour::fromRGBA(255, 196, 92, 88));
            g.fillRoundedRectangle(fermata_preview_->bounds.toFloat(), 2.0f);
            g.setColour(juce::Colour::fromRGB(245, 232, 150));
            g.drawRoundedRectangle(fermata_preview_->bounds.toFloat(), 2.0f, 1.5f);
        }
        if (resize_preview_.has_value()) {
            g.setColour(juce::Colour::fromRGBA(114, 178, 255, 72));
            g.fillRoundedRectangle(resize_preview_->base_bounds.toFloat(), 2.0f);
            if (resize_preview_->extension_ticks > 0 && resize_preview_->extension_bounds.getWidth() > 0) {
                g.setColour(juce::Colour::fromRGBA(255, 196, 92, 88));
                g.fillRoundedRectangle(resize_preview_->extension_bounds.toFloat(), 2.0f);
            }
            g.setColour(juce::Colour::fromRGB(245, 232, 150));
            g.drawRoundedRectangle(resize_preview_->total_bounds.toFloat(), 2.0f, 1.5f);
        }

        g.saveState();
        g.reduceClipRegion(juce::Rectangle<int>(timeline.getX(), opcode_top, timeline.getWidth(), std::max(0, opcode_bottom - opcode_top)));
        g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
        const auto placements = layout_command_chips(
            track->commands,
            [this](const FFTSmdLaneCommandBlock& command) {
                return should_draw_command(command);
            },
            [this](int32_t tick) {
                return tick_to_x(tick);
            },
            juce::Rectangle<int>(timeline.getX(), opcode_top, timeline.getWidth(), std::max(0, opcode_bottom - opcode_top)),
            opcode_row_count,
            g.getCurrentFont());
        for (const auto& placement : placements) {
            const auto& command = track->commands[placement.command_index];
            const auto& chip_bounds = placement.bounds;
            if (command.source_event_index >= 0) {
                visible_insert_anchors_.push_back(DisplayedInsertAnchor {
                    .bounds = chip_bounds,
                    .anchor = FFTSmdLaneInsertAnchor {
                        .tick = command.tick,
                        .insertion_sequence_index = command.source_event_index,
                        .source_event_index = command.source_event_index,
                        .opcode = command.opcode,
                        .kind = FFTSmdLaneInsertAnchorKind::command,
                        .label = "Before " + command.label,
                    },
                });
            }
            visible_command_chips_.push_back(DisplayedCommandChip {
                .bounds = chip_bounds,
                .command = command,
            });
            g.setColour(command_fill_colour(command.kind).withAlpha(command.enabled ? 1.0f : 0.35f));
            g.fillRoundedRectangle(
                chip_bounds.toFloat(),
                3.0f);
            if (is_selected_command(command)) {
                g.setColour(juce::Colour::fromRGB(245, 232, 150));
                g.drawRoundedRectangle(chip_bounds.toFloat(), 3.0f, 1.5f);
            }
            if (command.authored_opcode_index < 0 && command.source_event_index < 0) {
                g.setColour(juce::Colour::fromRGB(255, 84, 196));
                g.drawRoundedRectangle(chip_bounds.toFloat(), 3.0f, 1.5f);
                g.setColour(juce::Colour::fromRGB(255, 84, 196));
                g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
                g.drawText(
                    "NOID",
                    chip_bounds.reduced(3, 1),
                    juce::Justification::topRight,
                    false);
                g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
            }
            if (!command.enabled) {
                g.setColour(juce::Colours::white.withAlpha(0.35f));
                g.drawLine(
                    static_cast<float>(chip_bounds.getX() + 3),
                    static_cast<float>(chip_bounds.getCentreY()),
                    static_cast<float>(chip_bounds.getRight() - 3),
                    static_cast<float>(chip_bounds.getCentreY()),
                    1.5f);
            }
            g.setColour(juce::Colours::white.withAlpha(command.enabled ? 0.92f : 0.55f));
            g.drawText(
                command.label,
                chip_bounds.getX() + 5,
                chip_bounds.getY(),
                chip_bounds.getWidth() - 8,
                chip_bounds.getHeight(),
                juce::Justification::centredLeft,
                false);
        }

        if (insert_preview_anchor_.has_value()) {
            const auto& preview = *insert_preview_anchor_;
            g.setColour(juce::Colour::fromRGBA(242, 244, 248, 34));
            g.fillRoundedRectangle(preview.bounds.toFloat(), 3.0f);
            g.setColour(juce::Colour::fromRGB(245, 232, 150));
            g.drawRoundedRectangle(preview.bounds.toFloat(), 3.0f, 1.5f);
            g.drawLine(
                static_cast<float>(preview.bounds.getX() + 3),
                static_cast<float>(preview.bounds.getCentreY()),
                static_cast<float>(preview.bounds.getRight() - 3),
                static_cast<float>(preview.bounds.getCentreY()),
                2.0f);
        }

        if (hovered_command_tick_ >= 0) {
            const int hover_x = static_cast<int>(std::round(tick_to_x(hovered_command_tick_)));
            g.setColour(juce::Colour::fromRGBA(255, 255, 255, 80));
            g.drawVerticalLine(hover_x, static_cast<float>(grid_top), static_cast<float>(grid_bottom));
        }
        g.restoreState();
    } else {
        g.setColour(juce::Colour::fromRGB(180, 184, 192));
        g.drawText("No focused track data available.", timeline, juce::Justification::centred);
    }

    g.restoreState();

    g.setColour(juce::Colour::fromRGB(50, 54, 64));
    g.drawLine(static_cast<float>(kHeaderWidth), static_cast<float>(bounds.getY()), static_cast<float>(kHeaderWidth), static_cast<float>(bounds.getBottom()));

    if (hovered_diagnostic_marker_.has_value()) {
        auto tooltip_bounds = juce::Rectangle<int>(
            juce::roundToInt(last_mouse_position_.x) + 12,
            juce::roundToInt(last_mouse_position_.y) + 12,
            std::min(360, std::max(200, getWidth() / 3)),
            72);
        if (tooltip_bounds.getRight() > getWidth() - 8) {
            tooltip_bounds.setX(std::max(8, getWidth() - tooltip_bounds.getWidth() - 8));
        }
        if (tooltip_bounds.getBottom() > getHeight() - 8) {
            tooltip_bounds.setY(std::max(8, getHeight() - tooltip_bounds.getHeight() - 8));
        }

        g.setColour(juce::Colour::fromRGBA(18, 20, 25, 236));
        g.fillRoundedRectangle(tooltip_bounds.toFloat(), 6.0f);
        g.setColour(diagnostic_colour(hovered_diagnostic_marker_->severity));
        g.drawRoundedRectangle(tooltip_bounds.toFloat(), 6.0f, 1.2f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
        g.drawFittedText(
            hovered_diagnostic_marker_->message,
            tooltip_bounds.reduced(8),
            juce::Justification::topLeft,
            6);
    }
}

void FFTPianoDetailView::clear_insert_preview() {
    if (insert_preview_anchor_.has_value()) {
        insert_preview_anchor_.reset();
        repaint();
    }
}

int FFTPianoDetailView::pitch_row_for_y(float y) const {
    const int note_top = 4;
    const int row = (juce::roundToInt(y) - note_top) / kNoteRowHeight;
    if (row < 0 || row >= kPitchRowCount) {
        return -1;
    }
    return row;
}

const FFTPianoDetailView::DisplayedNoteBlock* FFTPianoDetailView::note_block_at(
    const juce::Point<float>& position,
    bool include_rests
) const {
    const int px = juce::roundToInt(position.x);
    const int py = juce::roundToInt(position.y);
    for (const auto& block : visible_note_blocks_) {
        if (!block.bounds.contains(px, py)) {
            continue;
        }
        if (!include_rests && block.note.relative_key == 13) {
            continue;
        }
        return &block;
    }
    return nullptr;
}

const FFTSmdGridSegment* FFTPianoDetailView::grid_segment_for_tick(int32_t tick) const {
    if (presentation_.grid_segments.empty()) {
        return nullptr;
    }
    for (const auto& segment : presentation_.grid_segments) {
        if (tick >= segment.start_tick && tick < segment.end_tick) {
            return &segment;
        }
    }
    if (tick < presentation_.grid_segments.front().start_tick) {
        return &presentation_.grid_segments.front();
    }
    return &presentation_.grid_segments.back();
}

int32_t FFTPianoDetailView::snap_step_for_tick(int32_t tick) const {
    const auto* segment = grid_segment_for_tick(tick);
    if (segment == nullptr) {
        return 1;
    }
    switch (note_snap_mode_index_) {
    case 0: return std::max(1, segment->ticks_per_bar);
    case 1: return std::max(1, segment->ticks_per_beat);
    case 2: return std::max(1, segment->ticks_per_beat / 2);
    case 3: return std::max(1, segment->ticks_per_beat / 4);
    case 4: return std::max(1, segment->ticks_per_beat / 8);
    case 5: return std::max(1, segment->ticks_per_beat / 12);
    case 6: return std::max(1, segment->ticks_per_beat / 16);
    case 7: return std::max(1, segment->ticks_per_beat / 24);
    case 8:
    default:
        return 1;
    }
}

int32_t FFTPianoDetailView::snap_tick_to_grid(int32_t raw_tick) const {
    const auto* segment = grid_segment_for_tick(raw_tick);
    if (segment == nullptr) {
        return std::max(0, raw_tick);
    }
    const int32_t step = snap_step_for_tick(raw_tick);
    const int32_t origin = segment->start_tick;
    const int32_t clamped_tick = std::max(origin, raw_tick);
    const int32_t delta = clamped_tick - origin;
    int32_t snapped = origin;
    if (step > 0) {
        const int32_t rounded_steps = (delta + (step / 2)) / step;
        snapped = origin + rounded_steps * step;
    }

    int32_t resolved = std::max(0, std::clamp(snapped, segment->start_tick, segment->end_tick));
    if (const auto* track = selected_track()) {
        int32_t best_tick = resolved;
        int32_t best_distance = std::abs(raw_tick - resolved);
        for (const auto& boundary : track->loop_boundaries) {
            const int32_t distance = std::abs(raw_tick - boundary.tick);
            if (distance < best_distance) {
                best_distance = distance;
                best_tick = boundary.tick;
            }
        }
        resolved = best_tick;
    }
    return resolved;
}

std::optional<FFTPianoDetailView::NoteDraftPreview> FFTPianoDetailView::build_note_preview(
    const juce::Point<float>& position,
    std::optional<int32_t> forced_relative_key
) const {
    const auto* track = selected_track();
    if (track == nullptr) {
        return std::nullopt;
    }

    const bool forced_rest = forced_relative_key.has_value() && *forced_relative_key == 13;
    const int row = forced_rest ? kPitchRowCount : pitch_row_for_y(position.y);
    if (row < 0 && !forced_rest) {
        return std::nullopt;
    }

    const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
    const int32_t snapped_start = snap_tick_to_grid(raw_tick);
    if (track->polyphonic_notes) {
        const int32_t default_duration = std::max(1, snap_step_for_tick(snapped_start));
        const int32_t end_tick = std::min(track->total_ticks, snapped_start + default_duration);
        if (end_tick <= snapped_start) {
            return std::nullopt;
        }

        const int note_top = 4;
        const int y = note_top + row * kNoteRowHeight + 1;
        const int h = std::max(8, kNoteRowHeight - 2);
        const int x = static_cast<int>(std::round(tick_to_x(snapped_start)));
        const int w = std::max(2, static_cast<int>(std::round((end_tick - snapped_start) * pixels_per_tick_)));
        return NoteDraftPreview {
            .source_event_index = -1,
            .covered_start_tick = 0,
            .covered_duration_ticks = track->total_ticks,
            .note_relative_key = forced_relative_key.value_or(11 - row),
            .start_tick = snapped_start,
            .duration_ticks = end_tick - snapped_start,
            .bounds = juce::Rectangle<int>(x, y, w, h),
        };
    }

    const auto covered_it = std::find_if(
        track->notes.begin(),
        track->notes.end(),
        [snapped_start](const FFTSmdLaneNoteBlock& note) {
            return note.source_event_index >= 0 &&
                snapped_start >= note.start_tick &&
                snapped_start < note.start_tick + note.duration_ticks;
        });
    if (covered_it == track->notes.end()) {
        return std::nullopt;
    }

    const int32_t rest_end_tick = covered_it->start_tick + covered_it->duration_ticks;
    const int32_t default_duration = std::max(1, snap_step_for_tick(snapped_start));
    const int32_t end_tick = std::min(rest_end_tick, snapped_start + default_duration);
    if (end_tick <= snapped_start) {
        return std::nullopt;
    }

    const int note_top = 4;
    const int y = note_top + row * kNoteRowHeight + 1;
    const int h = std::max(8, kNoteRowHeight - 2);
    const int x = static_cast<int>(std::round(tick_to_x(snapped_start)));
    const int w = std::max(2, static_cast<int>(std::round((end_tick - snapped_start) * pixels_per_tick_)));
    return NoteDraftPreview {
        .source_event_index = covered_it->source_event_index,
        .covered_start_tick = covered_it->start_tick,
        .covered_duration_ticks = covered_it->duration_ticks,
        .note_relative_key = forced_relative_key.value_or(11 - row),
        .start_tick = snapped_start,
        .duration_ticks = end_tick - snapped_start,
        .bounds = juce::Rectangle<int>(x, y, w, h),
    };
}

void FFTPianoDetailView::update_note_preview(const juce::Point<float>& position) {
    std::optional<NoteDraftPreview> next_preview;
    if (active_note_draft_.has_value()) {
        next_preview = active_note_draft_;
        const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
        int32_t snapped_end = snap_tick_to_grid(raw_tick);
        const int32_t min_end = std::min(
            active_note_draft_->covered_start_tick + active_note_draft_->covered_duration_ticks,
            active_note_draft_->start_tick + std::max(1, snap_step_for_tick(active_note_draft_->start_tick)));
        if (snapped_end < min_end) {
            snapped_end = min_end;
        }
        int32_t max_end = active_note_draft_->covered_start_tick + active_note_draft_->covered_duration_ticks;
        if (const auto* track = selected_track()) {
            max_end = std::max(max_end, track->total_ticks);
        }
        snapped_end = std::clamp(snapped_end, active_note_draft_->start_tick + 1, max_end);
        next_preview->duration_ticks = std::max(1, snapped_end - active_note_draft_->start_tick);
        next_preview->bounds.setWidth(std::max(
            2,
            static_cast<int>(std::round(next_preview->duration_ticks * pixels_per_tick_))));
    } else {
        next_preview = build_note_preview(position);
    }

    if (next_preview.has_value() != note_preview_.has_value() ||
        (next_preview.has_value() &&
         (next_preview->source_event_index != note_preview_->source_event_index ||
          next_preview->start_tick != note_preview_->start_tick ||
          next_preview->duration_ticks != note_preview_->duration_ticks ||
          next_preview->note_relative_key != note_preview_->note_relative_key))) {
        note_preview_ = std::move(next_preview);
        repaint();
    }
}

void FFTPianoDetailView::clear_note_preview() {
    if (note_preview_.has_value()) {
        note_preview_.reset();
        repaint();
    }
}

std::optional<FFTPianoDetailView::FermataDraftPreview> FFTPianoDetailView::build_fermata_preview(
    const juce::Point<float>& position
) const {
    const auto* note_block = note_block_at(position, false);
    if (note_block == nullptr || note_block->note.source_event_index < 0) {
        return std::nullopt;
    }

    const int32_t base_duration = std::max(1, note_block->note.duration_ticks - note_block->note.fermata_extension_ticks);
    const int32_t base_end_tick = note_block->note.start_tick + base_duration;
    const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
    int32_t snapped_end = snap_tick_to_grid(raw_tick);
    const int32_t min_end = base_end_tick + std::max(1, snap_step_for_tick(base_end_tick));
    if (snapped_end < min_end) {
        snapped_end = min_end;
    }
    if (const auto* track = selected_track()) {
        snapped_end = std::min(snapped_end, std::max(base_end_tick + 1, track->total_ticks));
    }

    const int extension_ticks = std::max(1, snapped_end - base_end_tick);
    const int row = 11 - std::clamp(note_block->note.relative_key, 0, 11);
    const int note_top = 4;
    const int y = note_top + row * kNoteRowHeight + 1;
    const int h = std::max(8, kNoteRowHeight - 2);
    const int x = static_cast<int>(std::round(tick_to_x(base_end_tick)));
    const int w = std::max(2, static_cast<int>(std::round(extension_ticks * pixels_per_tick_)));
    return FermataDraftPreview {
        .source_event_index = note_block->note.source_event_index,
        .note_start_tick = note_block->note.start_tick,
        .note_base_duration_ticks = base_duration,
        .extension_ticks = extension_ticks,
        .bounds = juce::Rectangle<int>(x, y, w, h),
    };
}

void FFTPianoDetailView::update_fermata_preview(const juce::Point<float>& position) {
    std::optional<FermataDraftPreview> next_preview;
    if (active_fermata_draft_.has_value()) {
        next_preview = active_fermata_draft_;
        const int32_t base_end_tick =
            active_fermata_draft_->note_start_tick + active_fermata_draft_->note_base_duration_ticks;
        const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
        int32_t snapped_end = snap_tick_to_grid(raw_tick);
        const int32_t min_end = base_end_tick + std::max(1, snap_step_for_tick(base_end_tick));
        if (snapped_end < min_end) {
            snapped_end = min_end;
        }
        if (const auto* track = selected_track()) {
            snapped_end = std::min(snapped_end, std::max(base_end_tick + 1, track->total_ticks));
        }
        next_preview->extension_ticks = std::max(1, snapped_end - base_end_tick);
        next_preview->bounds.setWidth(std::max(
            2,
            static_cast<int>(std::round(next_preview->extension_ticks * pixels_per_tick_))));
    } else {
        next_preview = build_fermata_preview(position);
    }

    if (next_preview.has_value() != fermata_preview_.has_value() ||
        (next_preview.has_value() &&
         (next_preview->source_event_index != fermata_preview_->source_event_index ||
          next_preview->extension_ticks != fermata_preview_->extension_ticks))) {
        fermata_preview_ = std::move(next_preview);
        repaint();
    }
}

void FFTPianoDetailView::clear_fermata_preview() {
    if (fermata_preview_.has_value()) {
        fermata_preview_.reset();
        repaint();
    }
}

std::optional<FFTPianoDetailView::ResizeDraftPreview> FFTPianoDetailView::build_resize_preview(
    const juce::Point<float>& position
) const {
    const auto* note_block = note_block_at(position, false);
    if (note_block == nullptr || note_block->note.source_event_index < 0) {
        return std::nullopt;
    }

    const int total_w = note_block->bounds.getWidth();
    const int extension_w = std::clamp(
        static_cast<int>(std::round(note_block->note.fermata_extension_ticks * pixels_per_tick_)),
        0,
        total_w);
    const int base_w = std::max(0, total_w - extension_w);
    const int note_x = note_block->bounds.getX();
    const int seam_x = note_x + base_w;
    const int total_end_x = note_x + total_w;
    const int base_duration = std::max(1, note_block->note.duration_ticks - note_block->note.fermata_extension_ticks);
    const int extension_duration = std::max(0, note_block->note.fermata_extension_ticks);

    ResizeDraftPreview::Handle handle;
    const int mouse_x = juce::roundToInt(position.x);
    if (std::abs(mouse_x - note_x) <= kResizeHandleRadiusPx) {
        handle = ResizeDraftPreview::Handle::start;
    } else if (extension_duration > 0 && std::abs(mouse_x - seam_x) <= kResizeHandleRadiusPx) {
        handle = ResizeDraftPreview::Handle::note_end;
    } else if (std::abs(mouse_x - total_end_x) <= kResizeHandleRadiusPx) {
        handle = ResizeDraftPreview::Handle::total_end;
    } else {
        return std::nullopt;
    }

    return ResizeDraftPreview {
        .source_event_index = note_block->note.source_event_index,
        .authored_start_tick = note_block->note.authored_start_tick,
        .note_relative_key = note_block->note.relative_key,
        .original_start_tick = note_block->note.start_tick,
        .original_base_duration_ticks = base_duration,
        .original_extension_ticks = extension_duration,
        .start_tick = note_block->note.start_tick,
        .base_duration_ticks = base_duration,
        .extension_ticks = extension_duration,
        .handle = handle,
        .total_bounds = note_block->bounds,
        .base_bounds = juce::Rectangle<int>(
            note_block->bounds.getX(),
            note_block->bounds.getY(),
            std::max(2, base_w),
            note_block->bounds.getHeight()),
        .extension_bounds = juce::Rectangle<int>(
            seam_x,
            note_block->bounds.getY(),
            std::max(0, extension_w),
            note_block->bounds.getHeight()),
    };
}

void FFTPianoDetailView::update_resize_preview(const juce::Point<float>& position) {
    std::optional<ResizeDraftPreview> next_preview;
    if (active_resize_draft_.has_value()) {
        next_preview = active_resize_draft_;
        const auto* track = selected_track();
        if (track == nullptr) {
            next_preview.reset();
        } else {
            const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
            const int32_t snapped_tick = snap_tick_to_grid(raw_tick);
            const int32_t original_base_end =
                active_resize_draft_->original_start_tick + active_resize_draft_->original_base_duration_ticks;
            const int32_t original_total_end = original_base_end + active_resize_draft_->original_extension_ticks;

            switch (active_resize_draft_->handle) {
            case ResizeDraftPreview::Handle::start: {
                const int32_t max_start = original_base_end - 1;
                next_preview->start_tick = std::clamp(snapped_tick, 0, max_start);
                next_preview->base_duration_ticks = std::max(1, original_base_end - next_preview->start_tick);
                next_preview->extension_ticks = active_resize_draft_->original_extension_ticks;
                break;
            }
            case ResizeDraftPreview::Handle::note_end: {
                const int32_t fixed_total_end = original_total_end;
                const int32_t min_note_end = next_preview->start_tick + 1;
                const int32_t max_note_end = fixed_total_end;
                const int32_t new_note_end = std::clamp(snapped_tick, min_note_end, max_note_end);
                next_preview->base_duration_ticks = std::max(1, new_note_end - next_preview->start_tick);
                next_preview->extension_ticks = std::max(0, fixed_total_end - new_note_end);
                break;
            }
            case ResizeDraftPreview::Handle::total_end: {
                const bool had_fermata = active_resize_draft_->original_extension_ticks > 0;
                const int32_t fixed_base_end =
                    next_preview->start_tick + active_resize_draft_->original_base_duration_ticks;
                const int32_t min_total_end = had_fermata
                    ? fixed_base_end
                    : (next_preview->start_tick + 1);
                const int32_t max_total_end = track->total_ticks;
                const int32_t new_total_end = std::clamp(snapped_tick, min_total_end, max_total_end);
                if (had_fermata) {
                    next_preview->base_duration_ticks = active_resize_draft_->original_base_duration_ticks;
                    next_preview->extension_ticks = std::max(0, new_total_end - fixed_base_end);
                } else {
                    next_preview->base_duration_ticks = std::max(1, new_total_end - next_preview->start_tick);
                    next_preview->extension_ticks = 0;
                }
                break;
            }
            }

            const int total_duration = next_preview->base_duration_ticks + next_preview->extension_ticks;
            const int x = static_cast<int>(std::round(tick_to_x(next_preview->start_tick)));
            const int total_w = std::max(2, static_cast<int>(std::round(total_duration * pixels_per_tick_)));
            const int base_w = std::max(2, static_cast<int>(std::round(next_preview->base_duration_ticks * pixels_per_tick_)));
            const int ext_w = std::max(0, total_w - base_w);
            const int y = active_resize_draft_->total_bounds.getY();
            const int h = active_resize_draft_->total_bounds.getHeight();
            next_preview->total_bounds = juce::Rectangle<int>(x, y, total_w, h);
            next_preview->base_bounds = juce::Rectangle<int>(x, y, std::min(base_w, total_w), h);
            next_preview->extension_bounds = juce::Rectangle<int>(x + next_preview->base_bounds.getWidth(), y, ext_w, h);
        }
    } else {
        next_preview = build_resize_preview(position);
    }

    if (next_preview.has_value() != resize_preview_.has_value() ||
        (next_preview.has_value() &&
         (next_preview->source_event_index != resize_preview_->source_event_index ||
          next_preview->start_tick != resize_preview_->start_tick ||
          next_preview->base_duration_ticks != resize_preview_->base_duration_ticks ||
          next_preview->extension_ticks != resize_preview_->extension_ticks ||
          next_preview->handle != resize_preview_->handle))) {
        resize_preview_ = std::move(next_preview);
        repaint();
    }
}

void FFTPianoDetailView::clear_resize_preview() {
    if (resize_preview_.has_value()) {
        resize_preview_.reset();
        repaint();
    }
}

bool FFTPianoDetailView::update_insert_preview(const juce::Point<float>& position, const juce::ModifierKeys& mods) {
    const bool was_insert_mode_active = insert_mode_active_;
    if (!mods.isShiftDown() && !active_command_drag_.has_value()) {
        insert_mode_active_ = false;
        if (was_insert_mode_active != insert_mode_active_) {
            repaint();
        }
        clear_insert_preview();
        return false;
    }

    insert_mode_active_ = true;
    if (was_insert_mode_active != insert_mode_active_) {
        repaint();
    }

    const auto* track = selected_track();
    if (track == nullptr) {
        clear_insert_preview();
        return false;
    }

    auto bounds = getLocalBounds();
    bounds.removeFromLeft(kHeaderWidth);
    auto timeline = bounds.reduced(kTimelinePadding, 0);
    const int note_top = timeline.getY() + 4;
    const int note_bottom = note_top + (kTotalRowCount * kNoteRowHeight);
    const int opcode_top = note_bottom + kSectionGap;
    const int opcode_bottom = timeline.getBottom() - 4;
    const int opcode_available_height = std::max(0, opcode_bottom - opcode_top);
    const int opcode_row_count = std::max(1, opcode_available_height / kOpcodeRowMinHeight);
    const int opcode_row_h = opcode_row_count > 0
        ? std::max(kOpcodeRowMinHeight, opcode_available_height / opcode_row_count)
        : kOpcodeRowMinHeight;

    const int py = juce::roundToInt(position.y);
    if (py < opcode_top || py >= opcode_bottom) {
        insert_mode_active_ = false;
        if (was_insert_mode_active != insert_mode_active_) {
            repaint();
        }
        clear_insert_preview();
        return false;
    }

    const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
    const int32_t requested_tick = round_tick_to_grid_segment(
        raw_tick,
        grid_segment_for_tick(raw_tick),
        snap_step_for_tick(raw_tick));

    int preview_row = 0;
    preview_row = std::clamp((py - opcode_top) / opcode_row_h, 0, opcode_row_count - 1);

    const auto is_drag_excluded_command = [this](const FFTSmdLaneCommandBlock& command) {
        if (!active_command_drag_.has_value()) {
            return false;
        }
        if (selected_track_index_ == selected_command_track_index_) {
            if (command.authored_opcode_index >= 0 &&
                !selected_command_authored_opcode_indices_.empty()) {
                return std::find(
                           selected_command_authored_opcode_indices_.begin(),
                           selected_command_authored_opcode_indices_.end(),
                           command.authored_opcode_index) != selected_command_authored_opcode_indices_.end();
            }
            if (command.source_event_index >= 0 && !selected_command_source_event_indices_.empty()) {
                return std::find(
                           selected_command_source_event_indices_.begin(),
                           selected_command_source_event_indices_.end(),
                           command.source_event_index) != selected_command_source_event_indices_.end();
            }
        }
        if (command.authored_opcode_index >= 0 && active_command_drag_->command.authored_opcode_index >= 0) {
            return command.authored_opcode_index == active_command_drag_->command.authored_opcode_index;
        }
        if (command.source_event_index < 0) {
            return false;
        }
        return command.source_event_index == active_command_drag_->command.source_event_index;
    };

    const auto is_drag_excluded_anchor = [this, &is_drag_excluded_command](const FFTSmdLaneInsertAnchor& anchor) {
        if (!active_command_drag_.has_value()) {
            return false;
        }
        if (anchor.authored_opcode_index >= 0 &&
            selected_track_index_ == selected_command_track_index_ &&
            !selected_command_authored_opcode_indices_.empty()) {
            return std::find(
                       selected_command_authored_opcode_indices_.begin(),
                       selected_command_authored_opcode_indices_.end(),
                       anchor.authored_opcode_index) != selected_command_authored_opcode_indices_.end();
        }
        if (anchor.source_event_index >= 0 &&
            selected_track_index_ == selected_command_track_index_ &&
            !selected_command_source_event_indices_.empty()) {
            return std::find(
                       selected_command_source_event_indices_.begin(),
                       selected_command_source_event_indices_.end(),
                       anchor.source_event_index) != selected_command_source_event_indices_.end();
        }
        return is_drag_excluded_command(FFTSmdLaneCommandBlock {
            .tick = anchor.tick,
            .authored_opcode_index = anchor.authored_opcode_index,
            .source_event_index = anchor.source_event_index,
        });
    };

    std::vector<const DisplayedInsertAnchor*> same_tick_command_slots;
    same_tick_command_slots.reserve(visible_insert_anchors_.size());
    for (const auto& anchor : visible_insert_anchors_) {
        if (anchor.anchor.tick != requested_tick) {
            continue;
        }
        if (anchor.anchor.kind != FFTSmdLaneInsertAnchorKind::command) {
            continue;
        }
        if (is_drag_excluded_anchor(anchor.anchor)) {
            continue;
        }
        same_tick_command_slots.push_back(&anchor);
    }
    std::sort(
        same_tick_command_slots.begin(),
        same_tick_command_slots.end(),
        [](const DisplayedInsertAnchor* lhs, const DisplayedInsertAnchor* rhs) {
            if (lhs->bounds.getY() != rhs->bounds.getY()) {
                return lhs->bounds.getY() < rhs->bounds.getY();
            }
            return lhs->anchor.insertion_sequence_index < rhs->anchor.insertion_sequence_index;
        });

    int insertion_sequence_index = 0;
    int max_source_event_index = -1;
    bool found_future_event = false;
    for (const auto& command : track->commands) {
        if (command.source_event_index < 0 && command.authored_opcode_index < 0) {
            continue;
        }
        if (is_drag_excluded_command(command)) {
            continue;
        }
        max_source_event_index = std::max(max_source_event_index, command.source_event_index);
        if (!found_future_event && command.tick >= requested_tick) {
            insertion_sequence_index = command.source_event_index;
            found_future_event = true;
        }
    }
    if (!found_future_event) {
        insertion_sequence_index = max_source_event_index + 1;
    }

    if (!same_tick_command_slots.empty()) {
        insertion_sequence_index = same_tick_command_slots.back()->anchor.insertion_sequence_index + 1;
        for (const auto* slot : same_tick_command_slots) {
            const int slot_row = std::max(0, (slot->bounds.getY() - opcode_top) / opcode_row_h);
            if (preview_row <= slot_row) {
                insertion_sequence_index = slot->anchor.insertion_sequence_index;
                preview_row = slot_row;
                break;
            }
        }
    }
    const int preview_x = static_cast<int>(std::round(tick_to_x(requested_tick)));
    const int preview_y = opcode_top + preview_row * opcode_row_h + 2;
    const int preview_h = std::max(12, opcode_row_h - 4);
    const int preview_w = 44;
    insert_preview_anchor_ = DisplayedInsertAnchor {
        .bounds = juce::Rectangle<int>(preview_x, preview_y, preview_w, preview_h),
        .anchor = FFTSmdLaneInsertAnchor {
            .tick = requested_tick,
            .insertion_sequence_index = insertion_sequence_index,
            .source_event_index = -1,
            .opcode = -1,
            .kind = FFTSmdLaneInsertAnchorKind::measure,
            .label = "Tick " + std::to_string(requested_tick),
        },
    };
    repaint();
    return true;
}

bool FFTPianoDetailView::try_handle_insert_anchor_click(const juce::MouseEvent& event) {
    if (!event.mods.isShiftDown() || event.mods.isRightButtonDown() || insert_anchor_callback_ == nullptr) {
        return false;
    }
    if (update_insert_preview(event.position, event.mods) && insert_preview_anchor_.has_value()) {
        insert_anchor_callback_(selected_track_index_, insert_preview_anchor_->anchor);
        return true;
    }
    return false;
}

void FFTPianoDetailView::mouseDown(const juce::MouseEvent& event) {
    const int mouse_x = juce::roundToInt(event.position.x);
    const int mouse_y = juce::roundToInt(event.position.y);
    for (const auto& filter_button : filter_button_targets_) {
        if (filter_button.bounds.contains(mouse_x, mouse_y)) {
            toggle_filter_button(filter_button.kind);
            return;
        }
    }
    if (event.mods.isCtrlDown() &&
        !event.mods.isShiftDown() &&
        !event.mods.isRightButtonDown() &&
        !event.mods.isMiddleButtonDown()) {
        update_resize_preview(event.position);
        if (resize_preview_.has_value()) {
            active_resize_draft_ = resize_preview_;
            clear_note_preview();
            clear_fermata_preview();
            return;
        }
    }
    if (event.mods.isShiftDown() && event.mods.isRightButtonDown()) {
        if (const auto* block = note_block_at(event.position, false)) {
            if (block->note.fermata_extension_ticks > 0) {
                const int note_x = block->bounds.getX();
                const int total_w = block->bounds.getWidth();
                const int extension_w = std::clamp(
                    static_cast<int>(std::round(block->note.fermata_extension_ticks * pixels_per_tick_)),
                    0,
                    total_w);
                const int base_w = std::max(0, total_w - extension_w);
                if (mouse_x >= note_x + base_w) {
                    if (note_fermata_delete_callback_) {
                        note_fermata_delete_callback_(selected_track_index_, block->note.source_event_index);
                    } else if (note_fermata_callback_) {
                        note_fermata_callback_(selected_track_index_, block->note.source_event_index, 0);
                    }
                    return;
                }
            }
        }
        for (const auto& anchor : visible_insert_anchors_) {
            if (anchor.anchor.kind == FFTSmdLaneInsertAnchorKind::note_start &&
                anchor.anchor.source_event_index >= 0 &&
                anchor.bounds.contains(mouse_x, mouse_y)) {
                if (note_delete_callback_) {
                    note_delete_callback_(selected_track_index_, anchor.anchor.source_event_index);
                }
                return;
            }
        }
    }
    if (event.mods.isShiftDown() &&
        !event.mods.isRightButtonDown() &&
        !event.mods.isMiddleButtonDown()) {
        update_fermata_preview(event.position);
        if (fermata_preview_.has_value()) {
            active_fermata_draft_ = fermata_preview_;
            return;
        }
    }
    if (event.mods.isAltDown() &&
        !event.mods.isCtrlDown() &&
        !event.mods.isShiftDown() &&
        !event.mods.isRightButtonDown() &&
        !event.mods.isMiddleButtonDown()) {
        if (const auto* block = note_block_at(event.position, false);
            block != nullptr &&
            block->note.relative_key >= 0 &&
            block->note.relative_key < 12 &&
            note_match_lab_callback_ != nullptr) {
            note_match_lab_callback_(selected_track_index_, block->note.source_event_index);
            return;
        }
    }
    if (!event.mods.isCtrlDown() &&
        !event.mods.isShiftDown() &&
        !event.mods.isRightButtonDown() &&
        !event.mods.isMiddleButtonDown()) {
        if (const auto* block = note_block_at(event.position, true);
            block != nullptr && block->note.relative_key == 13) {
            set_selected_rest(
                selected_track_index_,
                block->note.source_event_index,
                block->note.authored_span_index);
            if (rest_selected_callback_) {
                rest_selected_callback_(
                    selected_track_index_,
                    block->note.source_event_index,
                    block->note.authored_span_index);
            }
            clear_note_preview();
            clear_fermata_preview();
            clear_resize_preview();
            return;
        }
    }
    if (!event.mods.isCtrlDown() &&
        !event.mods.isShiftDown() &&
        !event.mods.isMiddleButtonDown()) {
        if (event.mods.isRightButtonDown()) {
            auto rest_preview = build_note_preview(event.position, 13);
            if (rest_preview.has_value()) {
                note_preview_ = rest_preview;
                active_note_draft_ = note_preview_;
                repaint();
                return;
            }
        } else if (note_preview_.has_value()) {
            active_note_draft_ = note_preview_;
            clear_fermata_preview();
            return;
        }
    }
    if (try_handle_insert_anchor_click(event)) {
        return;
    }
    if (!event.mods.isMiddleButtonDown()) {
        const auto dispatch_chip_click = [this, &event](const DisplayedCommandChip& chip) {
            if (event.mods.isCtrlDown() &&
                !event.mods.isShiftDown() &&
                !event.mods.isRightButtonDown() &&
                !event.mods.isMiddleButtonDown()) {
                pending_ctrl_command_click_ = PendingCtrlCommandClick {
                    .command = chip.command,
                    .mouse_down_position = event.position,
                };
                return true;
            }
            if (event.mods.isShiftDown() && event.mods.isRightButtonDown()) {
                if (command_delete_callback_ &&
                    (chip.command.authored_opcode_index >= 0 || chip.command.source_event_index >= 0)) {
                    command_delete_callback_(selected_track_index_, chip.command);
                    return true;
                }
                return false;
            }
            if (event.mods.isRightButtonDown()) {
                if (command_secondary_click_callback_) {
                    command_secondary_click_callback_(selected_track_index_, chip.command);
                    return true;
                }
                return false;
            }
            if (command_selected_callback_) {
                command_selected_callback_(selected_track_index_, chip.command);
                return true;
            }
            return false;
        };
        for (const auto& chip : visible_command_chips_) {
            if (chip.bounds.contains(mouse_x, mouse_y) &&
                dispatch_chip_click(chip)) {
                return;
            }
        }
    }
    if (event.mods.isMiddleButtonDown() && pan_callback_) {
        middle_drag_panning_ = true;
        pan_start_position_ = event.mouseDownPosition;
    }
}

void FFTPianoDetailView::mouseDrag(const juce::MouseEvent& event) {
    if (pending_ctrl_command_click_.has_value() &&
        (event.position - pending_ctrl_command_click_->mouse_down_position).getDistanceFromOrigin() > 3.0f) {
        active_command_drag_ = DraggedCommandChip {.command = pending_ctrl_command_click_->command};
        pending_ctrl_command_click_.reset();
        update_insert_preview(event.position, event.mods);
        return;
    }
    if (active_command_drag_.has_value()) {
        update_insert_preview(event.position, event.mods);
        return;
    }
    if (active_resize_draft_.has_value()) {
        update_resize_preview(event.position);
        return;
    }
    if (active_note_draft_.has_value()) {
        update_note_preview(event.position);
        return;
    }
    if (active_fermata_draft_.has_value()) {
        update_fermata_preview(event.position);
        return;
    }
    if (middle_drag_panning_ && pan_callback_) {
        const auto delta = event.position - pan_start_position_;
        pan_callback_(-static_cast<int>(std::round(delta.x)), -static_cast<int>(std::round(delta.y)));
        pan_start_position_ = event.position;
        return;
    }
    juce::Component::mouseDrag(event);
}

void FFTPianoDetailView::mouseMove(const juce::MouseEvent& event) {
    last_mouse_position_ = event.position;
    if (active_command_drag_.has_value()) {
        update_insert_preview(event.position, event.mods);
        return;
    }
    update_insert_preview(event.position, event.mods);
    if (event.mods.isCtrlDown() && !event.mods.isShiftDown()) {
        update_resize_preview(event.position);
        clear_note_preview();
        clear_fermata_preview();
    } else if (!event.mods.isShiftDown()) {
        update_note_preview(event.position);
        if (resize_preview_.has_value()) {
            clear_resize_preview();
        }
        if (fermata_preview_.has_value()) {
            clear_fermata_preview();
        }
    } else {
        update_fermata_preview(event.position);
        if (resize_preview_.has_value()) {
            clear_resize_preview();
        }
        if (note_preview_.has_value()) {
            clear_note_preview();
        }
    }
    if (event.mods.isShiftDown() && note_preview_.has_value()) {
        clear_note_preview();
    }

    int32_t new_hover_tick = -1;
    if (const auto* track = selected_track()) {
        if (event.position.x >= static_cast<float>(kHeaderWidth)) {
            const float hover_x = event.position.x;
            float best_distance = static_cast<float>(kHoverTickRadiusPx) + 1.0f;
            for (const auto& command : track->commands) {
                const float command_x = tick_to_x(command.tick);
                const float distance = std::abs(command_x - hover_x);
                if (distance <= static_cast<float>(kHoverTickRadiusPx) && distance < best_distance) {
                    best_distance = distance;
                    new_hover_tick = command.tick;
                }
            }
        }
    }

    if (new_hover_tick != hovered_command_tick_) {
        hovered_command_tick_ = new_hover_tick;
        repaint();
    }

    std::optional<DisplayedDiagnosticMarker> new_hovered_diagnostic;
    for (const auto& marker : visible_diagnostic_markers_) {
        if (marker.bounds.contains(event.getPosition())) {
            new_hovered_diagnostic = marker;
            break;
        }
    }
    if (new_hovered_diagnostic.has_value() != hovered_diagnostic_marker_.has_value() ||
        (new_hovered_diagnostic.has_value() &&
         hovered_diagnostic_marker_.has_value() &&
         new_hovered_diagnostic->tick != hovered_diagnostic_marker_->tick)) {
        hovered_diagnostic_marker_ = std::move(new_hovered_diagnostic);
        repaint();
    }
}

void FFTPianoDetailView::mouseExit(const juce::MouseEvent& event) {
    juce::ignoreUnused(event);
    if (active_command_drag_.has_value()) {
        return;
    }
    pending_ctrl_command_click_.reset();
    insert_mode_active_ = false;
    clear_insert_preview();
    if (!active_resize_draft_.has_value()) {
        clear_resize_preview();
    }
    if (!active_note_draft_.has_value()) {
        clear_note_preview();
    }
    if (!active_fermata_draft_.has_value()) {
        clear_fermata_preview();
    }
    if (hovered_command_tick_ != -1) {
        hovered_command_tick_ = -1;
        repaint();
    }
    if (hovered_diagnostic_marker_.has_value()) {
        hovered_diagnostic_marker_.reset();
        repaint();
    }
}

void FFTPianoDetailView::mouseUp(const juce::MouseEvent& event) {
    if (pending_ctrl_command_click_.has_value()) {
        if (command_selection_toggled_callback_) {
            command_selection_toggled_callback_(selected_track_index_, pending_ctrl_command_click_->command);
        }
        pending_ctrl_command_click_.reset();
        return;
    }
    if (active_command_drag_.has_value()) {
        if (command_move_callback_ && insert_preview_anchor_.has_value()) {
            command_move_callback_(
                selected_track_index_,
                active_command_drag_->command,
                insert_preview_anchor_->anchor);
        }
        active_command_drag_.reset();
        if (event.mods.isShiftDown()) {
            update_insert_preview(event.position, event.mods);
        } else {
            insert_mode_active_ = false;
            clear_insert_preview();
        }
        return;
    }
    if (active_resize_draft_.has_value()) {
        if (note_resize_callback_ && resize_preview_.has_value()) {
            const auto preview = *resize_preview_;
            const int32_t authored_start_tick = preview.authored_start_tick >= 0
                ? preview.authored_start_tick + (preview.start_tick - preview.original_start_tick)
                : preview.start_tick;
            note_resize_callback_(
                selected_track_index_,
                preview.source_event_index,
                authored_start_tick,
                preview.base_duration_ticks,
                preview.extension_ticks);
        }
        active_resize_draft_.reset();
        if (event.mods.isCtrlDown()) {
            update_resize_preview(event.position);
        } else {
            clear_resize_preview();
        }
        return;
    }
    if (active_note_draft_.has_value()) {
        if (note_insert_callback_ && note_preview_.has_value()) {
            const auto preview = *note_preview_;
            note_insert_callback_(
                selected_track_index_,
                preview.source_event_index,
                preview.note_relative_key,
                preview.start_tick,
                preview.covered_start_tick,
                preview.duration_ticks);
        }
        active_note_draft_.reset();
        if (!event.mods.isShiftDown()) {
            update_note_preview(event.position);
        } else {
            clear_note_preview();
        }
        return;
    }
    if (active_fermata_draft_.has_value()) {
        if (note_fermata_callback_ && fermata_preview_.has_value()) {
            const auto preview = *fermata_preview_;
            note_fermata_callback_(
                selected_track_index_,
                preview.source_event_index,
                preview.extension_ticks);
        }
        active_fermata_draft_.reset();
        if (event.mods.isShiftDown()) {
            update_fermata_preview(event.position);
        } else {
            clear_fermata_preview();
        }
        return;
    }
    if (middle_drag_panning_) {
        middle_drag_panning_ = false;
        return;
    }
    juce::Component::mouseUp(event);
}

void FFTPianoDetailView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    if (!event.mods.isAltDown() &&
        !event.mods.isCtrlDown() &&
        !event.mods.isShiftDown() &&
        rest_ripple_resize_callback_ != nullptr &&
        selected_track_index_ == selected_rest_track_index_ &&
        (selected_rest_source_event_index_ >= 0 || selected_rest_authored_span_index_ >= 0)) {
        if (const auto* block = note_block_at(event.position, true);
            block != nullptr &&
            block->note.relative_key == 13 &&
            ((selected_rest_authored_span_index_ >= 0 &&
              block->note.authored_span_index == selected_rest_authored_span_index_) ||
             (selected_rest_authored_span_index_ < 0 &&
              block->note.source_event_index == selected_rest_source_event_index_))) {
            const float delta = std::abs(wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX;
            if (std::abs(delta) > 0.0f) {
                const int32_t step = std::max(1, snap_step_for_tick(block->note.start_tick));
                rest_ripple_resize_callback_(
                    selected_track_index_,
                    block->note.source_event_index,
                    block->note.authored_span_index,
                    delta > 0.0f ? step : -step);
                return;
            }
        }
    }
    if (event.mods.isAltDown() && note_snap_adjust_callback_) {
        const float delta = std::abs(wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX;
        if (std::abs(delta) > 0.0f) {
            note_snap_adjust_callback_(delta > 0.0f ? -1 : 1);
            update_note_preview(event.position);
            return;
        }
    }
    if (event.mods.isCtrlDown() && zoom_callback_ && std::abs(wheel.deltaY) > 0.0f) {
        zoom_callback_(x_to_tick(event.position.x), wheel.deltaY);
        return;
    }
    if (event.mods.isShiftDown() && pan_callback_) {
        const float delta = std::abs(wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY;
        if (std::abs(delta) > 0.0f) {
            pan_callback_(static_cast<int>(std::round(-delta * 140.0f)), 0);
            return;
        }
    }
    juce::Component::mouseWheelMove(event, wheel);
}

}  // namespace jucewrap
}  // namespace fftplugin
