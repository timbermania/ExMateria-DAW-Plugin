#include "FFTMidiImport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "fft_plugin/fft_instrument_catalog.h"
#include "fft_plugin/fft_midi_matching.h"
#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_smd_presentation_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fftplugin {
namespace jucewrap {

namespace {

constexpr int32_t kDefaultTempoBpm = 120;
constexpr int32_t kDefaultTimeSigNumerator = 4;
constexpr int32_t kDefaultTimeSigDenominator = 4;
constexpr int32_t kDefaultDynamics = 63;
constexpr int32_t kDefaultPan = 64;
constexpr int32_t kDefaultMelodicVelocity = 100;
constexpr int32_t kDefaultRestVelocity = 100;
constexpr int32_t kDefaultMelodicInstrument = 0;
constexpr int32_t kDefaultPercussionOctave = 5;
constexpr int32_t kDefaultPercussionRelativeKey = 0;

struct MidiTempoEvent {
    int32_t tick = 0;
    int32_t tempo_value = 102;
};

struct MidiTimeSignatureEvent {
    int32_t tick = 0;
    int32_t numerator = kDefaultTimeSigNumerator;
    int32_t denominator = kDefaultTimeSigDenominator;
};

struct ImportedNoteState {
    int32_t fft_instrument = kDefaultMelodicInstrument;
    int32_t octave = 5;
    int32_t relative_key = 0;
    int32_t velocity = kDefaultMelodicVelocity;
    int32_t dynamics = kDefaultDynamics;
    int32_t pan = kDefaultPan;
};

struct ImportedNote {
    int32_t source_track_index = 0;
    int32_t channel = 0;
    int32_t start_tick = 0;
    int32_t duration_ticks = 1;
    int32_t source_midi_note = 60;
    int32_t source_velocity = 100;
    int32_t source_volume = 100;
    int32_t source_pan = 64;
    int32_t source_expression = 127;
    ImportedNoteState state;
    bool percussion = false;
    int32_t source_program = 0;
    std::string source_name;
};

struct ImportedLayer {
    std::vector<ImportedNote> notes;
    std::string name;
};

struct SourceGroupKey {
    int32_t source_track_index = 0;
    int32_t channel = 0;
    int32_t percussion_instrument = -1;

    auto tie() const {
        return std::tie(source_track_index, channel, percussion_instrument);
    }

