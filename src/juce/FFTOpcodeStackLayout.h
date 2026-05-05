#pragma once

#include <functional>
#include <vector>

#include <JuceHeader.h>

#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {
namespace jucewrap {

struct FFTCommandChipPlacement {
    size_t command_index = 0;
    juce::Rectangle<int> bounds;
    int row = 0;
};

std::vector<FFTCommandChipPlacement> layout_command_chips(
    const std::vector<FFTSmdLaneCommandBlock>& commands,
    const std::function<bool(const FFTSmdLaneCommandBlock&)>& include_command,
    const std::function<float(int32_t)>& tick_to_x,
    juce::Rectangle<int> lane_bounds,
    int row_count,
    const juce::Font& font);

}  // namespace jucewrap
}  // namespace fftplugin
