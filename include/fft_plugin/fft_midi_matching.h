#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fftplugin {

struct FFTMidiVariantGroupMember {
    std::string group_key;
    int32_t played_sample_id = 0;
};

struct FFTMidiMappingRule {
    int32_t gm_program = -1;
    std::string primary_group;
    std::vector<std::string> fallback_groups;
    std::vector<int32_t> forbidden_played_sample_ids;
    std::string notes;
};

struct FFTMidiMatchCandidate {
    int32_t played_sample_id = 0;
    std::string group_key;
    int32_t shifted_midi_note = 0;
    int32_t target_octave = 0;
    int32_t root_midi_note = -1;
    int32_t root_octave = -1;
    int32_t root_distance_semitones = 0;
    double repeat_tail_score = -1.0;
    double sustain_timbre_score = -1.0;
    double long_hold_risk_score = -1.0;
    double upper_mid_energy_ratio = -1.0;
    double presence_energy_ratio = -1.0;
    double mean_centroid_hz = -1.0;
    double total_score = 0.0;
    bool valid = false;
    std::string rejection_reason;
};

struct FFTMidiMatchPartFit {
    bool valid = false;
    int32_t gm_program = 0;
    std::string group_key;
    int32_t octave_shift = 0;
    bool sustained = false;
    std::vector<int32_t> candidate_played_sample_ids;
    double total_score = 0.0;
};

const std::vector<FFTMidiVariantGroupMember>& fft_midi_variant_groups();
const std::vector<FFTMidiMappingRule>& fft_midi_mapping_rules();
const FFTMidiMappingRule* find_fft_midi_mapping_rule(int32_t gm_program);
std::string fft_variant_group_for_played_sample_id(int32_t played_sample_id);
std::vector<int32_t> fft_played_samples_for_variant_group(std::string_view group_key);

FFTMidiMatchPartFit choose_fft_midi_part_fit(
    int32_t gm_program,
    const std::vector<int32_t>& midi_notes,
    bool sustained);

FFTMidiMatchCandidate choose_fft_midi_note_candidate(
    int32_t gm_program,
    int32_t midi_note,
    bool sustained,
    int32_t octave_shift = 0,
    std::string_view preferred_group = {});

std::vector<FFTMidiMatchCandidate> rank_fft_midi_note_candidates(
    int32_t gm_program,
    int32_t midi_note,
    bool sustained,
    int32_t octave_shift = 0,
    std::string_view preferred_group = {});

}  // namespace fftplugin