    bool operator<(const SourceGroupKey& other) const {
        return tie() < other.tie();
    }
};

struct TrackChannelState {
    int32_t bank = 0;
    int32_t program = 0;
    int32_t volume = 100;
    int32_t pan = 64;
    int32_t expression = 127;
};

struct ActiveNote {
    int32_t start_tick = 0;
    int32_t midi_note = 0;
    int32_t velocity = kDefaultMelodicVelocity;
    TrackChannelState state;
};

struct ImportedMidiData {
    std::vector<ImportedNote> notes;
    std::vector<MidiTempoEvent> tempo_events;
    std::vector<MidiTimeSignatureEvent> time_sig_events;
    int32_t total_ticks = 0;
    std::string song_title;
};

int32_t midi_velocity_to_smd(int32_t midi_velocity) {
    if (midi_velocity <= 0) {
        return 1;
    }
    return std::clamp(
        static_cast<int32_t>(std::lround((static_cast<double>(midi_velocity) * midi_velocity) / 127.0)),
        1,
        127);
}

int32_t cc7_to_smd_dynamics(int32_t cc7) {
    if (cc7 <= 0) {
        return 0;
    }
    return std::clamp(
        static_cast<int32_t>(std::lround((static_cast<double>(cc7) * cc7) / 127.0)),
        0,
        127);
}

int32_t bpm_to_fft_tempo(double bpm) {
    const double sanitized = std::isfinite(bpm) && bpm > 0.0 ? bpm : static_cast<double>(kDefaultTempoBpm);
    return std::clamp(static_cast<int32_t>(std::lround((sanitized * 218.0) / 256.0)), 1, 255);
}

int32_t scale_midi_tick(double midi_tick, double scale) {
    return std::max(0, static_cast<int32_t>(std::lround(midi_tick * scale)));
}

std::string fallback_track_name(int32_t source_track_index, int32_t channel) {
    std::ostringstream name;
    name << "Track " << (source_track_index + 1) << " Ch " << (channel + 1);
    return name.str();
}

std::string instrument_name_for_id(int32_t instrument_id) {
    const auto& catalog = fft_instrument_catalog();
    const auto it = std::find_if(
        catalog.begin(),
        catalog.end(),
        [instrument_id](const FFTInstrumentCatalogEntry& entry) {
            return entry.id == instrument_id;
        });
    if (it == catalog.end()) {
        return "Inst " + std::to_string(instrument_id);
    }
    return it->name.empty() ? ("Inst " + std::to_string(instrument_id)) : it->name;
}

int32_t authored_instrument_to_played_sample_id(int32_t instrument_id) {
    if (instrument_id >= 255) {
        return 255;
    }
    if (instrument_id < 0) {
        return instrument_id;
    }
    return instrument_id + 1;
}

std::optional<int32_t> played_sample_root_midi_note(int32_t played_sample_id) {
    const FFTInstrumentCatalogEntry* entry = find_fft_instrument_catalog_entry(played_sample_id);
    if (entry == nullptr || entry->root_midi_note < 0) {
        return std::nullopt;
    }
    return entry->root_midi_note;
}

bool instrument_is_effectively_null(int32_t instrument_id) {
    const FFTInstrumentCatalogEntry* entry = find_fft_instrument_catalog_entry(instrument_id);
    if (entry == nullptr) {
        return false;
    }
    return entry->is_null;
}

int32_t gm_program_to_fft_instrument(int32_t gm_program) {
    static constexpr std::array<int32_t, 128> kGmToFft = {
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
    if (gm_program < 0 || gm_program >= static_cast<int32_t>(kGmToFft.size())) {
        return smd_instrument_opcode_param_to_played_sample_id(86);
    }
    return smd_instrument_opcode_param_to_played_sample_id(kGmToFft[static_cast<size_t>(gm_program)]);
}

std::optional<std::string_view> octave_variant_group_for_program(int32_t gm_program) {
    switch (gm_program) {
    case 32: case 33: case 34: case 35: case 36: case 37: return "slap_bass";
    case 56: return "trumpet";
    case 57: return "trombone";
    case 58: return "tuba";
    case 60: return "french_horn";
    case 67: case 68: case 70: return "bassoon";
    case 71: case 73: case 74: case 75: return "clarinet";
    case 40: case 41: case 42: case 43: case 48: case 50: return "string_open";
    case 46: return "synth_str1";
    case 44: return "vibrato_str";
    case 47: return "timpani";
    case 0: case 1: case 2: case 3: case 4: case 5:
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        return "struck_string";
    case 49: case 51: case 88: case 89: case 90: case 91: case 92: case 93: case 94: case 95:
        return "synth_str";
    case 61: case 62: case 63: return "synth_brass";
    default: return std::nullopt;
    }
}

std::optional<std::string_view> preferred_octave_variant_group_for_program(int32_t gm_program) {
    switch (gm_program) {
    case 73:
        // Curated flute fallback: high sustained flute material fits the bassoon
        // family better than the clarinet family in current FFT content.
        return "bassoon";
    default:
        return octave_variant_group_for_program(gm_program);
    }
}

const std::vector<std::pair<int32_t, int32_t>>& octave_variants_for_group(std::string_view group) {
    static const std::unordered_map<std::string_view, std::vector<std::pair<int32_t, int32_t>>> kVariants {
        {"slap_bass", {{19, 12}, {20, 24}, {21, 36}, {22, 48}, {23, 60}}},
        {"trumpet", {{24, 36}, {25, 48}}},
        {"bassoon", {{28, 36}, {29, 48}, {30, 60}, {46, 48}, {47, 60}}},
        {"trombone", {{48, 24}, {49, 36}, {50, 48}, {79, 12}, {80, 24}, {81, 36}, {82, 48}, {83, 60}}},
        {"synth_brass", {{51, 36}, {52, 48}, {53, 60}}},
        {"french_horn", {{60, 24}, {61, 36}, {62, 48}, {63, 60}, {74, 36}, {75, 48}, {76, 60}, {77, 72}, {78, 84}}},
        {"clarinet", {{68, 36}, {70, 48}, {71, 60}}},
        {"timpani", {{66, 24}, {64, 36}}},
        {"struck_string", {{38, 24}, {39, 36}, {40, 48}, {41, 60}, {42, 72}}},
        {"synth_str", {{84, 24}, {85, 36}, {86, 48}, {87, 60}, {88, 72}, {89, 84}}},
        {"synth_str1", {{89, 24}, {90, 36}, {91, 48}, {92, 60}, {93, 72}}},
        {"string_open", {{143, 12}, {144, 24}, {145, 36}, {146, 48}, {147, 60}, {148, 72}}},
        {"vibrato_str", {{155, 24}, {156, 36}, {157, 48}, {158, 60}, {159, 72}}},
        {"tuba", {{43, 12}, {44, 24}, {7, 36}}},
    };
    static const std::vector<std::pair<int32_t, int32_t>> kEmpty;
    const auto it = kVariants.find(group);
    return it != kVariants.end() ? it->second : kEmpty;
}

std::vector<std::pair<int32_t, int32_t>> octave_variants_for_program(int32_t gm_program) {
    const auto group = preferred_octave_variant_group_for_program(gm_program);
    if (!group.has_value()) {
        return {};
    }

    const auto& variants = octave_variants_for_group(*group);
    std::vector<std::pair<int32_t, int32_t>> filtered(variants.begin(), variants.end());

    switch (gm_program) {
    case 60:
        // Curated Aerith horn rule: keep the french_horn family, but reject
        // the brighter/higher variants that make the imported horn rows sound
        // like they are blasting. Constrain the part to the lower horn subset
        // so uniform octave fitting lands on the softer C-3/C-4/C-5 samples.
        filtered.erase(
            std::remove_if(
                filtered.begin(),
                filtered.end(),
                [](const auto& candidate) {
                    return candidate.first == 63 ||
                        candidate.first == 74 ||
                        candidate.first == 75 ||
                        candidate.first == 76 ||
                        candidate.first == 77 ||
                        candidate.first == 78;
                }),
            filtered.end());
        break;
    case 49:
    case 51:
        // Curated Aerith string rule: keep the synth_str family, but reject
        // the two highest variants. They render about an octave high in the
        // problematic upper register and sound materially worse than 86/87.
        filtered.erase(
            std::remove_if(
                filtered.begin(),
                filtered.end(),
                [](const auto& candidate) {
                    return candidate.first == 88 || candidate.first == 89;
                }),
            filtered.end());
        break;
    case 46:
        // Curated Aerith harp rule: in this importer, the variant table stores
        // authored instrument ids, while the audible problem was observed on
        // played sample ids 93/94. Those correspond to authored ids 92/93.
        // Keep the synth_str1 family, but reject those top two variants so the
        // part shifts down onto the lower, less harsh samples.
        filtered.erase(
            std::remove_if(
                filtered.begin(),
                filtered.end(),
                [](const auto& candidate) {
                    return candidate.first == 92 || candidate.first == 93;
                }),
            filtered.end());
        break;
    default:
        break;
    }

    filtered.erase(
        std::remove_if(
            filtered.begin(),
            filtered.end(),
            [](const auto& candidate) {
                return instrument_is_effectively_null(candidate.first);
            }),
        filtered.end());

    return filtered.empty()
        ? std::vector<std::pair<int32_t, int32_t>>(variants.begin(), variants.end())
        : filtered;
}

int32_t root_midi_note_for_variant(const std::pair<int32_t, int32_t>& candidate) {
    const int32_t played_sample_id = authored_instrument_to_played_sample_id(candidate.first);
    if (const auto parsed = played_sample_root_midi_note(played_sample_id); parsed.has_value()) {
        return *parsed;
    }
    return candidate.second;
}

int32_t root_octave_for_variant(const std::pair<int32_t, int32_t>& candidate) {
    return std::max(0, root_midi_note_for_variant(candidate) / 12);
}

bool variant_allows_target_octave(
    const std::pair<int32_t, int32_t>& candidate,
    int32_t target_octave
) {
    const int32_t root_octave = root_octave_for_variant(candidate);
    return std::abs(target_octave - root_octave) <= 1;
}

int32_t choose_fft_instrument_for_note(int32_t gm_program, int32_t midi_note) {
    const int32_t target_octave = std::clamp(midi_note / 12, 0, 8);
    const auto variants = octave_variants_for_program(gm_program);
    if (!variants.empty()) {
        std::vector<std::pair<int32_t, int32_t>> octave_valid_variants;
        octave_valid_variants.reserve(variants.size());
        for (const auto& candidate : variants) {
            if (variant_allows_target_octave(candidate, target_octave)) {
                octave_valid_variants.push_back(candidate);
            }
        }
        const auto& candidate_pool = octave_valid_variants.empty() ? variants : octave_valid_variants;
        const auto best = std::min_element(
            candidate_pool.begin(),
            candidate_pool.end(),
            [midi_note](const auto& lhs, const auto& rhs) {
                return std::abs(root_midi_note_for_variant(lhs) - midi_note) <
                    std::abs(root_midi_note_for_variant(rhs) - midi_note);
            });
        return best->first;
    }
    const int32_t fallback = gm_program_to_fft_instrument(gm_program);
    return instrument_is_effectively_null(fallback) ? kDefaultMelodicInstrument : fallback;
}

std::optional<int32_t> choose_uniform_octave_shift_for_program(
    int32_t gm_program,
    const std::vector<int32_t>& midi_notes
) {
    const auto variants = octave_variants_for_program(gm_program);
    if (variants.empty() || midi_notes.empty()) {
        return 0;
    }

    const auto shift_cost = [&](int32_t shift) -> std::optional<int64_t> {
        int64_t total_cost = 0;
        for (const int32_t midi_note : midi_notes) {
            const int32_t shifted_note = midi_note + (shift * 12);
            if (shifted_note < 0 || shifted_note > 127) {
                return std::nullopt;
            }
            const int32_t shifted_octave = shifted_note / 12;
            int32_t best_distance = std::numeric_limits<int32_t>::max();
            for (const auto& candidate : variants) {
                if (!variant_allows_target_octave(candidate, shifted_octave)) {
                    continue;
                }
                best_distance = std::min(
                    best_distance,
                    std::abs(root_midi_note_for_variant(candidate) - shifted_note));
            }
            if (best_distance == std::numeric_limits<int32_t>::max()) {
                return std::nullopt;
            }
            total_cost += static_cast<int64_t>(best_distance);
        }
        return total_cost;
    };

    std::optional<int32_t> best_shift;
    std::optional<int64_t> best_cost;
    for (int32_t shift = -8; shift <= 8; ++shift) {
        const auto cost = shift_cost(shift);
        if (!cost.has_value()) {
            continue;
        }
        const bool replace =
            !best_cost.has_value() ||
            *cost < *best_cost ||
            (*cost == *best_cost &&
                (std::abs(shift) < std::abs(*best_shift) ||
                 (std::abs(shift) == std::abs(*best_shift) && shift < *best_shift)));
        if (replace) {
            best_shift = shift;
            best_cost = *cost;
        }
    }

    return best_shift;
}

int32_t choose_fft_instrument_for_octave_bucket(int32_t gm_program, int32_t octave) {
    const int32_t clamped_octave = std::clamp(octave, 0, 8);
    const int32_t bucket_root_note = clamped_octave * 12;
    return choose_fft_instrument_for_note(gm_program, bucket_root_note);
}

int32_t map_percussion_note_to_fft_instrument(int32_t midi_note) {
    switch (midi_note) {
    case 35:
    case 36:
        return 102;
    case 38:
    case 40:
        return 99;
    case 42:
    case 44:
        return 57;
    case 46:
    case 51:
        return 73;
    case 47:
    case 48:
    case 45:
    case 50:
        return 64;
    case 49:
    case 52:
    case 57:
        return 140;
    case 56:
        return 27;
    case 41:
    case 43:
        return 66;
    default:
        return 64;
    }
}

bool note_ranges_overlap(const ImportedNote& lhs, const ImportedNote& rhs) {
    const int32_t lhs_end = lhs.start_tick + lhs.duration_ticks;
    const int32_t rhs_end = rhs.start_tick + rhs.duration_ticks;
    return lhs.start_tick < rhs_end && rhs.start_tick < lhs_end;
}

bool can_place_note_in_layer(const ImportedLayer& layer, const ImportedNote& note) {
    for (auto it = layer.notes.rbegin(); it != layer.notes.rend(); ++it) {
        if (it->start_tick + it->duration_ticks <= note.start_tick) {
            break;
        }
        if (!note_ranges_overlap(*it, note)) {
            continue;
        }
        if (it->state.relative_key == note.state.relative_key) {
            return false;
        }
        if (it->state.fft_instrument != note.state.fft_instrument ||
            it->state.octave != note.state.octave ||
            it->state.dynamics != note.state.dynamics ||
            it->state.pan != note.state.pan) {
            return false;
        }
    }
    return true;
}

FFTSmdAuthoredOpcode make_opcode(
    int32_t tick,
    int32_t stack_order,
    int32_t opcode,
    std::vector<int32_t> params = {}
) {
    return FFTSmdAuthoredOpcode {
        .tick = tick,
        .stack_order = stack_order,
        .enabled = true,
        .exact_timing = false,
        .opcode = FFTSmdOpcodeEvent {
            .opcode = opcode,
            .params = std::move(params),
        },
    };
}

void renumber_stack_orders(std::vector<FFTSmdAuthoredOpcode>& opcodes) {
    std::stable_sort(
        opcodes.begin(),
        opcodes.end(),
        [](const FFTSmdAuthoredOpcode& lhs, const FFTSmdAuthoredOpcode& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.stack_order < rhs.stack_order;
        });

    int32_t current_tick = std::numeric_limits<int32_t>::min();
    int32_t stack_order = 0;
    for (auto& opcode : opcodes) {
        if (opcode.tick != current_tick) {
            current_tick = opcode.tick;
            stack_order = 0;
        }
        opcode.stack_order = stack_order++;
    }
}

std::vector<ImportedLayer> partition_group_into_layers(
    std::vector<ImportedNote> notes,
    const std::string& base_name
) {
    std::stable_sort(
        notes.begin(),
        notes.end(),
        [](const ImportedNote& lhs, const ImportedNote& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            if (lhs.state.octave != rhs.state.octave) {
                return lhs.state.octave < rhs.state.octave;
            }
            if (lhs.state.relative_key != rhs.state.relative_key) {
                return lhs.state.relative_key < rhs.state.relative_key;
            }
            return lhs.duration_ticks < rhs.duration_ticks;
        });

    std::vector<ImportedLayer> layers;
    for (const auto& note : notes) {
        auto layer_it = std::find_if(
            layers.begin(),
            layers.end(),
            [&note](const ImportedLayer& layer) {
                return can_place_note_in_layer(layer, note);
            });
        if (layer_it == layers.end()) {
            layers.push_back(ImportedLayer {});
            layer_it = std::prev(layers.end());
        }
        layer_it->notes.push_back(note);
    }

    for (size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
        layers[layer_index].name = layers.size() == 1
            ? base_name
            : (base_name + " " + std::to_string(layer_index + 1));
    }
    return layers;
}

void normalize_layer_octaves(ImportedLayer* layer) {
    if (layer == nullptr || layer->notes.empty()) {
        return;
    }

    const int32_t gm_program = layer->notes.front().source_program;
    std::vector<int32_t> midi_notes;
    midi_notes.reserve(layer->notes.size());
    bool sustained = false;
    for (const auto& note : layer->notes) {
        midi_notes.push_back(note.state.octave * 12 + note.state.relative_key);
        sustained = sustained || note.duration_ticks >= (kFftSmdPpq * 2);
    }

    const FFTMidiMatchPartFit fit = choose_fft_midi_part_fit(gm_program, midi_notes, sustained);
    const int32_t octave_shift = fit.valid ? fit.octave_shift : 0;
    for (auto& note : layer->notes) {
        const int32_t original_midi_note = note.state.octave * 12 + note.state.relative_key;
        const FFTMidiMatchCandidate candidate = choose_fft_midi_note_candidate(
            gm_program,
            original_midi_note,
            sustained,
            octave_shift,
            fit.group_key);
        const int32_t shifted_midi_note = std::clamp(candidate.shifted_midi_note, 0, 127);
        note.state.octave = std::clamp(shifted_midi_note / 12, 0, 8);
        note.state.relative_key = shifted_midi_note % 12;
        note.state.fft_instrument = candidate.valid
            ? candidate.played_sample_id
            : gm_program_to_fft_instrument(gm_program);
    }
}

FFTSmdAuthoringPart build_poly_part_from_layer(
    const ImportedLayer& layer,
    int32_t total_ticks
) {
    FFTSmdAuthoringPart part;
    part.kind = FFTSmdAuthoringPartKind::poly_track;
    part.name = layer.name;
    part.poly_track.total_ticks = total_ticks;

    std::vector<ImportedNote> notes = layer.notes;
    std::stable_sort(
        notes.begin(),
        notes.end(),
        [](const ImportedNote& lhs, const ImportedNote& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            if (lhs.state.relative_key != rhs.state.relative_key) {
                return lhs.state.relative_key < rhs.state.relative_key;
            }
            return lhs.duration_ticks < rhs.duration_ticks;
        });

    for (const auto& note : notes) {
        part.poly_track.notes.push_back(FFTSmdAuthoredPolyNote {
            .start_tick = note.start_tick,
            .total_ticks = note.duration_ticks,
            .base_ticks = note.duration_ticks,
            .velocity_hint = note.state.velocity,
            .relative_key = note.state.relative_key,
        });
    }

    if (notes.empty()) {
        return part;
    }

    std::vector<FFTSmdAuthoredOpcode> opcodes;
    opcodes.push_back(make_opcode(0, 0, 0xBA));

    ImportedNoteState current_state = notes.front().state;
    opcodes.push_back(make_opcode(0, 1, 0xE0, {current_state.dynamics}));
    opcodes.push_back(make_opcode(0, 2, 0xE8, {current_state.pan}));
    opcodes.push_back(make_opcode(0, 3, 0xAC, {smd_played_sample_id_to_instrument_opcode_param(current_state.fft_instrument)}));
    opcodes.push_back(make_opcode(0, 4, 0x94, {current_state.octave}));

    for (const auto& note : notes) {
        const int32_t tick = note.start_tick;
        if (note.state.dynamics != current_state.dynamics) {
            opcodes.push_back(make_opcode(tick, 0, 0xE0, {note.state.dynamics}));
            current_state.dynamics = note.state.dynamics;
        }
        if (note.state.pan != current_state.pan) {
            opcodes.push_back(make_opcode(tick, 0, 0xE8, {note.state.pan}));
            current_state.pan = note.state.pan;
        }
        if (note.state.fft_instrument != current_state.fft_instrument) {
            opcodes.push_back(make_opcode(tick, 0, 0xAC, {smd_played_sample_id_to_instrument_opcode_param(note.state.fft_instrument)}));
            current_state.fft_instrument = note.state.fft_instrument;
        }
        if (note.state.octave != current_state.octave) {
            opcodes.push_back(make_opcode(tick, 0, 0x94, {note.state.octave}));
            current_state.octave = note.state.octave;
        }
    }

    opcodes.push_back(make_opcode(total_ticks, 0, 0xAC, {255}));
    opcodes.push_back(make_opcode(total_ticks, 1, 0x90));
    renumber_stack_orders(opcodes);
    part.poly_track.opcodes = std::move(opcodes);
    return part;
}

FFTImportedMidiPartProvenance build_part_provenance_from_layer(const ImportedLayer& layer) {
    FFTImportedMidiPartProvenance provenance;
    provenance.part_name = layer.name;
    if (!layer.notes.empty()) {
        provenance.source_name = layer.notes.front().source_name;
        provenance.gm_program = layer.notes.front().source_program;
    }

    std::vector<ImportedNote> notes = layer.notes;
    std::stable_sort(
        notes.begin(),
        notes.end(),
        [](const ImportedNote& lhs, const ImportedNote& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            if (lhs.state.relative_key != rhs.state.relative_key) {
                return lhs.state.relative_key < rhs.state.relative_key;
            }
            return lhs.duration_ticks < rhs.duration_ticks;
        });

    provenance.notes_by_authored_index.reserve(notes.size());
    for (const auto& note : notes) {
        provenance.notes_by_authored_index.push_back(FFTImportedMidiNoteProvenance {
            .source_track_index = note.source_track_index,
            .channel = note.channel,
            .gm_program = note.source_program,
            .midi_note = note.source_midi_note,
            .velocity = note.source_velocity,
            .gm_volume = note.source_volume,
            .gm_pan = note.source_pan,
            .gm_expression = note.source_expression,
            .start_tick = note.start_tick,
            .duration_ticks = note.duration_ticks,
            .source_name = note.source_name,
        });
    }
    return provenance;
}

FFTSmdAuthoringPart build_conductor_part(
    const std::vector<MidiTempoEvent>& tempos,
    const std::vector<MidiTimeSignatureEvent>& time_sigs,
    int32_t total_ticks
) {
    FFTSmdAuthoringPart part;
    part.kind = FFTSmdAuthoringPartKind::raw_track;
    part.name = "Orchestral";
    part.raw_track.total_ticks = total_ticks;
    part.raw_track.spans.push_back(FFTSmdAuthoredSpan {
        .start_tick = 0,
        .total_ticks = total_ticks,
        .base_ticks = total_ticks,
        .velocity_hint = kDefaultRestVelocity,
        .relative_key = 13,
    });

    std::vector<FFTSmdAuthoredOpcode> opcodes;
    opcodes.push_back(make_opcode(0, 0, 0xBA));
    for (const auto& tempo : tempos) {
        opcodes.push_back(make_opcode(tempo.tick, 0, 0xA0, {tempo.tempo_value}));
    }
    for (const auto& sig : time_sigs) {
        opcodes.push_back(make_opcode(sig.tick, 0, 0x97, {sig.numerator, sig.denominator}));
    }
    opcodes.push_back(make_opcode(total_ticks, 0, 0x90));
    renumber_stack_orders(opcodes);
    part.raw_track.opcodes = std::move(opcodes);
    return part;
}

std::optional<ImportedMidiData> parse_midi_file(const juce::File& midi_file, std::string* error_message) {
    juce::FileInputStream input(midi_file);
    if (!input.openedOk()) {
        if (error_message != nullptr) {
            *error_message = "Cannot open MIDI file";
        }
        return std::nullopt;
    }

    juce::MidiFile midi;
    int midi_file_type = 1;
    if (!midi.readFrom(input, true, &midi_file_type)) {
        if (error_message != nullptr) {
            *error_message = "Failed to parse MIDI file";
        }
        return std::nullopt;
    }

    const int time_format = midi.getTimeFormat();
    if (time_format <= 0) {
        if (error_message != nullptr) {
            *error_message = "SMPTE-timed MIDI files are not supported yet";
        }
        return std::nullopt;
    }

    const double scale = static_cast<double>(kFftSmdPpq) / static_cast<double>(time_format);

    ImportedMidiData parsed;
    parsed.song_title = midi_file.getFileNameWithoutExtension().toStdString();

    juce::MidiMessageSequence tempo_sequence;
    midi.findAllTempoEvents(tempo_sequence);
    {
        std::map<int32_t, int32_t> tempos;
        for (int index = 0; index < tempo_sequence.getNumEvents(); ++index) {
            const auto* event = tempo_sequence.getEventPointer(index);
            if (event == nullptr || !event->message.isTempoMetaEvent()) {
                continue;
            }
            tempos[scale_midi_tick(event->message.getTimeStamp(), scale)] =
                bpm_to_fft_tempo(event->message.getTempoSecondsPerQuarterNote() > 0.0
                    ? (60.0 / event->message.getTempoSecondsPerQuarterNote())
                    : static_cast<double>(kDefaultTempoBpm));
        }
        if (tempos.empty()) {
            tempos[0] = bpm_to_fft_tempo(kDefaultTempoBpm);
        }
        for (const auto& [tick, value] : tempos) {
            parsed.tempo_events.push_back(MidiTempoEvent {.tick = tick, .tempo_value = value});
            parsed.total_ticks = std::max(parsed.total_ticks, tick);
        }
    }

    juce::MidiMessageSequence time_sig_sequence;
    midi.findAllTimeSigEvents(time_sig_sequence);
    {
        std::map<int32_t, std::pair<int32_t, int32_t>> time_sigs;
        for (int index = 0; index < time_sig_sequence.getNumEvents(); ++index) {
            const auto* event = time_sig_sequence.getEventPointer(index);
            if (event == nullptr || !event->message.isTimeSignatureMetaEvent()) {
                continue;
            }
            int numerator = kDefaultTimeSigNumerator;
            int denominator = kDefaultTimeSigDenominator;
            event->message.getTimeSignatureInfo(numerator, denominator);
            time_sigs[scale_midi_tick(event->message.getTimeStamp(), scale)] = {
                std::max(1, numerator),
                std::max(1, denominator)
            };
        }
        if (time_sigs.empty()) {
            time_sigs[0] = {kDefaultTimeSigNumerator, kDefaultTimeSigDenominator};
        }
        for (const auto& [tick, value] : time_sigs) {
            parsed.time_sig_events.push_back(MidiTimeSignatureEvent {
                .tick = tick,
                .numerator = value.first,
                .denominator = value.second,
            });
            parsed.total_ticks = std::max(parsed.total_ticks, tick);
        }
    }

    for (int track_index = 0; track_index < midi.getNumTracks(); ++track_index) {
        const auto* track = midi.getTrack(track_index);
        if (track == nullptr) {
            continue;
        }

        std::array<TrackChannelState, 16> channel_state {};
        for (auto& state : channel_state) {
            state.program = 0;
            state.volume = 100;
            state.pan = 64;
            state.bank = 0;
        }

        std::array<std::vector<ActiveNote>, 16 * 128> active_notes;
        std::string track_name;
        const int32_t track_end_tick = scale_midi_tick(track->getEndTime(), scale);
        parsed.total_ticks = std::max(parsed.total_ticks, track_end_tick);

        for (int event_index = 0; event_index < track->getNumEvents(); ++event_index) {
            const auto* holder = track->getEventPointer(event_index);
            if (holder == nullptr) {
                continue;
            }

            const auto& message = holder->message;
            const int32_t event_tick = scale_midi_tick(message.getTimeStamp(), scale);
            parsed.total_ticks = std::max(parsed.total_ticks, event_tick);

            if (message.isTrackNameEvent() && track_name.empty()) {
                track_name = message.getTextFromTextMetaEvent().toStdString();
                continue;
            }

            if (!message.isForChannel(1) && !message.isForChannel(2) && !message.isForChannel(3) &&
                !message.isForChannel(4) && !message.isForChannel(5) && !message.isForChannel(6) &&
                !message.isForChannel(7) && !message.isForChannel(8) && !message.isForChannel(9) &&
                !message.isForChannel(10) && !message.isForChannel(11) && !message.isForChannel(12) &&
                !message.isForChannel(13) && !message.isForChannel(14) && !message.isForChannel(15) &&
                !message.isForChannel(16)) {
                continue;
            }

            const int channel = message.getChannel() - 1;
            if (channel < 0 || channel >= 16) {
                continue;
            }

            auto& state = channel_state[static_cast<size_t>(channel)];
            if (message.isController()) {
                const int controller = message.getControllerNumber();
                if (controller == 0) {
                    state.bank = message.getControllerValue();
                } else if (controller == 7) {
                    state.volume = message.getControllerValue();
                } else if (controller == 11) {
                    state.expression = message.getControllerValue();
                } else if (controller == 10) {
                    state.pan = message.getControllerValue();
                }
                continue;
            }

            if (message.isProgramChange()) {
                state.program = message.getProgramChangeNumber() + (state.bank * 128);
                state.bank = 0;
                continue;
            }

            if (!message.isNoteOnOrOff()) {
                continue;
            }

            const int midi_note = message.getNoteNumber();
            if (midi_note < 0 || midi_note > 127) {
                continue;
            }

            auto& stack = active_notes[static_cast<size_t>(channel * 128 + midi_note)];
            if (message.isNoteOn()) {
                stack.push_back(ActiveNote {
                    .start_tick = event_tick,
                    .midi_note = midi_note,
                    .velocity = std::max(1, static_cast<int32_t>(message.getVelocity())),
                    .state = state,
                });
                continue;
            }

            if (stack.empty()) {
                continue;
            }

            const ActiveNote active = stack.back();
            stack.pop_back();
            const int32_t duration_ticks = std::max(1, event_tick - active.start_tick);
            const bool percussion = (channel == 9);
            ImportedNote imported;
            imported.source_track_index = track_index;
            imported.channel = channel;
            imported.start_tick = active.start_tick;
            imported.duration_ticks = duration_ticks;
            imported.source_midi_note = active.midi_note;
            imported.source_velocity = active.velocity;
            imported.source_volume = active.state.volume;
            imported.source_pan = active.state.pan;
            imported.source_expression = active.state.expression;
            imported.percussion = percussion;
            imported.source_program = active.state.program;
            imported.source_name = track_name.empty() ? fallback_track_name(track_index, channel) : track_name;

            if (percussion) {
                imported.state.fft_instrument = map_percussion_note_to_fft_instrument(active.midi_note);
                imported.state.octave = kDefaultPercussionOctave;
                imported.state.relative_key = kDefaultPercussionRelativeKey;
            } else {
                imported.state.octave = std::clamp(active.midi_note / 12, 0, 8);
                imported.state.fft_instrument = gm_program_to_fft_instrument(active.state.program);
                imported.state.relative_key = active.midi_note % 12;
            }

            imported.state.velocity = midi_velocity_to_smd(active.velocity);
            imported.state.dynamics = cc7_to_smd_dynamics(active.state.volume);
            imported.state.pan = std::clamp(active.state.pan, 0, 127);
            parsed.notes.push_back(imported);
            parsed.total_ticks = std::max(parsed.total_ticks, imported.start_tick + imported.duration_ticks);
        }
    }

    if (parsed.total_ticks <= 0) {
        parsed.total_ticks = kFftSmdPpq * 4;
    }
    parsed.total_ticks = std::max(1, parsed.total_ticks);

    return parsed;
}

}  // namespace

std::optional<FFTMidiImportResult> import_midi_file_to_authoring_result(
    const juce::File& midi_file,
    std::string* error_message
) {
    const auto parsed = parse_midi_file(midi_file, error_message);
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    FFTMidiImportResult result;
    FFTSmdAuthoringDocument& document = result.document;
    document.format_version = 4;
    document.initial_tempo = !parsed->tempo_events.empty() ? parsed->tempo_events.front().tempo_value : bpm_to_fft_tempo(kDefaultTempoBpm);
    document.initial_volume = 127;
    document.assoc_wds_id = 0;
    document.song_title = parsed->song_title;

    document.parts.push_back(build_conductor_part(parsed->tempo_events, parsed->time_sig_events, parsed->total_ticks));
    result.part_provenance.push_back(FFTImportedMidiPartProvenance {
        .part_name = "Orchestral",
    });

    std::map<SourceGroupKey, std::vector<ImportedNote>> grouped_notes;
    for (const auto& note : parsed->notes) {
        SourceGroupKey key {
            .source_track_index = note.source_track_index,
            .channel = note.channel,
            .percussion_instrument = note.percussion ? note.state.fft_instrument : -1,
        };
        grouped_notes[key].push_back(note);
    }

    for (auto& [key, notes] : grouped_notes) {
        if (notes.empty()) {
            continue;
        }

        std::string base_name = notes.front().source_name;
        if (key.channel == 9) {
            base_name += " - " + instrument_name_for_id(key.percussion_instrument);
        }

        std::vector<ImportedLayer> layers = partition_group_into_layers(std::move(notes), base_name);
        for (auto& layer : layers) {
            if (!layer.notes.empty() && !layer.notes.front().percussion) {
                normalize_layer_octaves(&layer);
            }
            document.parts.push_back(build_poly_part_from_layer(layer, parsed->total_ticks));
            result.part_provenance.push_back(build_part_provenance_from_layer(layer));
        }
    }

    if (document.parts.size() <= 1) {
        if (error_message != nullptr) {
            *error_message = "MIDI import produced no musical notes";
        }
        return std::nullopt;
    }

    document.track_count = compile_smd_authoring_document(document).smd.track_count;
    document.tracks.clear();
    for (const auto& part : document.parts) {
        if (part.kind == FFTSmdAuthoringPartKind::raw_track) {
            document.tracks.push_back(part.raw_track);
        }
    }
    return result;
}

std::optional<FFTSmdAuthoringDocument> import_midi_file_to_authoring_document(
    const juce::File& midi_file,
    std::string* error_message
) {
    const auto result = import_midi_file_to_authoring_result(midi_file, error_message);
    if (!result.has_value()) {
        return std::nullopt;
    }
    return result->document;
}

}  // namespace jucewrap
}  // namespace fftplugin
