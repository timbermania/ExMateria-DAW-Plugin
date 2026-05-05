#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

class FFTAuthoringBridge;
struct FFTPluginState;

// Owns the raw <-> poly track grouping operations: pack one or more raw
// tracks into a single PolyTrack and reverse it. Held by
// FFTPluginProcessorCore. Cross-cutting finalize stays on the processor
// (passed in as a callback).
class FFTTrackGroupingService {
public:
    using FinalizeCallback = std::function<bool(const FFTSmdCompiledDocument&, std::string*)>;

    FFTTrackGroupingService(FFTPluginState& state, FFTAuthoringBridge& bridge, FinalizeCallback finalize);

    // Pack the named raw tracks into a single PolyTrack and replace them
    // in the authoring document. All selected tracks must be raw_track
    // and must share a compatible structural timeline.
    bool convert_to_poly(const std::vector<int32_t>& track_indices, std::string* error_message);

    // Reverse: explode a PolyTrack back into raw tracks (one per voice
    // lane). Replaces the poly part with the resulting raw parts.
    bool ungroup(int32_t track_idx, std::string* error_message);

    // True when the part at track_idx is a poly_track (the authored doc
    // is the source of truth, so this doesn't need ensure_loaded).
    bool is_poly(int32_t track_idx) const;

private:
    FFTPluginState* state_;
    FFTAuthoringBridge* bridge_;
    FinalizeCallback finalize_;
};

}  // namespace fftplugin
