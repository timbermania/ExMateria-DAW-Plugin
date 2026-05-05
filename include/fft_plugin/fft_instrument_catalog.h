#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fftplugin {

struct FFTInstrumentCatalogEntry {
    int32_t id = 0;
    std::string hex;
    int32_t opcode_param = 0;
    std::string name;
    std::string minor_group;
    std::string major_group;
    std::string waveset_dependency;
    std::string root_note_name;
    int32_t root_midi_note = -1;
    int32_t root_octave = -1;
    bool is_null = false;
    double repeat_tail_score = -1.0;
    double sustain_timbre_score = -1.0;
    double long_hold_risk_score = -1.0;
    double upper_mid_energy_ratio = -1.0;
    double presence_energy_ratio = -1.0;
    double high_freq_energy_ratio = -1.0;
    double mean_centroid_hz = -1.0;
};

const std::vector<FFTInstrumentCatalogEntry>& fft_instrument_catalog();
const FFTInstrumentCatalogEntry* find_fft_instrument_catalog_entry(int32_t played_sample_id);
std::vector<FFTInstrumentCatalogEntry> search_fft_instrument_catalog(
    const std::string& query,
    int32_t max_results = 64);

}  // namespace fftplugin
