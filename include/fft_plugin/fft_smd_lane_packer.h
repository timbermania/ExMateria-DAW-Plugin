#pragma once

// Cross-track lane packer for SMD game-target compile.
//
// Goal: when an FFT SMD compile produces more instrument tracks than the
// 23-track instrument budget (24 - 1 conductor), don't just drop whole
// lanes (Stage B). Instead, fold a lighter "guest" lane's events into
// silent windows of a heavier "host" lane, bracketing each insertion
// with the SMD state-bytes the guest needs (instrument, octave, dynamics,
// pan, reverb) and restoring the host's state on the way out.
//
// The packer treats every instrument track event-stream as a tick-stamped
// timeline. For each guest lane it walks all hosts looking for a free
// window covering the guest's note (the host's last note must have ended
// before the guest's note begins, and the host's next own note must
// start after the guest's note ends, with enough margin for any
// state-restore opcodes). When a window is found, the host's stream is
// rewritten to insert
//
//     [save host state] [set guest state] [guest note] [restore host state]
//
// at that point. The "save" is implicit — we just remember it. The
// "set guest state" only emits opcodes for state fields that actually
// differ from the host's current state. The "restore" only emits
// opcodes for fields that the host needs back at its next own event.
//
// If every event in a guest lane finds a packing site, the guest lane
// is removed entirely; otherwise the partial guest lane stays and only
// its packed events get pulled out.

#include <cstdint>
#include <optional>
#include <vector>

#include "fft_plugin/fft_smd_file.h"

namespace fftplugin {

struct FFTSmdLanePackerMergeRecord {
    int32_t guest_lane_index = -1;   // index in the input SMD before this merge
    int32_t host_lane_index = -1;
    int32_t notes_moved = 0;
    int32_t state_bytes_added = 0;
    int32_t guest_min_tick = 0;
    int32_t guest_max_tick = 0;
    int32_t blocks = 0;              // how many save/restore brackets were needed
};

struct FFTSmdLanePackerReport {
    int32_t lanes_in = 0;
    int32_t lanes_out = 0;
    int32_t lanes_fully_absorbed = 0;
    int32_t notes_packed = 0;
    int32_t state_change_bytes_added = 0;
    bool ok = false;
    std::string error;
    std::vector<FFTSmdLanePackerMergeRecord> merges;
};

// Returns the result of packing instrument tracks in `smd` (track 0
// untouched) so total `track_count` is at most `target_track_count`.
// On success returns a new FFTSmdFile; the input is not mutated.
//
// `target_track_count` includes the conductor track at index 0. So a
// 24-track engine cap means callers pass 24.
//
// If the packer can't reach the target (e.g., everything overlaps so
// nothing fits in any free window), returns the smd unchanged with
// ok=false and error populated. Callers should fall back to whole-track
// dropping (Stage B) in that case.
// `first_packable_track`: index of the first track eligible for packing.
// In the final FFTSmdFile (after the conductor track is prepended) this
// should be 1 to leave the conductor alone. Mid-pipeline, before the
// conductor has been prepended, pass 0 (every track is an instrument
// track and is fair game for packing).
FFTSmdFile pack_lanes_to_track_budget(
    const FFTSmdFile& smd,
    int32_t target_track_count,
    int32_t first_packable_track = 1,
    FFTSmdLanePackerReport* report = nullptr);

}  // namespace fftplugin
