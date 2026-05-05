#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fft_plugin/fft_plugin_state.h"

namespace fftplugin {

std::vector<uint8_t> serialize_plugin_state_minimal(const FFTPluginState& state);
bool deserialize_plugin_state_minimal(
    const std::vector<uint8_t>& bytes,
    FFTPluginState* state,
    std::string* error_message = nullptr
);

}  // namespace fftplugin
