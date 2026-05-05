#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <JuceHeader.h>

#include "FFTOpcodeFilterState.h"
#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {
namespace jucewrap {

enum class FFTTrackLaneViewMode {
    full,
    header,
    tracks,
};

class FFTTrackLaneView final : public juce::Component {
public:
    explicit FFTTrackLaneView(FFTTrackLaneViewMode mode = FFTTrackLaneViewMode::full);

    void set_presentation(FFTSmdSongPresentation presentation);
    void set_selected_track(int track_index);
    void set_multi_selected_tracks(std::vector<int32_t> track_indices);
    void set_track_selected_callback(std::function<void(int, bool additive)> callback);
    void set_track_mute_callback(std::function<void(int)> callback);
    void set_track_solo_callback(std::function<void(int)> callback);
    void set_command_selected_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_selection_toggled_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_secondary_click_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_delete_callback(std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_move_callback(std::function<void(int, FFTSmdLaneCommandBlock, FFTSmdLaneInsertAnchor)> callback);
    void set_note_delete_callback(std::function<void(int, int)> callback);
    void set_note_fermata_delete_callback(std::function<void(int, int)> callback);
    void set_note_match_lab_callback(std::function<void(int, int)> callback);
    void set_insert_anchor_callback(std::function<void(int, FFTSmdLaneInsertAnchor)> callback);
    void set_time_selection_callback(std::function<void(int32_t, int32_t)> callback);
    void set_zoom_callback(std::function<void(float, float)> callback);
    void set_pan_callback(std::function<void(int, int)> callback);
    void set_horizontal_scroll(int horizontal_scroll);
    void set_pixels_per_tick(float pixels_per_tick);
    void set_presentation_mode(fftplugin::FFTSmdPresentationMode mode);
    void set_global_playhead_tick(int32_t tick);
    void set_source_track_cursor_ticks(std::vector<int32_t> ticks);
    void set_selected_command(int track_index, int source_event_index, int authored_opcode_index, int opcode);
    void set_selected_commands(
        int track_index,
        const std::vector<int32_t>& source_event_indices,
        const std::vector<int32_t>& authored_opcode_indices);
    void set_time_selection(int32_t start_tick, int32_t end_tick);
    void set_command_filter_state(const FFTOpcodeFilterState& state);
    void set_note_snap_mode_index(int mode_index);
    float pixels_per_tick() const;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    int content_width() const;
    int content_height() const;
    int current_header_x() const;
    int current_timeline_origin_x() const;
    juce::Rectangle<int> ruler_timeline_bounds() const;
    float tick_to_x(int32_t tick) const;
    float x_to_tick(float x) const;
    juce::Rectangle<int> conductor_bounds() const;
    juce::Rectangle<int> track_row_bounds(int track_index) const;
    juce::Rectangle<int> track_mute_button_bounds(const juce::Rectangle<int>& header) const;
    juce::Rectangle<int> track_solo_button_bounds(const juce::Rectangle<int>& header) const;
    void draw_ruler(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void draw_conductor(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void draw_track_row(
        juce::Graphics& g,
        juce::Rectangle<int> bounds,
        const FFTSmdTrackLanePresentation& track
    );
    const FFTSmdGridSegment* grid_segment_for_tick(int32_t tick) const;
    int32_t snap_step_for_tick(int32_t tick) const;
    int32_t snap_tick_to_grid(const FFTSmdTrackLanePresentation& track, int32_t raw_tick) const;
    bool update_insert_preview(const juce::Point<float>& position, const juce::ModifierKeys& mods);
    void clear_insert_preview();

    struct DisplayedCommandChip {
        juce::Rectangle<int> bounds;
        int track_index = -1;
        FFTSmdLaneCommandBlock command;
    };

    struct DisplayedInsertAnchor {
        juce::Rectangle<int> bounds;
        int track_index = -1;
        FFTSmdLaneInsertAnchor anchor;
    };

    struct DisplayedNoteBlock {
        juce::Rectangle<int> bounds;
        int track_index = -1;
        FFTSmdLaneNoteBlock note;
    };

    struct DisplayedDiagnosticMarker {
        juce::Rectangle<int> bounds;
        int track_index = -1;
        int32_t tick = 0;
        FFTSmdLaneDiagnosticSeverity severity = FFTSmdLaneDiagnosticSeverity::error;
        juce::String message;
    };

    struct DraggedCommandChip {
        int track_index = -1;
        FFTSmdLaneCommandBlock command;
    };

    struct PendingCtrlCommandClick {
        int track_index = -1;
        FFTSmdLaneCommandBlock command;
        juce::Point<float> mouse_down_position;
    };

    struct TimeSelectionDraft {
        int32_t anchor_tick = 0;
        int32_t current_tick = 0;
    };

    FFTSmdSongPresentation presentation_;
    FFTTrackLaneViewMode mode_ = FFTTrackLaneViewMode::full;
    int selected_track_index_ = 0;
    std::vector<int32_t> multi_selected_track_indices_;
    int selected_command_track_index_ = -1;
    int selected_command_source_event_index_ = -1;
    int selected_command_authored_opcode_index_ = -1;
    int selected_command_opcode_ = -1;
    std::vector<int32_t> selected_command_source_event_indices_;
    std::vector<int32_t> selected_command_authored_opcode_indices_;
    int horizontal_scroll_ = 0;
    fftplugin::FFTSmdPresentationMode presentation_mode_ = fftplugin::FFTSmdPresentationMode::source;
    int32_t global_playhead_tick_ = -1;
    std::vector<int32_t> source_track_cursor_ticks_;
    std::function<void(int, bool additive)> track_selected_callback_;
    std::function<void(int)> track_mute_callback_;
    std::function<void(int)> track_solo_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_selected_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_selection_toggled_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_secondary_click_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_delete_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock, FFTSmdLaneInsertAnchor)> command_move_callback_;
    std::function<void(int, int)> note_delete_callback_;
    std::function<void(int, int)> note_fermata_delete_callback_;
    std::function<void(int, int)> note_match_lab_callback_;
    std::function<void(int, FFTSmdLaneInsertAnchor)> insert_anchor_callback_;
    std::function<void(int32_t, int32_t)> time_selection_callback_;
    std::function<void(float, float)> zoom_callback_;
    std::function<void(int, int)> pan_callback_;
    float pixels_per_tick_ = 0.7f;
    bool middle_drag_panning_ = false;
    juce::Point<float> pan_start_position_;
    std::vector<DisplayedCommandChip> visible_command_chips_;
    std::vector<DisplayedInsertAnchor> visible_insert_anchors_;
    std::vector<DisplayedNoteBlock> visible_note_blocks_;
    std::vector<DisplayedDiagnosticMarker> visible_diagnostic_markers_;
    std::optional<DisplayedInsertAnchor> insert_preview_anchor_;
    std::optional<DisplayedDiagnosticMarker> hovered_diagnostic_marker_;
    std::optional<DraggedCommandChip> active_command_drag_;
    std::optional<PendingCtrlCommandClick> pending_ctrl_command_click_;
    std::optional<TimeSelectionDraft> active_time_selection_draft_;
    bool insert_mode_active_ = false;
    juce::Point<float> last_mouse_position_;
    int note_snap_mode_index_ = 6;
    FFTOpcodeFilterState command_filter_state_;
    int32_t selected_time_start_tick_ = -1;
    int32_t selected_time_end_tick_ = -1;

    static constexpr int kHeaderWidth = 124;
    static constexpr int kTimelinePadding = 12;
    static constexpr int kRulerHeight = 42;
    static constexpr int kTrackRowHeight = 128;
    static constexpr int kOverviewOpcodeRows = 5;
    static constexpr int kInsertAnchorRadiusPx = 14;
};

}  // namespace jucewrap
}  // namespace fftplugin
