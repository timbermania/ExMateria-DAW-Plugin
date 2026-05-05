#pragma once

#include <cstdint>
#include <string>

namespace fftplugin {
namespace jucewrap {

struct FFTMatchLabSeed {
    bool has_midi_reference = false;
    int32_t gm_program = 0;
    int32_t gm_midi_note = 60;
    int32_t velocity = 100;
    int32_t gm_volume = 100;
    int32_t gm_pan = 64;
    int32_t gm_expression = 127;
    int32_t duration_ms = 1000;
    int32_t fft_played_sample_id = 0;
    int32_t fft_midi_note = 60;
    int32_t dynamics = 63;
    int32_t pan = 64;
    std::string source_name;
    std::string editor_track_name;
};

}  // namespace jucewrap
}  // namespace fftplugin
