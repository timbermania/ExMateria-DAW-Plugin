#include "FFTTrackLaneView.h"
#include "FFTOpcodeStackLayout.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace fftplugin {
namespace jucewrap {

namespace {

constexpr const char* kRelativeKeyNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

juce::String note_label(int relative_key) {
    if (relative_key >= 0 && relative_key < 12) {
        return kRelativeKeyNames[relative_key];
    }
    if (relative_key == 12) {
        return "Tie";
    }
    if (relative_key == 13) {
        return {};
    }
    return juce::String(relative_key);
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

bool should_draw_overview_command(const FFTSmdLaneCommandBlock& command) {
    juce::ignoreUnused(command);
    return true;
}

int32_t align_to_grid_start(int32_t tick, int32_t origin, int32_t step) {
    if (step <= 0) {
        return tick;
    }
    if (tick <= origin) {
        return origin;
    }
    const int32_t delta = tick - origin;
    const int32_t remainder = delta % step;
    return remainder == 0 ? tick : (tick + (step - remainder));
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

}  // namespace

FFTTrackLaneView::FFTTrackLaneView(FFTTrackLaneViewMode mode)
    : mode_(mode) {
    setInterceptsMouseClicks(true, false);
}

void FFTTrackLaneView::set_presentation(FFTSmdSongPresentation presentation) {
    presentation_ = std::move(presentation);
    hovered_diagnostic_marker_.reset();
    selected_track_index_ = juce::jlimit(0, std::max(0, static_cast<int>(presentation_.tracks.size()) - 1), selected_track_index_);
    if (mode_ == FFTTrackLaneViewMode::header) {
        setSize(getWidth(), content_height());
    } else {
        setSize(content_width(), content_height());
    }
    repaint();
}

void FFTTrackLaneView::set_selected_track(int track_index) {
    selected_track_index_ = juce::jlimit(0, std::max(0, static_cast<int>(presentation_.tracks.size()) - 1), track_index);
    repaint();
}

void FFTTrackLaneView::set_multi_selected_tracks(std::vector<int32_t> track_indices) {
    multi_selected_track_indices_ = std::move(track_indices);
    std::sort(multi_selected_track_indices_.begin(), multi_selected_track_indices_.end());
    multi_selected_track_indices_.erase(
        std::unique(multi_selected_track_indices_.begin(), multi_selected_track_indices_.end()),
        multi_selected_track_indices_.end());
    repaint();
}

void FFTTrackLaneView::set_track_selected_callback(std::function<void(int, bool additive)> callback) {
    track_selected_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_track_mute_callback(std::function<void(int)> callback) {
    track_mute_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_track_solo_callback(std::function<void(int)> callback) {
    track_solo_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_command_selected_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback) {
    command_selected_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_command_selection_toggled_callback(
    std::function<void(int, FFTSmdLaneCommandBlock)> callback
) {
    command_selection_toggled_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_command_secondary_click_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback) {
    command_secondary_click_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_command_delete_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback) {
    command_delete_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_command_move_callback(
    std::function<void(int, FFTSmdLaneCommandBlock, FFTSmdLaneInsertAnchor)> callback) {
    command_move_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_note_delete_callback(std::function<void(int, int)> callback) {
    note_delete_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_note_fermata_delete_callback(std::function<void(int, int)> callback) {
    note_fermata_delete_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_note_match_lab_callback(std::function<void(int, int)> callback) {
    note_match_lab_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_insert_anchor_callback(std::function<void(int, FFTSmdLaneInsertAnchor)> callback) {
    insert_anchor_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_time_selection_callback(std::function<void(int32_t, int32_t)> callback) {
    time_selection_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_zoom_callback(std::function<void(float, float)> callback) {
    zoom_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_pan_callback(std::function<void(int, int)> callback) {
    pan_callback_ = std::move(callback);
}

void FFTTrackLaneView::set_horizontal_scroll(int horizontal_scroll) {
    horizontal_scroll_ = std::max(0, horizontal_scroll);
    repaint();
}

void FFTTrackLaneView::set_pixels_per_tick(float pixels_per_tick) {
    pixels_per_tick_ = juce::jlimit(0.1f, 12.0f, pixels_per_tick);
    if (mode_ == FFTTrackLaneViewMode::header) {
        setSize(getWidth(), content_height());
    } else {
        setSize(content_width(), content_height());
    }
    repaint();
}

void FFTTrackLaneView::set_presentation_mode(fftplugin::FFTSmdPresentationMode mode) {
    presentation_mode_ = mode;
    repaint();
}

void FFTTrackLaneView::set_global_playhead_tick(int32_t tick) {
    global_playhead_tick_ = tick;
    repaint();
}

void FFTTrackLaneView::set_source_track_cursor_ticks(std::vector<int32_t> ticks) {
    source_track_cursor_ticks_ = std::move(ticks);
    repaint();
}

void FFTTrackLaneView::set_selected_command(
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

void FFTTrackLaneView::set_selected_commands(
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

void FFTTrackLaneView::set_time_selection(int32_t start_tick, int32_t end_tick) {
    selected_time_start_tick_ = start_tick;
    selected_time_end_tick_ = end_tick;
    repaint();
}

void FFTTrackLaneView::set_command_filter_state(const FFTOpcodeFilterState& state) {
    command_filter_state_ = state;
    repaint();
}

void FFTTrackLaneView::set_note_snap_mode_index(int mode_index) {
    note_snap_mode_index_ = std::clamp(mode_index, 0, 8);
    repaint();
}

float FFTTrackLaneView::pixels_per_tick() const {
    return pixels_per_tick_;
}

void FFTTrackLaneView::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour::fromRGB(18, 20, 24));
    visible_command_chips_.clear();
    visible_insert_anchors_.clear();
    visible_note_blocks_.clear();
    visible_diagnostic_markers_.clear();

    if (mode_ != FFTTrackLaneViewMode::tracks) {
        draw_ruler(g, getLocalBounds().removeFromTop(kRulerHeight));
    }

    if (mode_ != FFTTrackLaneViewMode::header) {
        for (size_t i = 0; i < presentation_.tracks.size(); ++i) {
            draw_track_row(g, track_row_bounds(static_cast<int>(i)), presentation_.tracks[i]);
        }
    }

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

void FFTTrackLaneView::mouseDown(const juce::MouseEvent& event) {
    update_insert_preview(event.position, event.mods);
    if (mode_ == FFTTrackLaneViewMode::header &&
        !event.mods.isMiddleButtonDown() &&
        !event.mods.isRightButtonDown() &&
        !event.mods.isShiftDown() &&
        !event.mods.isCtrlDown()) {
        const auto timeline = ruler_timeline_bounds();
        if (timeline.contains(event.getPosition())) {
            const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(event.position.x)));
            const int32_t snapped_tick = round_tick_to_grid_segment(
                raw_tick,
                grid_segment_for_tick(raw_tick),
                snap_step_for_tick(raw_tick));
            const int32_t step = std::max(1, snap_step_for_tick(snapped_tick));
            active_time_selection_draft_ = TimeSelectionDraft {
                .anchor_tick = snapped_tick,
                .current_tick = snapped_tick + step,
            };
            if (time_selection_callback_ != nullptr) {
                time_selection_callback_(snapped_tick, snapped_tick + step);
            }
            repaint();
            return;
        }
    }
    if (event.mods.isMiddleButtonDown() && pan_callback_) {
        middle_drag_panning_ = true;
        pan_start_position_ = event.mouseDownPosition;
        return;
    }
    if (mode_ == FFTTrackLaneViewMode::header) {
        return;
    }
    const int y = event.getPosition().y;
    const int track_area_y = mode_ == FFTTrackLaneViewMode::tracks ? 0 : kRulerHeight;
    if (y < track_area_y) {
        return;
    }

    if (!event.mods.isMiddleButtonDown()) {
        if (event.mods.isAltDown() &&
            !event.mods.isCtrlDown() &&
            !event.mods.isShiftDown() &&
            !event.mods.isRightButtonDown()) {
            for (const auto& block : visible_note_blocks_) {
                if (!block.bounds.contains(event.getPosition()) ||
                    block.note.relative_key < 0 ||
                    block.note.relative_key >= 12 ||
                    note_match_lab_callback_ == nullptr) {
                    continue;
                }
                set_selected_track(block.track_index);
                if (track_selected_callback_) {
                    track_selected_callback_(block.track_index, false);
                }
                note_match_lab_callback_(block.track_index, block.note.source_event_index);
                return;
            }
        }
        if (event.mods.isShiftDown() && event.mods.isRightButtonDown()) {
            for (const auto& block : visible_note_blocks_) {
                if (block.track_index != selected_track_index_ || !block.bounds.contains(event.getPosition())) {
                    continue;
                }
                if (block.note.fermata_extension_ticks <= 0) {
                    continue;
                }
                const int total_w = block.bounds.getWidth();
                const int extension_w = std::clamp(
                    static_cast<int>(std::round(block.note.fermata_extension_ticks * pixels_per_tick_)),
                    0,
                    total_w);
                const int base_w = std::max(0, total_w - extension_w);
                if (event.getPosition().x >= block.bounds.getX() + base_w) {
                    set_selected_track(block.track_index);
                    if (track_selected_callback_) {
                        track_selected_callback_(block.track_index, false);
                    }
                    if (note_fermata_delete_callback_) {
                        note_fermata_delete_callback_(block.track_index, block.note.source_event_index);
                    }
                    return;
                }
            }
            for (const auto& anchor : visible_insert_anchors_) {
                if (anchor.anchor.kind == FFTSmdLaneInsertAnchorKind::note_start &&
                    anchor.anchor.source_event_index >= 0 &&
                    anchor.bounds.contains(event.getPosition())) {
                    set_selected_track(anchor.track_index);
                    if (track_selected_callback_) {
                        track_selected_callback_(anchor.track_index, false);
                    }
                    if (note_delete_callback_) {
                        note_delete_callback_(anchor.track_index, anchor.anchor.source_event_index);
                    }
                    return;
                }
            }
        }
        if (event.mods.isShiftDown() && !event.mods.isRightButtonDown() && insert_anchor_callback_) {
            if (update_insert_preview(event.position, event.mods) && insert_preview_anchor_.has_value()) {
                set_selected_track(insert_preview_anchor_->track_index);
                if (track_selected_callback_) {
                    track_selected_callback_(insert_preview_anchor_->track_index, false);
                }
                insert_anchor_callback_(insert_preview_anchor_->track_index, insert_preview_anchor_->anchor);
                return;
            }
        }
        for (const auto& chip : visible_command_chips_) {
            if (chip.bounds.contains(event.getPosition())) {
                set_selected_track(chip.track_index);
                if (track_selected_callback_) {
                    track_selected_callback_(chip.track_index, false);
                }
                if (event.mods.isCtrlDown() &&
                    !event.mods.isShiftDown() &&
                    !event.mods.isRightButtonDown() &&
                    !event.mods.isMiddleButtonDown()) {
                    pending_ctrl_command_click_ = PendingCtrlCommandClick {
                        .track_index = chip.track_index,
                        .command = chip.command,
                        .mouse_down_position = event.position,
                    };
                } else if (event.mods.isShiftDown() && event.mods.isRightButtonDown()) {
                    if (command_delete_callback_ &&
                        (chip.command.authored_opcode_index >= 0 || chip.command.source_event_index >= 0)) {
                        command_delete_callback_(chip.track_index, chip.command);
                    }
                } else if (event.mods.isRightButtonDown()) {
                    if (command_secondary_click_callback_) {
                        command_secondary_click_callback_(chip.track_index, chip.command);
                    }
                } else if (command_selected_callback_) {
                    command_selected_callback_(chip.track_index, chip.command);
                }
                return;
            }
        }
    }

    const int track_index = (y - track_area_y) / kTrackRowHeight;
    if (track_index < 0 || static_cast<size_t>(track_index) >= presentation_.tracks.size()) {
        return;
    }

    const auto row_bounds = track_row_bounds(track_index);
    auto header = juce::Rectangle<int>(current_header_x(), row_bounds.getY(), kHeaderWidth, row_bounds.getHeight());
    const auto mute_bounds = track_mute_button_bounds(header);
    const auto solo_bounds = track_solo_button_bounds(header);
    const int actual_track_index = presentation_.tracks[static_cast<size_t>(track_index)].track_index;
    if (mute_bounds.contains(event.getPosition())) {
        if (track_mute_callback_) {
            track_mute_callback_(actual_track_index);
        }
        set_selected_track(actual_track_index);
        return;
    }
    if (solo_bounds.contains(event.getPosition())) {
        if (track_solo_callback_) {
            track_solo_callback_(actual_track_index);
        }
        set_selected_track(actual_track_index);
        return;
    }

    set_selected_track(actual_track_index);
    if (track_selected_callback_) {
        track_selected_callback_(actual_track_index, event.mods.isCtrlDown());
    }
}

void FFTTrackLaneView::mouseDrag(const juce::MouseEvent& event) {
    if (active_time_selection_draft_.has_value()) {
        const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(event.position.x)));
        active_time_selection_draft_->current_tick = round_tick_to_grid_segment(
            raw_tick,
            grid_segment_for_tick(raw_tick),
            snap_step_for_tick(raw_tick));
        if (time_selection_callback_ != nullptr) {
            const int32_t start_tick = std::min(
                active_time_selection_draft_->anchor_tick,
                active_time_selection_draft_->current_tick);
            const int32_t end_tick = std::max(
                active_time_selection_draft_->anchor_tick,
                active_time_selection_draft_->current_tick);
            time_selection_callback_(start_tick, end_tick);
        }
        repaint();
        return;
    }
    if (pending_ctrl_command_click_.has_value() &&
        (event.position - pending_ctrl_command_click_->mouse_down_position).getDistanceFromOrigin() > 3.0f) {
        active_command_drag_ = DraggedCommandChip {
            .track_index = pending_ctrl_command_click_->track_index,
            .command = pending_ctrl_command_click_->command,
        };
        pending_ctrl_command_click_.reset();
        update_insert_preview(event.position, event.mods);
        return;
    }
    if (active_command_drag_.has_value()) {
        update_insert_preview(event.position, event.mods);
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

void FFTTrackLaneView::mouseMove(const juce::MouseEvent& event) {
    last_mouse_position_ = event.position;
    if (active_command_drag_.has_value()) {
        update_insert_preview(event.position, event.mods);
        return;
    }
    update_insert_preview(event.position, event.mods);

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
         (new_hovered_diagnostic->tick != hovered_diagnostic_marker_->tick ||
          new_hovered_diagnostic->track_index != hovered_diagnostic_marker_->track_index))) {
        hovered_diagnostic_marker_ = std::move(new_hovered_diagnostic);
        repaint();
    }
    juce::Component::mouseMove(event);
}

void FFTTrackLaneView::mouseExit(const juce::MouseEvent& event) {
    juce::ignoreUnused(event);
    if (active_command_drag_.has_value() || active_time_selection_draft_.has_value()) {
        return;
    }
    pending_ctrl_command_click_.reset();
    insert_mode_active_ = false;
    clear_insert_preview();
    if (hovered_diagnostic_marker_.has_value()) {
        hovered_diagnostic_marker_.reset();
        repaint();
    }
}

void FFTTrackLaneView::mouseUp(const juce::MouseEvent& event) {
    if (active_time_selection_draft_.has_value()) {
        int32_t start_tick = std::min(
            active_time_selection_draft_->anchor_tick,
            active_time_selection_draft_->current_tick);
        int32_t end_tick = std::max(
            active_time_selection_draft_->anchor_tick,
            active_time_selection_draft_->current_tick);
        if (end_tick <= start_tick) {
            const int32_t step = std::max(1, snap_step_for_tick(active_time_selection_draft_->anchor_tick));
            end_tick = start_tick + step;
        }
        active_time_selection_draft_.reset();
        if (time_selection_callback_ != nullptr) {
            time_selection_callback_(start_tick, end_tick);
        }
        repaint();
        return;
    }
    if (pending_ctrl_command_click_.has_value()) {
        if (command_selection_toggled_callback_) {
            command_selection_toggled_callback_(
                pending_ctrl_command_click_->track_index,
                pending_ctrl_command_click_->command);
        }
        pending_ctrl_command_click_.reset();
        return;
    }
    if (active_command_drag_.has_value()) {
        if (command_move_callback_ && insert_preview_anchor_.has_value()) {
            command_move_callback_(
                active_command_drag_->track_index,
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
    if (middle_drag_panning_) {
        middle_drag_panning_ = false;
        return;
    }
    juce::Component::mouseUp(event);
}

void FFTTrackLaneView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    if (event.mods.isCtrlDown() && zoom_callback_ && std::abs(wheel.deltaY) > 0.0f) {
        zoom_callback_(x_to_tick(event.position.x), wheel.deltaY);
        return;
    }
    juce::Component::mouseWheelMove(event, wheel);
}

void FFTTrackLaneView::clear_insert_preview() {
    if (insert_preview_anchor_.has_value()) {
        insert_preview_anchor_.reset();
        repaint();
    }
}

const FFTSmdGridSegment* FFTTrackLaneView::grid_segment_for_tick(int32_t tick) const {
    for (const auto& segment : presentation_.grid_segments) {
        if (tick >= segment.start_tick && tick < segment.end_tick) {
            return &segment;
        }
    }
    if (!presentation_.grid_segments.empty()) {
        return &presentation_.grid_segments.back();
    }
    return nullptr;
}

int32_t FFTTrackLaneView::snap_step_for_tick(int32_t tick) const {
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

int32_t FFTTrackLaneView::snap_tick_to_grid(const FFTSmdTrackLanePresentation& track, int32_t raw_tick) const {
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
    return std::max(0, std::clamp(snapped, segment->start_tick, segment->end_tick));
}

bool FFTTrackLaneView::update_insert_preview(const juce::Point<float>& position, const juce::ModifierKeys& mods) {
    if ((!mods.isShiftDown() && !active_command_drag_.has_value()) || mode_ == FFTTrackLaneViewMode::header) {
        insert_mode_active_ = false;
        clear_insert_preview();
        return false;
    }

    insert_mode_active_ = true;

    const int y = juce::roundToInt(position.y);
    const int track_area_y = mode_ == FFTTrackLaneViewMode::tracks ? 0 : kRulerHeight;
    if (y < track_area_y) {
        clear_insert_preview();
        return false;
    }

    int row_index = (y - track_area_y) / kTrackRowHeight;
    if (active_command_drag_.has_value()) {
        const auto track_it = std::find_if(
            presentation_.tracks.begin(),
            presentation_.tracks.end(),
            [this](const FFTSmdTrackLanePresentation& track) {
                return track.track_index == active_command_drag_->track_index;
            });
        if (track_it == presentation_.tracks.end()) {
            clear_insert_preview();
            return false;
        }
        row_index = static_cast<int>(std::distance(presentation_.tracks.begin(), track_it));
    } else if (row_index < 0 || static_cast<size_t>(row_index) >= presentation_.tracks.size()) {
        clear_insert_preview();
        return false;
    }

    const auto& track = presentation_.tracks[static_cast<size_t>(row_index)];
    const auto row_bounds = track_row_bounds(row_index);
    auto timeline = juce::Rectangle<int>(
        current_timeline_origin_x(),
        row_bounds.getY() + 4,
        getWidth() - (current_timeline_origin_x() + kTimelinePadding),
        row_bounds.getHeight() - 8);
    auto note_lane = timeline.removeFromTop(28);
    timeline.removeFromTop(4);
    auto opcode_lane = timeline;
    const int opcode_row_h = std::max(16, opcode_lane.getHeight() / kOverviewOpcodeRows);

    const int py = juce::roundToInt(position.y);
    const int32_t raw_tick = static_cast<int32_t>(std::round(x_to_tick(position.x)));
    const int32_t requested_tick = round_tick_to_grid_segment(
        raw_tick,
        grid_segment_for_tick(raw_tick),
        snap_step_for_tick(raw_tick));

    int preview_row = 0;
    if (py >= opcode_lane.getY() && py < opcode_lane.getBottom()) {
        preview_row = std::clamp((py - opcode_lane.getY()) / opcode_row_h, 0, kOverviewOpcodeRows - 1);
    }

    const auto is_drag_excluded_command = [this, &track](const FFTSmdLaneCommandBlock& command) {
        if (!active_command_drag_.has_value()) {
            return false;
        }
        if (track.track_index == selected_command_track_index_) {
            if (command.authored_opcode_index >= 0 &&
                !selected_command_authored_opcode_indices_.empty()) {
                return std::find(
                           selected_command_authored_opcode_indices_.begin(),
                           selected_command_authored_opcode_indices_.end(),
                           command.authored_opcode_index) != selected_command_authored_opcode_indices_.end();
            }
            if (command.source_event_index >= 0 &&
                !selected_command_source_event_indices_.empty()) {
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

    const auto is_drag_excluded_anchor = [this, &track, &is_drag_excluded_command](const FFTSmdLaneInsertAnchor& anchor) {
        if (!active_command_drag_.has_value()) {
            return false;
        }
        if (track.track_index == selected_command_track_index_ &&
            anchor.authored_opcode_index >= 0 &&
            !selected_command_authored_opcode_indices_.empty()) {
            return std::find(
                       selected_command_authored_opcode_indices_.begin(),
                       selected_command_authored_opcode_indices_.end(),
                       anchor.authored_opcode_index) != selected_command_authored_opcode_indices_.end();
        }
        if (track.track_index == selected_command_track_index_ &&
            anchor.source_event_index >= 0 &&
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

    std::vector<const DisplayedInsertAnchor*> same_tick_slots;
    for (const auto& anchor : visible_insert_anchors_) {
        if (anchor.track_index != track.track_index || anchor.anchor.tick != requested_tick) {
            continue;
        }
        if (anchor.anchor.kind != FFTSmdLaneInsertAnchorKind::command) {
            continue;
        }
        if (is_drag_excluded_anchor(anchor.anchor)) {
            continue;
        }
        same_tick_slots.push_back(&anchor);
    }
    std::sort(
        same_tick_slots.begin(),
        same_tick_slots.end(),
        [](const DisplayedInsertAnchor* lhs, const DisplayedInsertAnchor* rhs) {
            if (lhs->bounds.getY() != rhs->bounds.getY()) {
                return lhs->bounds.getY() < rhs->bounds.getY();
            }
            return lhs->anchor.insertion_sequence_index < rhs->anchor.insertion_sequence_index;
        });

    int insertion_sequence_index = 0;
    int max_source_event_index = -1;
    bool found_future_event = false;
    for (const auto& command : track.commands) {
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

    if (!same_tick_slots.empty()) {
        insertion_sequence_index = same_tick_slots.back()->anchor.insertion_sequence_index + 1;
        for (const auto* slot : same_tick_slots) {
            const int slot_row = std::max(0, (slot->bounds.getY() - opcode_lane.getY()) / opcode_row_h);
            if (preview_row <= slot_row) {
                insertion_sequence_index = slot->anchor.insertion_sequence_index;
                preview_row = slot_row;
                break;
            }
        }
    }

    const int preview_x = static_cast<int>(std::round(tick_to_x(requested_tick)));
    const int preview_y = opcode_lane.getY() + preview_row * opcode_row_h + 2;
    const int preview_h = std::max(12, opcode_row_h - 4);
    insert_preview_anchor_ = DisplayedInsertAnchor {
        .bounds = juce::Rectangle<int>(preview_x, preview_y, 40, preview_h),
        .track_index = track.track_index,
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

int FFTTrackLaneView::content_width() const {
    return std::max(
        720,
        kHeaderWidth + (kTimelinePadding * 2) + static_cast<int>(std::ceil(presentation_.total_ticks * pixels_per_tick_))
    );
}

int FFTTrackLaneView::content_height() const {
    if (mode_ == FFTTrackLaneViewMode::header) {
        return kRulerHeight;
    }
    if (mode_ == FFTTrackLaneViewMode::tracks) {
        return static_cast<int>(presentation_.tracks.size()) * kTrackRowHeight;
    }
    return kRulerHeight + (static_cast<int>(presentation_.tracks.size()) * kTrackRowHeight);
}

int FFTTrackLaneView::current_header_x() const {
    return mode_ == FFTTrackLaneViewMode::tracks ? horizontal_scroll_ : 0;
}

int FFTTrackLaneView::current_timeline_origin_x() const {
    return (mode_ == FFTTrackLaneViewMode::tracks ? 0 : current_header_x()) + kHeaderWidth + kTimelinePadding;
}

juce::Rectangle<int> FFTTrackLaneView::ruler_timeline_bounds() const {
    const int header_x = current_header_x();
    return juce::Rectangle<int>(
        header_x + kHeaderWidth,
        0,
        std::max(0, getWidth() - (header_x + kHeaderWidth)),
        kRulerHeight);
}

float FFTTrackLaneView::tick_to_x(int32_t tick) const {
    return static_cast<float>(current_timeline_origin_x()) + (static_cast<float>(tick) * pixels_per_tick_) -
        (mode_ == FFTTrackLaneViewMode::header ? static_cast<float>(horizontal_scroll_) : 0.0f);
}

float FFTTrackLaneView::x_to_tick(float x) const {
    const float content_x = x - static_cast<float>(current_timeline_origin_x()) +
        (mode_ == FFTTrackLaneViewMode::header ? static_cast<float>(horizontal_scroll_) : 0.0f);
    return std::max(0.0f, content_x / pixels_per_tick_);
}

juce::Rectangle<int> FFTTrackLaneView::conductor_bounds() const {
    return {};
}

juce::Rectangle<int> FFTTrackLaneView::track_row_bounds(int track_index) const {
    const int track_origin_y = mode_ == FFTTrackLaneViewMode::tracks ? 0 : kRulerHeight;
    return {0, track_origin_y + (track_index * kTrackRowHeight), getWidth(), kTrackRowHeight};
}

juce::Rectangle<int> FFTTrackLaneView::track_mute_button_bounds(const juce::Rectangle<int>& header) const {
    auto button_row = header.withTrimmedTop(6).removeFromBottom(header.getHeight() - 22).withTrimmedLeft(58);
    return button_row.removeFromLeft(22);
}

juce::Rectangle<int> FFTTrackLaneView::track_solo_button_bounds(const juce::Rectangle<int>& header) const {
    auto button_row = header.withTrimmedTop(6).removeFromBottom(header.getHeight() - 22).withTrimmedLeft(84);
    return button_row.removeFromLeft(22);
}

void FFTTrackLaneView::draw_ruler(juce::Graphics& g, juce::Rectangle<int> bounds) const {
    g.setColour(juce::Colour::fromRGB(36, 39, 46));
    g.fillRect(bounds);
    g.setColour(juce::Colour::fromRGB(60, 63, 72));
    const int header_x = current_header_x();
    g.drawLine(static_cast<float>(header_x + kHeaderWidth), static_cast<float>(bounds.getY()), static_cast<float>(header_x + kHeaderWidth), static_cast<float>(bounds.getBottom()));

    auto timeline = ruler_timeline_bounds().reduced(kTimelinePadding, 0);
    const int seconds_top = timeline.getY();
    const int seconds_bottom = timeline.getY() + 14;
    const int bars_top = timeline.getY() + 16;
    const int bars_bottom = timeline.getBottom();

    int32_t selection_start_tick = selected_time_start_tick_;
    int32_t selection_end_tick = selected_time_end_tick_;
    if (active_time_selection_draft_.has_value()) {
        selection_start_tick = std::min(
            active_time_selection_draft_->anchor_tick,
            active_time_selection_draft_->current_tick);
        selection_end_tick = std::max(
            active_time_selection_draft_->anchor_tick,
            active_time_selection_draft_->current_tick);
    }
    if (selection_end_tick > selection_start_tick) {
        const int left = static_cast<int>(std::round(tick_to_x(selection_start_tick)));
        const int right = static_cast<int>(std::round(tick_to_x(selection_end_tick)));
        const int actual_width = std::max(1, right - left);
        const int fill_width = std::max(6, actual_width);
        const int fill_left = actual_width >= fill_width ? left : (left - ((fill_width - actual_width) / 2));
        g.setColour(juce::Colour::fromRGBA(245, 232, 150, 34));
        g.fillRect(juce::Rectangle<int>(fill_left, bounds.getY(), fill_width, bounds.getHeight()));
        g.setColour(juce::Colour::fromRGB(245, 232, 150));
        g.drawVerticalLine(left, static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
        g.drawVerticalLine(right, static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
    }

    g.setColour(juce::Colour::fromRGB(70, 74, 84));
    g.drawHorizontalLine(seconds_bottom, static_cast<float>(timeline.getX()), static_cast<float>(timeline.getRight()));
    g.setColour(juce::Colour::fromRGB(178, 182, 192));
    for (const auto& marker : presentation_.second_markers) {
        const float x = tick_to_x(marker.tick);
        if (x < static_cast<float>(header_x + kHeaderWidth) - 8.0f || x > static_cast<float>(getWidth() + 8)) {
            continue;
        }
        g.drawText(marker.label, static_cast<int>(x) + 3, seconds_top, 44, 12, juce::Justification::left);
    }

    if (!presentation_.grid_segments.empty()) {
        int bar_number = 1;
        for (const auto& segment : presentation_.grid_segments) {
            int32_t beat_tick = align_to_grid_start(segment.start_tick, segment.start_tick, segment.ticks_per_beat);
            for (; beat_tick <= segment.end_tick; beat_tick += segment.ticks_per_beat) {
                const bool is_bar = ((beat_tick - segment.start_tick) % segment.ticks_per_bar) == 0;
                const float x = tick_to_x(beat_tick);
                if (x < static_cast<float>(header_x + kHeaderWidth) - 8.0f || x > static_cast<float>(getWidth() + 8)) {
                    if (is_bar) {
                        bar_number += 1;
                    }
                    continue;
                }
                g.setColour(is_bar ? juce::Colour::fromRGB(120, 124, 136) : juce::Colour::fromRGB(74, 78, 88));
                g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(bars_top + 2), static_cast<float>(bars_bottom));
                if (is_bar) {
                    g.drawText(juce::String(bar_number), static_cast<int>(x) + 3, bars_top, 40, 12, juce::Justification::left);
                    bar_number += 1;
                }
            }
        }
    } else {
        g.setColour(juce::Colour::fromRGB(94, 97, 108));
        for (int tick = 0; tick <= presentation_.total_ticks; tick += 96) {
            const float x = tick_to_x(tick);
            if (x < static_cast<float>(header_x + kHeaderWidth) - 8.0f || x > static_cast<float>(getWidth() + 8)) {
                continue;
            }
            g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(bars_top + 2), static_cast<float>(bars_bottom));
            g.drawText(juce::String(tick), static_cast<int>(x) + 3, bars_top, 52, 12, juce::Justification::left);
        }
    }

    if (presentation_mode_ == fftplugin::FFTSmdPresentationMode::playback && global_playhead_tick_ >= 0) {
        const float x = tick_to_x(global_playhead_tick_);
        g.setColour(juce::Colour::fromRGB(255, 96, 96));
        g.drawLine(x, static_cast<float>(bounds.getY()), x, static_cast<float>(bounds.getBottom()), 1.5f);
    }
}

void FFTTrackLaneView::draw_conductor(juce::Graphics& g, juce::Rectangle<int> bounds) const {
    juce::ignoreUnused(g, bounds);
}

void FFTTrackLaneView::draw_track_row(
    juce::Graphics& g,
    juce::Rectangle<int> bounds,
    const FFTSmdTrackLanePresentation& track
) {
    const bool selected = track.track_index == selected_track_index_ ||
        std::binary_search(
            multi_selected_track_indices_.begin(),
            multi_selected_track_indices_.end(),
            track.track_index);
    const juce::Colour base_colour = track.muted
        ? juce::Colour::fromRGB(20, 21, 25)
        : juce::Colour::fromRGB(24, 26, 31);
    g.setColour(selected ? juce::Colour::fromRGB(42, 52, 70) : base_colour);
    g.fillRect(bounds);
    g.setColour(juce::Colour::fromRGB(60, 64, 74));
    g.drawRect(bounds);

    const int header_x = current_header_x();
    const auto header_bounds = juce::Rectangle<int>(header_x, bounds.getY(), kHeaderWidth, bounds.getHeight());
    const auto timeline_visible = juce::Rectangle<int>(
        header_x + kHeaderWidth + kTimelinePadding,
        bounds.getY() + 4,
        getWidth() - (header_x + kHeaderWidth + (kTimelinePadding * 2)),
        bounds.getHeight() - 8);
    auto timeline = juce::Rectangle<int>(
        current_timeline_origin_x(),
        bounds.getY() + 4,
        getWidth() - (current_timeline_origin_x() + kTimelinePadding),
        bounds.getHeight() - 8);
    auto note_lane = timeline.removeFromTop(28);
    timeline.removeFromTop(4);
    auto opcode_lane = timeline;
    auto note_label_lane = note_lane.removeFromTop(12);
    auto note_bar_lane = note_lane;

    g.saveState();
    g.reduceClipRegion(timeline_visible);

    g.setColour(juce::Colour::fromRGB(33, 36, 44));
    g.fillRect(juce::Rectangle<int>(note_label_lane.getX(), note_label_lane.getY(), note_label_lane.getWidth(), note_label_lane.getHeight() + note_bar_lane.getHeight()));
    g.fillRect(opcode_lane);
    g.setColour(juce::Colour::fromRGB(52, 56, 66));
    g.drawRect(juce::Rectangle<int>(note_label_lane.getX(), note_label_lane.getY(), note_label_lane.getWidth(), note_label_lane.getHeight() + note_bar_lane.getHeight()));
    g.drawRect(opcode_lane);

    for (const auto& segment : presentation_.grid_segments) {
        int32_t beat_tick = align_to_grid_start(segment.start_tick, segment.start_tick, segment.ticks_per_beat);
        for (; beat_tick <= segment.end_tick; beat_tick += segment.ticks_per_beat) {
            const bool is_bar = ((beat_tick - segment.start_tick) % segment.ticks_per_bar) == 0;
            const float x = tick_to_x(beat_tick);
            if (x < static_cast<float>(current_timeline_origin_x()) - 8.0f || x > static_cast<float>(getWidth() + 8)) {
                continue;
            }
            g.setColour(is_bar ? juce::Colour::fromRGB(70, 74, 86) : juce::Colour::fromRGB(42, 45, 54));
            g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(note_label_lane.getY()), static_cast<float>(opcode_lane.getBottom()));
        }
    }

    if (presentation_mode_ == fftplugin::FFTSmdPresentationMode::playback) {
        for (const auto& boundary : track.loop_boundaries) {
            const float x = tick_to_x(boundary.tick);
            if (x < static_cast<float>(current_timeline_origin_x()) - 12.0f || x > static_cast<float>(getWidth() + 12)) {
                continue;
            }
            const auto colour = loop_boundary_colour(boundary.loop_depth);
            g.setColour(colour.withAlpha(0.28f));
            g.fillRect(juce::Rectangle<int>(
                static_cast<int>(std::round(x)) - 1,
                note_label_lane.getY(),
                3,
                opcode_lane.getBottom() - note_label_lane.getY()));
            g.setColour(colour);
            g.drawVerticalLine(static_cast<int>(std::round(x)), static_cast<float>(note_label_lane.getY()), static_cast<float>(opcode_lane.getBottom()));
            g.drawText(
                juce::String(boundary.label),
                static_cast<int>(std::round(x)) + 4,
                note_label_lane.getY() + 1,
                56,
                12,
                juce::Justification::left,
                false);
        }
    }

    if (presentation_mode_ == fftplugin::FFTSmdPresentationMode::source) {
        const size_t cursor_index = static_cast<size_t>(track.track_index);
        if (cursor_index < source_track_cursor_ticks_.size()) {
            const float x = tick_to_x(source_track_cursor_ticks_[cursor_index]);
            g.setColour(juce::Colour::fromRGB(255, 96, 96));
            g.drawLine(x, static_cast<float>(note_label_lane.getY()), x, static_cast<float>(opcode_lane.getBottom()), 1.5f);
        }
    } else if (global_playhead_tick_ >= 0) {
        const float x = tick_to_x(global_playhead_tick_);
        g.setColour(juce::Colour::fromRGB(255, 96, 96));
        g.drawLine(x, static_cast<float>(note_label_lane.getY()), x, static_cast<float>(opcode_lane.getBottom()), 1.5f);
    }

    if (track.track_index == selected_track_index_ && selected_time_end_tick_ > selected_time_start_tick_) {
        const int left = static_cast<int>(std::round(tick_to_x(selected_time_start_tick_)));
        const int right = static_cast<int>(std::round(tick_to_x(selected_time_end_tick_)));
        g.setColour(juce::Colour::fromRGBA(245, 232, 150, 26));
        g.fillRect(juce::Rectangle<int>(
            left,
            note_label_lane.getY(),
            std::max(1, right - left),
            opcode_lane.getBottom() - note_label_lane.getY()));
        g.setColour(juce::Colour::fromRGB(245, 232, 150));
        g.drawVerticalLine(left, static_cast<float>(note_label_lane.getY()), static_cast<float>(opcode_lane.getBottom()));
        g.drawVerticalLine(right, static_cast<float>(note_label_lane.getY()), static_cast<float>(opcode_lane.getBottom()));
    }

    for (const auto& diagnostic : group_diagnostics(track.diagnostics)) {
        const int x = static_cast<int>(std::round(tick_to_x(diagnostic.tick)));
        const juce::Rectangle<int> marker_bounds(
            x - 4,
            note_label_lane.getY(),
            8,
            opcode_lane.getBottom() - note_label_lane.getY());
        visible_diagnostic_markers_.push_back(DisplayedDiagnosticMarker {
            .bounds = marker_bounds,
            .track_index = track.track_index,
            .tick = diagnostic.tick,
            .severity = diagnostic.severity,
            .message = diagnostic.message,
        });
        g.setColour(diagnostic_colour(diagnostic.severity).withAlpha(0.40f));
        g.drawVerticalLine(x, static_cast<float>(note_label_lane.getY()), static_cast<float>(opcode_lane.getBottom()));
        juce::Path triangle;
        triangle.addTriangle(
            static_cast<float>(x), static_cast<float>(note_label_lane.getY() + 2),
            static_cast<float>(x - 5), static_cast<float>(note_label_lane.getY() + 10),
            static_cast<float>(x + 5), static_cast<float>(note_label_lane.getY() + 10));
        g.setColour(diagnostic_colour(diagnostic.severity));
        g.fillPath(triangle);
    }

        for (const FFTSmdLaneNoteBlock& note : track.notes) {
            const int x = static_cast<int>(std::round(tick_to_x(note.start_tick)));
            const int w = std::max(2, static_cast<int>(std::round(note.duration_ticks * pixels_per_tick_)));
        const bool is_rest = note.relative_key == 13;
        const auto note_rect = juce::Rectangle<int>(x, note_bar_lane.getY() + 2, w, 10);
        visible_note_blocks_.push_back(DisplayedNoteBlock {
            .bounds = note_rect,
            .track_index = track.track_index,
            .note = note,
        });
        if (note.source_event_index >= 0) {
            visible_insert_anchors_.push_back(DisplayedInsertAnchor {
                .bounds = note_rect,
                .track_index = track.track_index,
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
            g.fillRoundedRectangle(note_rect.toFloat(), 2.0f);
        } else {
            const int extension_w = std::clamp(
                static_cast<int>(std::round(note.fermata_extension_ticks * pixels_per_tick_)),
                0,
                w);
            const int base_w = std::max(0, w - extension_w);
            if (base_w > 0) {
                const auto base_rect = juce::Rectangle<int>(x, note_bar_lane.getY() + 2, base_w, 10);
                g.setColour(juce::Colour::fromRGB(114, 178, 255));
                g.fillRoundedRectangle(base_rect.toFloat(), 2.0f);
            }
            if (extension_w > 0) {
                const auto ext_rect = juce::Rectangle<int>(x + base_w, note_bar_lane.getY() + 2, extension_w, 10);
                g.setColour(juce::Colour::fromRGB(255, 196, 92));
                g.fillRoundedRectangle(ext_rect.toFloat(), 2.0f);
            }
            if (base_w == 0 && extension_w == 0) {
                g.setColour(juce::Colour::fromRGB(114, 178, 255));
                g.fillRoundedRectangle(note_rect.toFloat(), 2.0f);
            }
        }
        const juce::String label = note_label(note.relative_key);
        if (label.isNotEmpty()) {
            g.setColour(juce::Colours::lightgrey);
            g.drawText(
                label,
                juce::Rectangle<int>(x, note_label_lane.getY(), std::max(w + 16, 24), note_label_lane.getHeight()),
                juce::Justification::centredLeft,
                false
            );
        }
    }

    g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    const auto placements = layout_command_chips(
        track.commands,
        [this](const FFTSmdLaneCommandBlock& command) {
            return should_draw_overview_command(command) &&
                (insert_mode_active_ || fft_should_draw_filtered_command(command, command_filter_state_));
        },
        [this](int32_t tick) {
            return tick_to_x(tick);
        },
        opcode_lane,
        kOverviewOpcodeRows,
        g.getCurrentFont());
    for (const auto& placement : placements) {
        const auto& command = track.commands[placement.command_index];
        const auto& chip_bounds = placement.bounds;
            if (command.source_event_index >= 0) {
                visible_insert_anchors_.push_back(DisplayedInsertAnchor {
                    .bounds = chip_bounds,
                    .track_index = track.track_index,
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
        const bool selected_command =
            (track.track_index == selected_command_track_index_ &&
             command.authored_opcode_index >= 0 &&
             std::find(
                 selected_command_authored_opcode_indices_.begin(),
                 selected_command_authored_opcode_indices_.end(),
                 command.authored_opcode_index) != selected_command_authored_opcode_indices_.end()) ||
            (track.track_index == selected_command_track_index_ &&
             command.source_event_index >= 0 &&
             std::find(
                 selected_command_source_event_indices_.begin(),
                 selected_command_source_event_indices_.end(),
                 command.source_event_index) != selected_command_source_event_indices_.end()) ||
            (track.track_index == selected_command_track_index_ &&
             ((command.authored_opcode_index >= 0 &&
               command.authored_opcode_index == selected_command_authored_opcode_index_) ||
              (command.source_event_index >= 0 &&
               command.source_event_index == selected_command_source_event_index_)) &&
             command.opcode == selected_command_opcode_);
        visible_command_chips_.push_back(DisplayedCommandChip {
            .bounds = chip_bounds,
            .track_index = track.track_index,
            .command = command,
        });
        g.setColour(command_fill_colour(command.kind).withAlpha(command.enabled ? 1.0f : 0.35f));
        g.fillRoundedRectangle(chip_bounds.toFloat(), 3.0f);
        if (selected_command) {
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
        g.drawText(command.label, chip_bounds.getX() + 5, chip_bounds.getY(), chip_bounds.getWidth() - 8, chip_bounds.getHeight(), juce::Justification::centredLeft, false);
    }

    if (insert_preview_anchor_.has_value() && insert_preview_anchor_->track_index == track.track_index) {
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

    g.restoreState();

    auto header = header_bounds;
    g.setColour(selected ? juce::Colour::fromRGB(42, 52, 70) : base_colour);
    g.fillRect(header);
    g.setColour(juce::Colour::fromRGB(60, 64, 74));
    g.drawRect(header);
    g.setColour(juce::Colours::white);
    const juce::String track_title =
        track.track_index == 0 ? "Orchestrator" : ("Track " + juce::String(track.track_index));
    g.drawText(track_title, header.removeFromTop(18).reduced(8, 0), juce::Justification::centredLeft);

    const auto mute_bounds = track_mute_button_bounds(header_bounds).reduced(1);
    const auto solo_bounds = track_solo_button_bounds(header_bounds).reduced(1);
    g.setColour(track.muted ? juce::Colour::fromRGB(208, 96, 96) : juce::Colour::fromRGB(54, 57, 66));
    g.fillRoundedRectangle(mute_bounds.toFloat(), 3.0f);
    g.setColour(track.soloed ? juce::Colour::fromRGB(255, 194, 102) : juce::Colour::fromRGB(54, 57, 66));
    g.fillRoundedRectangle(solo_bounds.toFloat(), 3.0f);
    g.setColour(juce::Colours::white);
    g.drawFittedText("M", mute_bounds, juce::Justification::centred, 1);
    g.drawFittedText("S", solo_bounds, juce::Justification::centred, 1);
}

}  // namespace jucewrap
}  // namespace fftplugin
