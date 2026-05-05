#include "FFTOpcodeStackLayout.h"

#include <algorithm>
#include <cmath>

namespace fftplugin {
namespace jucewrap {

std::vector<FFTCommandChipPlacement> layout_command_chips(
    const std::vector<FFTSmdLaneCommandBlock>& commands,
    const std::function<bool(const FFTSmdLaneCommandBlock&)>& include_command,
    const std::function<float(int32_t)>& tick_to_x,
    juce::Rectangle<int> lane_bounds,
    int row_count,
    const juce::Font& font
) {
    std::vector<FFTCommandChipPlacement> placements;
    if (row_count <= 0 || lane_bounds.getWidth() <= 0 || lane_bounds.getHeight() <= 0) {
        return placements;
    }

    const int row_height = std::max(16, lane_bounds.getHeight() / row_count);
    std::vector<int> row_rights(static_cast<size_t>(row_count), lane_bounds.getX() - 4);
    placements.reserve(commands.size());

    for (size_t command_index = 0; command_index < commands.size(); ++command_index) {
        const auto& command = commands[command_index];
        if (!include_command(command)) {
            continue;
        }

        const int x = static_cast<int>(std::round(tick_to_x(command.tick)));
        if (x < lane_bounds.getX() - 4 || x > lane_bounds.getRight() + 4) {
            continue;
        }

        const int chip_width = std::max(30, font.getStringWidth(command.label.c_str()) + 10);
        int chosen_row = -1;
        for (int row = 0; row < row_count; ++row) {
            if (x >= row_rights[static_cast<size_t>(row)] + 4) {
                chosen_row = row;
                break;
            }
        }
        if (chosen_row < 0) {
            chosen_row = static_cast<int>(std::min_element(row_rights.begin(), row_rights.end()) - row_rights.begin());
        }

        const int chip_y = lane_bounds.getY() + chosen_row * row_height + 2;
        const int chip_height = std::max(12, row_height - 4);
        juce::Rectangle<int> bounds(x, chip_y, chip_width, chip_height);
        row_rights[static_cast<size_t>(chosen_row)] =
            std::max(row_rights[static_cast<size_t>(chosen_row)], bounds.getRight());
        placements.push_back(FFTCommandChipPlacement {
            .command_index = command_index,
            .bounds = bounds,
            .row = chosen_row,
        });
    }

    return placements;
}

}  // namespace jucewrap
}  // namespace fftplugin
