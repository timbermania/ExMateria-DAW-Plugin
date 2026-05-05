#include "fft_plugin/fft_midi_matching.h"

#include "fft_plugin/fft_instrument_catalog.h"
#include "fft_plugin/fft_smd_presentation_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

namespace fftplugin {

namespace {

const std::vector<FFTMidiVariantGroupMember>& parsed_variant_groups() {
    static const std::vector<FFTMidiVariantGroupMember> groups = {
        #include "fft_variant_groups_generated.inc"
    };
    return groups;
}

const std::vector<FFTMidiMappingRule>& parsed_mapping_rules() {
    static const std::vector<FFTMidiMappingRule> rules = {
        #include "fft_gm_mapping_rules_generated.inc"
    };
    return rules;
}

int32_t legacy_base_played_sample_id_for_program(int32_t gm_program) {
    static constexpr std::array<int32_t, 128> kGmToFftOpcodeParam = {
        39, 39, 39, 40, 39, 39, 59, 101,
        59, 8, 101, 59, 59, 59, 18, 138,
        51, 52, 52, 86, 86, 86, 86, 86,
        39, 39, 40, 40, 40, 40, 40, 40,
        19, 20, 21, 21, 21, 21, 38, 38,
        145, 145, 144, 143, 155, 26, 92, 64,
        145, 86, 145, 86, 130, 132, 130, 92,
        25, 50, 7, 173, 62, 51, 52, 53,
        70, 70, 68, 28, 29, 62, 28, 70,
        71, 70, 70, 70, 70, 70, 70, 71,
        52, 86, 70, 52, 52, 130, 52, 52,
        86, 86, 86, 130, 86, 86, 86, 86,
        86, 86, 86, 86, 86, 130, 86, 86,
        40, 40, 40, 40, 70, 70, 145, 70,
        18, 59, 57, 72, 64, 64, 138, 73,
        40, 130, 10, 130, 9, 15, 130, 15
    };
    if (gm_program < 0 || gm_program >= static_cast<int32_t>(kGmToFftOpcodeParam.size())) {
        return 1;
    }
    return smd_instrument_opcode_param_to_played_sample_id(kGmToFftOpcodeParam[static_cast<size_t>(gm_program)]);
}

const FFTInstrumentCatalogEntry* catalog_entry(int32_t played_sample_id) {
    return find_fft_instrument_catalog_entry(played_sample_id);
}

bool sample_is_valid_for_target_octave(int32_t played_sample_id, int32_t target_octave) {
    const FFTInstrumentCatalogEntry* entry = catalog_entry(played_sample_id);
    if (entry == nullptr || entry->is_null || entry->root_octave < 0) {
        return false;
    }
    return std::abs(entry->root_octave - target_octave) <= 1;
}

double sample_score_for_note(
    int32_t played_sample_id,
    int32_t midi_note,
    bool sustained
) {
    const FFTInstrumentCatalogEntry* entry = catalog_entry(played_sample_id);
    if (entry == nullptr || entry->is_null || entry->root_midi_note < 0) {
        return std::numeric_limits<double>::infinity();
    }
    double score = static_cast<double>(std::abs(entry->root_midi_note - midi_note));
    if (sustained) {
        if (entry->long_hold_risk_score >= 0.0) {
            score += entry->long_hold_risk_score * 8.0;
        }
        if (entry->upper_mid_energy_ratio >= 0.0) {
            score += entry->upper_mid_energy_ratio * 18.0;
        }
        if (entry->presence_energy_ratio >= 0.0) {
            score += entry->presence_energy_ratio * 10.0;
        }
        if (entry->sustain_timbre_score >= 0.0) {
            score += entry->sustain_timbre_score * 6.0;
        }
    } else {
        if (entry->upper_mid_energy_ratio >= 0.0) {
            score += entry->upper_mid_energy_ratio * 3.0;
        }
    }
    return score;
}

std::vector<std::string> candidate_groups_for_program(int32_t gm_program) {
    std::vector<std::string> groups;
    if (const FFTMidiMappingRule* rule = find_fft_midi_mapping_rule(gm_program); rule != nullptr) {
        if (!rule->primary_group.empty()) {
            groups.push_back(rule->primary_group);
        }
        groups.insert(groups.end(), rule->fallback_groups.begin(), rule->fallback_groups.end());
    }

    const std::string base_group = fft_variant_group_for_played_sample_id(legacy_base_played_sample_id_for_program(gm_program));
    if (!base_group.empty() &&
        std::find(groups.begin(), groups.end(), base_group) == groups.end()) {
        groups.push_back(base_group);
    }
    return groups;
}

std::vector<int32_t> filtered_group_members_for_program(int32_t gm_program, std::string_view group_key) {
    std::vector<int32_t> members = fft_played_samples_for_variant_group(group_key);
    const FFTMidiMappingRule* rule = find_fft_midi_mapping_rule(gm_program);
    members.erase(
        std::remove_if(
            members.begin(),
            members.end(),
            [rule](int32_t played_sample_id) {
                const FFTInstrumentCatalogEntry* entry = catalog_entry(played_sample_id);
                if (entry == nullptr || entry->is_null) {
                    return true;
                }
                if (rule == nullptr) {
                    return false;
                }
                return std::find(
                           rule->forbidden_played_sample_ids.begin(),
                           rule->forbidden_played_sample_ids.end(),
                           played_sample_id) != rule->forbidden_played_sample_ids.end();
            }),
        members.end());
    return members;
}

FFTMidiMatchCandidate build_candidate(
    int32_t played_sample_id,
    std::string group_key,
    int32_t shifted_midi_note,
    bool sustained
) {
    FFTMidiMatchCandidate candidate;
    candidate.played_sample_id = played_sample_id;
    candidate.group_key = std::move(group_key);
    candidate.shifted_midi_note = shifted_midi_note;
    candidate.target_octave = std::clamp(shifted_midi_note / 12, 0, 10);

    const FFTInstrumentCatalogEntry* entry = catalog_entry(played_sample_id);
    if (entry == nullptr) {
        candidate.rejection_reason = "Missing catalog entry";
        candidate.total_score = std::numeric_limits<double>::infinity();
        return candidate;
    }

    candidate.root_midi_note = entry->root_midi_note;
    candidate.root_octave = entry->root_octave;
    candidate.repeat_tail_score = entry->repeat_tail_score;
    candidate.sustain_timbre_score = entry->sustain_timbre_score;
    candidate.long_hold_risk_score = entry->long_hold_risk_score;
    candidate.upper_mid_energy_ratio = entry->upper_mid_energy_ratio;
    candidate.presence_energy_ratio = entry->presence_energy_ratio;
    candidate.mean_centroid_hz = entry->mean_centroid_hz;

    if (entry->is_null) {
        candidate.rejection_reason = "Null/silent sample";
        candidate.total_score = std::numeric_limits<double>::infinity();
        return candidate;
    }
    if (entry->root_midi_note < 0 || entry->root_octave < 0) {
        candidate.rejection_reason = "Missing root-note metadata";
        candidate.total_score = std::numeric_limits<double>::infinity();
        return candidate;
    }
    if (!sample_is_valid_for_target_octave(played_sample_id, candidate.target_octave)) {
        candidate.rejection_reason = "Outside root ±1 octave";
        candidate.total_score = std::numeric_limits<double>::infinity();
        return candidate;
    }

    candidate.valid = true;
    candidate.root_distance_semitones = std::abs(candidate.root_midi_note - shifted_midi_note);
    candidate.total_score = sample_score_for_note(played_sample_id, shifted_midi_note, sustained);
    return candidate;
}

}  // namespace

const std::vector<FFTMidiVariantGroupMember>& fft_midi_variant_groups() {
    return parsed_variant_groups();
}

const std::vector<FFTMidiMappingRule>& fft_midi_mapping_rules() {
    return parsed_mapping_rules();
}

const FFTMidiMappingRule* find_fft_midi_mapping_rule(int32_t gm_program) {
    const auto& rules = parsed_mapping_rules();
    const auto it = std::find_if(
        rules.begin(),
        rules.end(),
        [gm_program](const FFTMidiMappingRule& rule) {
            return rule.gm_program == gm_program;
        });
    return it == rules.end() ? nullptr : &(*it);
}

std::string fft_variant_group_for_played_sample_id(int32_t played_sample_id) {
    const auto& groups = parsed_variant_groups();
    const auto it = std::find_if(
        groups.begin(),
        groups.end(),
        [played_sample_id](const FFTMidiVariantGroupMember& member) {
            return member.played_sample_id == played_sample_id;
        });
    return it == groups.end() ? std::string() : it->group_key;
}

std::vector<int32_t> fft_played_samples_for_variant_group(std::string_view group_key) {
    std::vector<int32_t> matches;
    for (const auto& member : parsed_variant_groups()) {
        if (member.group_key == group_key) {
            matches.push_back(member.played_sample_id);
        }
    }
    return matches;
}

FFTMidiMatchPartFit choose_fft_midi_part_fit(
    int32_t gm_program,
    const std::vector<int32_t>& midi_notes,
    bool sustained
) {
    FFTMidiMatchPartFit best_fit;
    best_fit.gm_program = gm_program;
    best_fit.sustained = sustained;

    if (midi_notes.empty()) {
        best_fit.valid = true;
        return best_fit;
    }

    const auto groups = candidate_groups_for_program(gm_program);
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto& group : groups) {
        const auto members = filtered_group_members_for_program(gm_program, group);
        if (members.empty()) {
            continue;
        }
        for (int32_t octave_shift = -8; octave_shift <= 8; ++octave_shift) {
            double total = 0.0;
            bool valid = true;
            for (const int32_t midi_note : midi_notes) {
                const int32_t shifted = midi_note + octave_shift * 12;
                if (shifted < 0 || shifted > 127) {
                    valid = false;
                    break;
                }
                double best_note_score = std::numeric_limits<double>::infinity();
                for (const int32_t played_sample_id : members) {
                    const auto candidate = build_candidate(played_sample_id, group, shifted, sustained);
                    if (!candidate.valid) {
                        continue;
                    }
                    best_note_score = std::min(best_note_score, candidate.total_score);
                }
                if (!std::isfinite(best_note_score)) {
                    valid = false;
                    break;
                }
                total += best_note_score;
            }

            if (!valid) {
                continue;
            }

            const bool replace =
                !best_fit.valid ||
                total < best_score ||
                (std::abs(total - best_score) < 1e-9 &&
                    (std::abs(octave_shift) < std::abs(best_fit.octave_shift) ||
                     (std::abs(octave_shift) == std::abs(best_fit.octave_shift) && octave_shift < best_fit.octave_shift)));
            if (replace) {
                best_fit.valid = true;
                best_fit.group_key = group;
                best_fit.octave_shift = octave_shift;
                best_fit.candidate_played_sample_ids = members;
                best_fit.total_score = total;
                best_score = total;
            }
        }
    }

