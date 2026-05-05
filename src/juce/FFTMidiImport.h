#pragma once

#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {
namespace jucewrap {

struct FFTImportedMidiNoteProvenance {
    int32_t source_track_index = 0;
    int32_t channel = 0;
    int32_t gm_program = 0;
    int32_t midi_note = 60;
    int32_t velocity = 100;
    int32_t gm_volume = 100;
    int32_t gm_pan = 64;
    int32_t gm_expression = 127;
    int32_t start_tick = 0;
    int32_t duration_ticks = 0;
    std::string source_name;
};

struct FFTImportedMidiPartProvenance {
    std::string part_name;
    std::string source_name;
    int32_t gm_program = -1;
    std::vector<FFTImportedMidiNoteProvenance> notes_by_authored_index;
};

struct FFTMidiImportResult {
    FFTSmdAuthoringDocument document;
    std::vector<FFTImportedMidiPartProvenance> part_provenance;
};

std::optional<FFTMidiImportResult> import_midi_file_to_authoring_result(
    const juce::File& midi_file,
    std::string* error_message = nullptr);

std::optional<FFTSmdAuthoringDocument> import_midi_file_to_authoring_document(
    const juce::File& midi_file,
    std::string* error_message = nullptr);

}  // namespace jucewrap
}  // namespace fftplugin
