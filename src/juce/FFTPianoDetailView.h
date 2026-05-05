#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <JuceHeader.h>

#include "FFTOpcodeFilterState.h"
#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {
namespace jucewrap {

class FFTPianoDetailView final : public juce::Component {
public:
    FFTPianoDetailView();

    void set_presentation(FFTSmdSongPresentation presentation, int selected_track_index);
    void set_selected_command(int track_index, int source_event_index, int authored_opcode_index, int opcode);
    void set_selected_commands(
        int track_index,
        const std::vector<int32_t>& source_event_indices,
        const std::vector<int32_t>& authored_opcode_indices);
    void set_horizontal_scroll(int horizontal_scroll);
    void set_pixels_per_tick(float pixels_per_tick);
    void set_zoom_callback(std::function<void(float, float)> callback);
    void set_pan_callback(std::function<void(int, int)> callback);
    void set_command_selected_callback(
        std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_selection_toggled_callback(
        std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_secondary_click_callback(
        std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_delete_callback(
        std::function<void(int, FFTSmdLaneCommandBlock)> callback);
    void set_command_move_callback(
        std::function<void(int, FFTSmdLaneCommandBlock, FFTSmdLaneInsertAnchor)> callback);
    void set_note_delete_callback(std::function<void(int, int)> callback);
    void set_note_fermata_delete_callback(std::function<void(int, int)> callback);
    void set_note_match_lab_callback(std::function<void(int, int)> callback);
    void set_note_resize_callback(std::function<void(int, int, int32_t, int32_t, int32_t)> callback);
    void set_note_insert_callback(std::function<void(int, int, int, int32_t, int32_t, int32_t)> callback);
    void set_note_fermata_callback(std::function<void(int, int, int32_t)> callback);
    void set_rest_selected_callback(std::function<void(int, int, int)> callback);
    void set_rest_ripple_resize_callback(std::function<void(int, int, int, int32_t)> callback);
    void set_selected_rest(int track_index, int source_event_index, int authored_span_index);
    void set_insert_anchor_callback(
        std::function<void(int, FFTSmdLaneInsertAnchor)> callback);
    void set_time_selection(int32_t start_tick, int32_t end_tick);
    void set_filter_changed_callback(std::function<void(const FFTOpcodeFilterState&)> callback);
    void set_note_snap_mode_index(int mode_index);
    void set_note_snap_adjust_callback(std::function<void(int)> callback);
    FFTOpcodeFilterState filter_state() const;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    enum class OpcodeFilterCategory {
        instrument,
        dynamics,
        pan,
        tempo,
        adsr,
        octave,
        lfo,
        bend,
        detune,
        reverb,
        slur,
        percussion,
        structure,
    };

    struct DisplayedCommandChip {
        juce::Rectangle<int> bounds;
        FFTSmdLaneCommandBlock command;
    };

    struct DisplayedNoteBlock {
        juce::Rectangle<int> bounds;
        FFTSmdLaneNoteBlock note;
    };

    struct DisplayedInsertAnchor {
        juce::Rectangle<int> bounds;
        FFTSmdLaneInsertAnchor anchor;
    };

    struct DisplayedDiagnosticMarker {
        juce::Rectangle<int> bounds;
        int32_t tick = 0;
        FFTSmdLaneDiagnosticSeverity severity = FFTSmdLaneDiagnosticSeverity::error;
        juce::String message;
    };

    struct DraggedCommandChip {
        FFTSmdLaneCommandBlock command;
    };

    struct PendingCtrlCommandClick {
        FFTSmdLaneCommandBlock command;
        juce::Point<float> mouse_down_position;
    };

    struct NoteDraftPreview {
        int32_t source_event_index = -1;
        int32_t covered_start_tick = 0;
        int32_t covered_duration_ticks = 0;
        int32_t note_relative_key = 0;
        int32_t start_tick = 0;
        int32_t duration_ticks = 0;
        juce::Rectangle<int> bounds;
    };

    struct FermataDraftPreview {
        int32_t source_event_index = -1;
        int32_t note_start_tick = 0;
        int32_t note_base_duration_ticks = 0;
        int32_t extension_ticks = 0;
        juce::Rectangle<int> bounds;
    };

    struct ResizeDraftPreview {
        enum class Handle {
            start,
            note_end,
            total_end,
        };

        int32_t source_event_index = -1;
        int32_t authored_start_tick = -1;
        int32_t note_relative_key = 0;
        int32_t original_start_tick = 0;
        int32_t original_base_duration_ticks = 0;
        int32_t original_extension_ticks = 0;
        int32_t start_tick = 0;
        int32_t base_duration_ticks = 0;
        int32_t extension_ticks = 0;
        Handle handle = Handle::total_end;
        juce::Rectangle<int> total_bounds;
        juce::Rectangle<int> base_bounds;
        juce::Rectangle<int> extension_bounds;
    };

    struct FilterButtonTarget {
        enum class Kind {
            notes,
            rests,
            holds,
            instrument,
            dynamics,
            pan,
            tempo,
            adsr,
            octave,
            lfo,
            bend,
            detune,
            reverb,
            slur,
            percussion,
            structure,
        };

        juce::Rectangle<int> bounds;
        Kind kind = Kind::notes;
    };

    float tick_to_x(int32_t tick) const;
    float x_to_tick(float x) const;
    const FFTSmdTrackLanePresentation* selected_track() const;
    bool should_draw_command(const FFTSmdLaneCommandBlock& command) const;
    bool opcode_filter_active() const;
    bool opcode_filter_enabled(OpcodeFilterCategory category) const;
    void toggle_filter_button(FilterButtonTarget::Kind kind);
    bool try_handle_insert_anchor_click(const juce::MouseEvent& event);
    bool update_insert_preview(const juce::Point<float>& position, const juce::ModifierKeys& mods);
    void clear_insert_preview();
    int pitch_row_for_y(float y) const;
    const DisplayedNoteBlock* note_block_at(const juce::Point<float>& position, bool include_rests) const;
    const FFTSmdGridSegment* grid_segment_for_tick(int32_t tick) const;
    int32_t snap_step_for_tick(int32_t tick) const;
    int32_t snap_tick_to_grid(int32_t raw_tick) const;
    std::optional<NoteDraftPreview> build_note_preview(
        const juce::Point<float>& position,
        std::optional<int32_t> forced_relative_key = std::nullopt) const;
    void update_note_preview(const juce::Point<float>& position);
    void clear_note_preview();
    std::optional<FermataDraftPreview> build_fermata_preview(const juce::Point<float>& position) const;
    void update_fermata_preview(const juce::Point<float>& position);
    void clear_fermata_preview();
    std::optional<ResizeDraftPreview> build_resize_preview(const juce::Point<float>& position) const;
    void update_resize_preview(const juce::Point<float>& position);
    void clear_resize_preview();

    FFTSmdSongPresentation presentation_;
    int selected_track_index_ = 0;
    int selected_command_track_index_ = -1;
    int selected_command_source_event_index_ = -1;
    int selected_command_authored_opcode_index_ = -1;
    int selected_command_opcode_ = -1;
    std::vector<int32_t> selected_command_source_event_indices_;
    std::vector<int32_t> selected_command_authored_opcode_indices_;
    int horizontal_scroll_ = 0;
    float pixels_per_tick_ = 0.7f;
    std::function<void(float, float)> zoom_callback_;
    std::function<void(int, int)> pan_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_selected_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_selection_toggled_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_secondary_click_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock)> command_delete_callback_;
    std::function<void(int, FFTSmdLaneCommandBlock, FFTSmdLaneInsertAnchor)> command_move_callback_;
    std::function<void(int, int)> note_delete_callback_;
    std::function<void(int, int)> note_fermata_delete_callback_;
    std::function<void(int, int)> note_match_lab_callback_;
    std::function<void(int, int, int32_t, int32_t, int32_t)> note_resize_callback_;
    std::function<void(int, int, int, int32_t, int32_t, int32_t)> note_insert_callback_;
    std::function<void(int, int, int32_t)> note_fermata_callback_;
    std::function<void(int, int, int)> rest_selected_callback_;
    std::function<void(int, int, int, int32_t)> rest_ripple_resize_callback_;
    std::function<void(int, FFTSmdLaneInsertAnchor)> insert_anchor_callback_;
    std::function<void(const FFTOpcodeFilterState&)> filter_changed_callback_;
    std::function<void(int)> note_snap_adjust_callback_;
    bool middle_drag_panning_ = false;
    int32_t hovered_command_tick_ = -1;
    juce::Point<float> pan_start_position_;
    std::vector<DisplayedNoteBlock> visible_note_blocks_;
    std::vector<DisplayedCommandChip> visible_command_chips_;
    std::vector<DisplayedCommandChip> hover_overlay_command_chips_;
    std::vector<DisplayedDiagnosticMarker> visible_diagnostic_markers_;
    std::vector<DisplayedInsertAnchor> visible_insert_anchors_;
    std::vector<FilterButtonTarget> filter_button_targets_;
    std::optional<DisplayedInsertAnchor> insert_preview_anchor_;
    std::optional<DisplayedDiagnosticMarker> hovered_diagnostic_marker_;
    std::optional<DraggedCommandChip> active_command_drag_;
    std::optional<PendingCtrlCommandClick> pending_ctrl_command_click_;
    std::optional<NoteDraftPreview> note_preview_;
    std::optional<NoteDraftPreview> active_note_draft_;
    std::optional<FermataDraftPreview> fermata_preview_;
    std::optional<FermataDraftPreview> active_fermata_draft_;
    std::optional<ResizeDraftPreview> resize_preview_;
    std::optional<ResizeDraftPreview> active_resize_draft_;
    bool insert_mode_active_ = false;
    juce::Point<float> last_mouse_position_;
    int note_snap_mode_index_ = 6;
    bool show_note_commands_ = false;
    bool show_rest_commands_ = false;
    bool show_hold_commands_ = false;
    bool show_instrument_commands_ = false;
    bool show_dynamics_commands_ = false;
    bool show_pan_commands_ = false;
    bool show_tempo_commands_ = false;
    bool show_adsr_commands_ = false;
    bool show_octave_commands_ = false;
    bool show_lfo_commands_ = false;
    bool show_bend_commands_ = false;
    bool show_detune_commands_ = false;
    bool show_reverb_commands_ = false;
    bool show_slur_commands_ = false;
    bool show_percussion_commands_ = false;
    bool show_structure_commands_ = false;
    int32_t selected_time_start_tick_ = -1;
    int32_t selected_time_end_tick_ = -1;
    int selected_rest_track_index_ = -1;
    int selected_rest_source_event_index_ = -1;
    int selected_rest_authored_span_index_ = -1;

    static constexpr int kHeaderWidth = 124;
    static constexpr int kTimelinePadding = 12;
    static constexpr int kInsertAnchorRadiusPx = 14;
    static constexpr int kResizeHandleRadiusPx = 6;
};

}  // namespace jucewrap
}  // namespace fftplugin