    return best_fit;
}

std::vector<FFTMidiMatchCandidate> rank_fft_midi_note_candidates(
    int32_t gm_program,
    int32_t midi_note,
    bool sustained,
    int32_t octave_shift,
    std::string_view preferred_group
) {
    std::vector<std::string> groups;
    if (!preferred_group.empty()) {
        groups.push_back(std::string(preferred_group));
    } else {
        groups = candidate_groups_for_program(gm_program);
    }

    const int32_t shifted_note = midi_note + octave_shift * 12;
    std::vector<FFTMidiMatchCandidate> results;
    std::set<int32_t> seen;
    for (const auto& group : groups) {
        for (const int32_t played_sample_id : filtered_group_members_for_program(gm_program, group)) {
            if (!seen.insert(played_sample_id).second) {
                continue;
            }
            results.push_back(build_candidate(played_sample_id, group, shifted_note, sustained));
        }
    }

    std::stable_sort(
        results.begin(),
        results.end(),
        [](const FFTMidiMatchCandidate& lhs, const FFTMidiMatchCandidate& rhs) {
            if (lhs.valid != rhs.valid) {
                return lhs.valid > rhs.valid;
            }
            if (std::abs(lhs.total_score - rhs.total_score) > 1e-9) {
                return lhs.total_score < rhs.total_score;
            }
            return lhs.played_sample_id < rhs.played_sample_id;
        });
    return results;
}

FFTMidiMatchCandidate choose_fft_midi_note_candidate(
    int32_t gm_program,
    int32_t midi_note,
    bool sustained,
    int32_t octave_shift,
    std::string_view preferred_group
) {
    const auto ranked = rank_fft_midi_note_candidates(gm_program, midi_note, sustained, octave_shift, preferred_group);
    if (!ranked.empty()) {
        return ranked.front();
    }
    FFTMidiMatchCandidate candidate;
    candidate.played_sample_id = legacy_base_played_sample_id_for_program(gm_program);
    candidate.group_key = fft_variant_group_for_played_sample_id(candidate.played_sample_id);
    candidate.shifted_midi_note = midi_note + octave_shift * 12;
    candidate.target_octave = std::clamp(candidate.shifted_midi_note / 12, 0, 10);
    candidate.rejection_reason = "No ranked candidates";
    return candidate;
}

}  // namespace fftplugin
