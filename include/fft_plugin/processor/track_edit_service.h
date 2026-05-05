#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

class FFTAuthoringBridge;
struct FFTPluginState;

// Owns the leaf "set one or more opcode parameters" edits. The processor
// (and other services) compose these via the public set_track_*_opcode_value
// surface; each public wrapper is a one-line forwarder to set_param /
// set_param_values here.
//
// The service does NOT own the cross-cutting "finalize" step (which writes
// the new compiled doc back into the engine, saves the .fftauth, and
// replays mute/solo). The processor passes that in as a callback.
class FFTTrackEditService {
public:
    // Callback signature: (compiled doc, error_message_out) -> success.
    // The processor binds this to its finalize_successful_authored_edit.
    using FinalizeCallback = std::function<bool(const FFTSmdCompiledDocument&, std::string*)>;

    FFTTrackEditService(FFTPluginState& state, FFTAuthoringBridge& bridge, FinalizeCallback finalize);

    // Resolve `(track_idx, source_event_index)` to an authored opcode,
    // verify it still matches `expected_opcode`, set its first parameter
    // to `value` (clamped to [min_value, max_value]), recompile, and
    // finalize. Used by all single-value setters.
    bool set_param(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        int32_t value,
        int32_t min_value,
        int32_t max_value,
        std::string* error_message);

    // Same, but replaces the full param vector. Used by compound setters
    // (LFO, time signature, decay+sustain).
    bool set_param_values(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        const std::vector<int32_t>& values,
        std::string* error_message);

    // Read the per-track transposition (in semitones, clamped to ±36).
    // Returns 0 when no authored doc is loaded or the track is invalid.
    int32_t track_transposition(int32_t track_idx) const;

    // Set the per-track transposition. Recompiles + finalizes.
    bool set_track_transposition(int32_t track_idx, int32_t semitones, std::string* error_message);

    // Toggle the `enabled` flag on the authored opcode behind the given
    // compiled-track source event. Recompiles + finalizes.
    bool set_source_opcode_disabled(
        int32_t track_idx,
        int32_t source_event_index,
        bool disabled,
        std::string* error_message);

    // Replace the opcode byte (e.g. swap a Repeat for a Coda) on the
    // authored opcode behind a compiled source event. Asserts the
    // current value matches expected_opcode before swapping.
    bool set_opcode_code(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        int32_t new_opcode,
        std::string* error_message);

    // Erase the authored opcode behind a compiled source event.
    bool delete_opcode_by_source_event(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        std::string* error_message);

    // Erase the authored opcode by its direct authored-list index. Used
    // by poly-track UIs that already work in authored space.
    bool delete_opcode_by_authored_index(
        int32_t track_idx,
        int32_t authored_opcode_index,
        int32_t expected_opcode,
        std::string* error_message);

    // Append a new authored opcode at target_tick / insertion_sequence_index.
    // Returns the inserted source-event-index in *inserted (when non-null)
    // for raw tracks, or the authored index for poly tracks.
    bool insert_opcode(
        int32_t track_idx,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        int32_t opcode,
        const std::vector<int32_t>& params,
        int32_t* inserted_source_event_index,
        std::string* error_message);

    // Move a single authored opcode (resolved via compiled source event)
    // to target_tick + insertion_sequence_index. Reorders within the
    // compiled timeline.
    bool move_opcode_by_source_event(
        int32_t track_idx,
        int32_t source_event_index,
        int32_t expected_opcode,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        int32_t* moved_source_event_index,
        std::string* error_message);

    // Move a single authored opcode by direct authored index. Used by
    // poly-track UIs.
    bool move_opcode_by_authored_index(
        int32_t track_idx,
        int32_t authored_opcode_index,
        int32_t expected_opcode,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        int32_t* moved_authored_opcode_index,
        std::string* error_message);

    // Batch-move a set of authored opcodes (raw track) resolved through
    // compiled source events. Preserves their relative ordering. The
    // dragged event determines the tick offset.
    bool move_opcodes_by_source_events(
        int32_t track_idx,
        const std::vector<int32_t>& source_event_indices,
        int32_t dragged_source_event_index,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        std::vector<int32_t>* moved_source_event_indices,
        int32_t* moved_dragged_source_event_index,
        std::string* error_message);

    // Batch-move authored opcodes by direct authored indices (poly track).
    bool move_opcodes_by_authored_indices(
        int32_t track_idx,
        const std::vector<int32_t>& authored_opcode_indices,
        int32_t dragged_authored_opcode_index,
        int32_t target_tick,
        int32_t insertion_sequence_index,
        std::vector<int32_t>* moved_authored_opcode_indices,
        int32_t* moved_dragged_authored_opcode_index,
        std::string* error_message);

private:
    FFTPluginState* state_;
    FFTAuthoringBridge* bridge_;
    FinalizeCallback finalize_;
};

}  // namespace fftplugin
