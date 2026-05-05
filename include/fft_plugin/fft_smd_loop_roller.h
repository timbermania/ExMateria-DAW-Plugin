#pragma once

// Loop-rolling pass for compiled SMDs.
//
// When a track's event stream contains a phrase repeated K times in a
// row, replace the K copies with `[REPEAT 0x98, K] phrase [CODA 0x99]`.
// Net byte savings = (K - 1) * sizeof(phrase) - 3.
//
// Engine-supported in any track including instrument tracks
// (verified 2026-05-03 via the `--smoke-loop` smoke gate). Body of
// the loop can contain any opcode the engine handles, including
// instrument / pan / dynamics / octave changes — provided the body
// is "state-balanced": every sticky-state opcode inside the body has
// a matching inverse before CODA so iteration N+1 starts in the same
// state as iteration N.
//
// Algorithm: per track, walk windows of length W (powers of 2), find
// consecutive identical windows, pick the (start, W, K) that
// maximizes byte savings, splice in the Repeat/Coda pair. Iterate
// until no more profitable collapses.

#include <cstdint>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_file.h"

namespace fftplugin {

struct FFTSmdLoopRollerTrackRecord {
    int32_t track_index = 0;
    int32_t collapses = 0;        // number of REPEAT/CODA pairs inserted
    int32_t bytes_before = 0;
    int32_t bytes_after = 0;
};

struct FFTSmdLoopRollerReport {
    int32_t total_collapses = 0;
    int32_t total_bytes_saved = 0;
    bool ok = false;
    std::string error;
    std::vector<FFTSmdLoopRollerTrackRecord> per_track;
};

// Returns a copy of `smd` with each track's events compressed via
// REPEAT/CODA where profitable. Does not mutate the input.
FFTSmdFile roll_loops_in_smd(
    const FFTSmdFile& smd,
    FFTSmdLoopRollerReport* report = nullptr);

}  // namespace fftplugin
