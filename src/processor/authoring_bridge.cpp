#include "fft_plugin/processor/authoring_bridge.h"

#include "fft_plugin/fft_file_playback_engine.h"
#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_file.h"

namespace fftplugin {

FFTAuthoringBridge::FFTAuthoringBridge(FFTPluginState& state, FFTFilePlaybackEngine& engine)
    : state_(&state), engine_(&engine) {}

bool FFTAuthoringBridge::ensure_loaded(std::string* error_message) {
    if (state_->smd_authoring.has_value()) {
        return true;
    }
    if (!state_->authoring_path.empty()) {
        std::string load_error;
        const auto authored = load_smd_authoring_document(state_->authoring_path, &load_error);
        if (!authored.has_value()) {
            if (error_message != nullptr) {
                *error_message = load_error;
            }
            return false;
        }
        state_->smd_authoring = std::move(authored);
        return true;
    }

    if (auto authored = build_from_engine(); authored.has_value()) {
        state_->smd_authoring = std::move(authored);
        return true;
    }

    if (error_message != nullptr) {
        *error_message = "No authored SMD document available";
    }
    return false;
}

bool FFTAuthoringBridge::load_engine_from_state(std::string* error_message) {
    if (!state_->smd_authoring.has_value()) {
        if (state_->authoring_path.empty()) {
            if (error_message != nullptr) {
                *error_message = "No authored SMD document available";
            }
            return false;
        }
        std::string load_error;
        auto authored = load_smd_authoring_document(state_->authoring_path, &load_error);
        if (!authored.has_value()) {
            if (error_message != nullptr) {
                *error_message = load_error;
            }
            return false;
        }
        state_->smd_authoring = std::move(authored);
    }

    const FFTSmdCompiledDocument compiled = compile_smd_authoring_document(*state_->smd_authoring);
    return engine_->load_compiled_smd_document(compiled, state_->smd_path, error_message);
}

bool FFTAuthoringBridge::save_to_disk(std::string* error_message) const {
    if (!state_->smd_authoring.has_value()) {
        if (error_message != nullptr) {
            *error_message = "No authored document to save";
        }
        return false;
    }
    if (state_->authoring_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "No authoring document path selected";
        }
        return false;
    }
    return save_smd_authoring_document(state_->authoring_path, *state_->smd_authoring, error_message);
}

std::optional<FFTSmdAuthoringDocument> FFTAuthoringBridge::build_from_engine() const {
    const auto& smd = engine_->smd_file();
    if (!smd.has_value()) {
        return std::nullopt;
    }
    return import_smd_authoring_document(
        *smd,
        [this](int32_t track_idx, int32_t source_event_index) {
            return engine_->track_source_opcode_disabled(track_idx, source_event_index);
        });
}

}  // namespace fftplugin
