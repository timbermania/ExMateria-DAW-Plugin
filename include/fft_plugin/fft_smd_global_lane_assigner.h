#pragma once

// Global linear-scan lane reassignment for compiled SMDs.
//
// Per-part `assign_poly_notes_to_voice_lanes` allocates lanes
// independently per source poly_track. Aerith for example has 23
// peak simultaneous voices across the whole song (verified via the
// regression driver's polyphony analysis), but per-part compile sums
// each part's local max simultaneity and produces ~45 lanes. The
// post-compile packer can recover some but not all of that.
//
// This pass runs AFTER the per-part compile and BEFORE the packer.
// It walks every compiled track, extracts each sounding note (with
// its trailing Fermatas) plus the per-track sticky state in effect
// at that note's tick, and assigns all notes globally to a minimum
// number of lanes via greedy linear scan with state-affinity
// tiebreaking. Each output lane carries notes from possibly many
// source parts, with state-flip opcodes (Instrument / Octave /
// Dynamics / Pan / Reverb) emitted between consecutive notes that
// have different state.
//
// Algorithm: Poletto & Sarkar 1999 linear-scan register allocation,
// generalized so the state-affinity score (lane already in the right
// state) is the tiebreaker among free lanes.
//
// Skips the conductor track (track 0) — its scaffold is built by the
// compile pipeline and shouldn't be touched.

#include <cstdint>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_file.h"

namespace fftplugin {

struct FFTSmdGlobalLaneAssignerReport {
    int32_t lanes_in = 0;
    int32_t lanes_out = 0;
    int32_t notes_assigned = 0;
    int32_t free_assignments = 0;     // assignments where state already matched
    int32_t state_flip_bytes = 0;     // total bytes spent on state-change opcodes
    bool ok = false;
    std::string error;
};

// `first_packable_track`: index of the first track to reassign. Pass
// 0 when called pre-conductor-prepend (every track is an instrument
// track), 1 when called post-prepend to leave the conductor alone.
FFTSmdFile global_lane_reassign(
    const FFTSmdFile& smd,
    int32_t max_lanes = 24,
    int32_t first_packable_track = 1,
    FFTSmdGlobalLaneAssignerReport* report = nullptr);

}  // namespace fftplugin
