#include "fft_plugin/fft_instrument_catalog.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace fftplugin {

namespace {

std::string trim_copy(std::string_view value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::string to_lower_copy(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::vector<std::string> split_tokens(const std::string& value) {
    std::istringstream stream(value);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

const std::vector<FFTInstrumentCatalogEntry>& parsed_catalog() {
    static const std::vector<FFTInstrumentCatalogEntry> catalog = {
#include "fft_instrument_catalog_generated.inc"
    };
    return catalog;
}

bool token_matches_entry(const FFTInstrumentCatalogEntry& entry, const std::string& token) {
    const std::string lowered_token = to_lower_copy(token);
    if (lowered_token.empty()) {
        return true;
    }

    const std::string id_text = std::to_string(entry.id);
    const std::string opcode_param_text = std::to_string(entry.opcode_param);
    const std::string hex_text = to_lower_copy(entry.hex);
    const std::string hex_prefixed = "0x" + hex_text;
    const std::string name = to_lower_copy(entry.name);
    const std::string minor = to_lower_copy(entry.minor_group);
    const std::string major = to_lower_copy(entry.major_group);
    const std::string dependency = to_lower_copy(entry.waveset_dependency);
    const std::string root_note_name = to_lower_copy(entry.root_note_name);
    const std::string root_midi_note = entry.root_midi_note >= 0 ? std::to_string(entry.root_midi_note) : std::string();
    const std::string root_octave = entry.root_octave >= 0 ? std::to_string(entry.root_octave) : std::string();

    return id_text.find(lowered_token) != std::string::npos ||
        opcode_param_text.find(lowered_token) != std::string::npos ||
        hex_text.find(lowered_token) != std::string::npos ||
        hex_prefixed.find(lowered_token) != std::string::npos ||
        name.find(lowered_token) != std::string::npos ||
        minor.find(lowered_token) != std::string::npos ||
        major.find(lowered_token) != std::string::npos ||
        dependency.find(lowered_token) != std::string::npos ||
        root_note_name.find(lowered_token) != std::string::npos ||
        root_midi_note.find(lowered_token) != std::string::npos ||
        root_octave.find(lowered_token) != std::string::npos;
}

int score_entry(const FFTInstrumentCatalogEntry& entry, const std::string& query) {
    const std::string lowered_query = to_lower_copy(query);
    const std::string id_text = std::to_string(entry.id);
    const std::string opcode_param_text = std::to_string(entry.opcode_param);
    const std::string hex_text = to_lower_copy(entry.hex);
    const std::string hex_prefixed = "0x" + hex_text;
    const std::string name = to_lower_copy(entry.name);
    const std::string minor = to_lower_copy(entry.minor_group);
    const std::string major = to_lower_copy(entry.major_group);
    const std::string dependency = to_lower_copy(entry.waveset_dependency);
    const std::string root_note_name = to_lower_copy(entry.root_note_name);
    const std::string root_midi_note = entry.root_midi_note >= 0 ? std::to_string(entry.root_midi_note) : std::string();
    const std::string root_octave = entry.root_octave >= 0 ? std::to_string(entry.root_octave) : std::string();

    int score = 0;
    if (lowered_query == id_text) {
        score += 1200;
    } else if (id_text.starts_with(lowered_query)) {
        score += 900;
    }
    if (lowered_query == opcode_param_text) {
        score += 1000;
    } else if (opcode_param_text.starts_with(lowered_query)) {
        score += 700;
    }
    if (lowered_query == hex_text || lowered_query == hex_prefixed) {
        score += 1150;
    } else if (hex_text.starts_with(lowered_query) || hex_prefixed.starts_with(lowered_query)) {
        score += 850;
    }
    if (name == lowered_query) {
        score += 1100;
    } else if (name.starts_with(lowered_query)) {
        score += 800;
    } else if (name.find(lowered_query) != std::string::npos) {
        score += 650;
    }
    if (!root_note_name.empty() && root_note_name == lowered_query) {
        score += 700;
    } else if (!root_note_name.empty() && root_note_name.find(lowered_query) != std::string::npos) {
        score += 300;
    }
    if (!root_midi_note.empty() && root_midi_note == lowered_query) {
        score += 500;
    }
    if (!root_octave.empty() && root_octave == lowered_query) {
        score += 300;
    }
    if (!minor.empty() && minor.find(lowered_query) != std::string::npos) {
        score += 240;
    }
    if (!major.empty() && major.find(lowered_query) != std::string::npos) {
        score += 220;
    }
    if (!dependency.empty() && dependency.find(lowered_query) != std::string::npos) {
        score += 160;
    }

    for (const auto& token : split_tokens(lowered_query)) {
        if (token.empty()) {
            continue;
        }
        if (name.find(token) != std::string::npos) {
            score += 120;
        } else if (major.find(token) != std::string::npos || minor.find(token) != std::string::npos) {
            score += 70;
        } else if (root_note_name.find(token) != std::string::npos) {
            score += 70;
        }
    }

    if (entry.is_null) {
        score -= 20;
    }
    return score;
}

}  // namespace

const std::vector<FFTInstrumentCatalogEntry>& fft_instrument_catalog() {
    return parsed_catalog();
}

const FFTInstrumentCatalogEntry* find_fft_instrument_catalog_entry(int32_t played_sample_id) {
    const auto& catalog = parsed_catalog();
    const auto it = std::find_if(
        catalog.begin(),
        catalog.end(),
        [played_sample_id](const FFTInstrumentCatalogEntry& entry) {
            return entry.id == played_sample_id;
        });
    return it == catalog.end() ? nullptr : &(*it);
}

std::vector<FFTInstrumentCatalogEntry> search_fft_instrument_catalog(
    const std::string& query,
    int32_t max_results
) {
    const auto& catalog = parsed_catalog();
    if (max_results <= 0) {
        return {};
    }

    const std::string trimmed_query = trim_copy(query);
    if (trimmed_query.empty()) {
        return std::vector<FFTInstrumentCatalogEntry>(
            catalog.begin(),
            catalog.begin() + std::min<int32_t>(max_results, static_cast<int32_t>(catalog.size())));
    }

    const auto tokens = split_tokens(to_lower_copy(trimmed_query));
    struct ScoredEntry {
        FFTInstrumentCatalogEntry entry;
        int score = 0;
    };
    std::vector<ScoredEntry> matches;
    matches.reserve(catalog.size());

    for (const auto& entry : catalog) {
        bool all_tokens_match = true;
        for (const auto& token : tokens) {
            if (!token_matches_entry(entry, token)) {
                all_tokens_match = false;
                break;
            }
        }
        if (!all_tokens_match) {
            continue;
        }

        matches.push_back(ScoredEntry {
            .entry = entry,
            .score = score_entry(entry, trimmed_query),
        });
    }

    std::stable_sort(matches.begin(), matches.end(), [](const ScoredEntry& lhs, const ScoredEntry& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.entry.id < rhs.entry.id;
    });

    std::vector<FFTInstrumentCatalogEntry> results;
    results.reserve(static_cast<size_t>(std::min<int32_t>(max_results, static_cast<int32_t>(matches.size()))));
    for (int32_t i = 0; i < max_results && i < static_cast<int32_t>(matches.size()); ++i) {
        results.push_back(std::move(matches[static_cast<size_t>(i)].entry));
    }
    return results;
}

}  // namespace fftplugin
