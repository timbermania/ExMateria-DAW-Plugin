#pragma once

#include <optional>
#include <string>

#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

class FFTFilePlaybackEngine;
struct FFTPluginState;

// Bridges the authoring document (FFTSmdAuthoringDocument in plugin state)
// with the playback engine. Owns the load/save side of the
// authoring-doc lifecycle: pulling the doc off disk, converting compiled
// output into engine-friendly form, persisting back to .fftauth.
//
// Cross-cutting orchestrators (finalize_successful_authored_edit,
// reload_from_state) still live on FFTPluginProcessorCore for now —
// they coordinate across this bridge, the track-control service, and
// transport state.
class FFTAuthoringBridge {
public:
    FFTAuthoringBridge(FFTPluginState& state, FFTFilePlaybackEngine& engine);

    // Make sure state_.smd_authoring has a value: if already loaded, no-op;
    // else load from state_.authoring_path; else import from the engine's
    // currently-loaded SMD. Returns false (with error_message set) when
    // none of those paths produces a doc.
    bool ensure_loaded(std::string* error_message = nullptr);

    // Load → compile → push compiled SMD into the engine. Used by
    // reload_from_state to wake the engine back up from saved state.
    bool load_engine_from_state(std::string* error_message = nullptr);

    // Persist state_.smd_authoring back to state_.authoring_path. Returns
    // false when there's nothing to save (no doc / no path).
    bool save_to_disk(std::string* error_message = nullptr) const;

    // Build a fresh authoring document by importing the engine's currently-
    // loaded SMD. Returns nullopt when the engine has no SMD loaded.
    std::optional<FFTSmdAuthoringDocument> build_from_engine() const;

private:
    FFTPluginState* state_;
    FFTFilePlaybackEngine* engine_;
};

}  // namespace fftplugin
