#include "fft_plugin/fft_smd_file.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fftplugin {

const std::array<int32_t, 19> kFftSmdDeltaTimeTable = {
    0, 192, 144, 96, 72, 64, 48, 36, 32, 24, 18, 16, 12, 9, 8, 6, 4, 3, 2,
};

namespace {

const std::unordered_map<uint8_t, std::pair<const char*, int32_t>> kOpcodeInfo = {
    {0x80, {"Rest", 1}}, {0x81, {"Fermata", 1}}, {0x82, {"NOP", 0}},
    {0x90, {"EndBar", 0}}, {0x91, {"Loop", 0}},
    {0x98, {"Repeat", 1}}, {0x99, {"Coda", 0}}, {0x9A, {"RepeatBreak", 0}},
    {0x94, {"Octave", 1}}, {0x95, {"RaiseOctave", 0}}, {0x96, {"LowerOctave", 0}},
    {0x97, {"TimeSignature", 2}}, {0xA0, {"Tempo", 1}}, {0xA2, {"TempoSlide", 2}},
    {0xAC, {"Instrument", 1}}, {0xAD, {"Unknown_AD", 1}},
    {0xAE, {"PercussionOn", 0}}, {0xAF, {"PercussionOff", 0}},
    {0xB0, {"SlurOn", 0}}, {0xB1, {"SlurOff", 0}},
    {0xBA, {"ReverbOn", 0}}, {0xBB, {"ReverbOff", 0}},
    {0xC0, {"ADSR_Reset", 0}}, {0xC2, {"ADSR_Attack", 1}},
    {0xC4, {"ADSR_SustainRate", 1}}, {0xC5, {"ADSR_Release", 1}},
    {0xC6, {"ADSR1_LowNibble_SlideTarget", 1}},
    {0xC7, {"ADSR_DecayAndSustainLevel", 2}},
    {0xC9, {"ADSR_Decay", 1}}, {0xCA, {"ADSR_SustainLevel", 1}},
    {0xD0, {"SetPitchBend", 1}}, {0xD2, {"ConditionalSeqFlag", 1}},
    {0xD6, {"Detune", 1}},
    {0xD7, {"LFO_Depth_Pitch", 1}}, {0xD8, {"LFO_Length_Pitch", 3}},
    {0xDA, {"FlagSet_0xFE", 0}}, {0xDB, {"FlagClear_0xFE", 0}},
    {0xE0, {"Dynamics", 1}}, {0xE3, {"LFO_Depth_Volume", 1}},
    {0xE4, {"LFO_Length_Volume", 3}},
    {0xE6, {"FlagSet_0x11E", 0}}, {0xE8, {"Pan", 1}},
    {0xFE, {"BankSelect", 1}},
};

const std::unordered_map<uint8_t, int32_t> kExtraOpcodes = {
    {0xA3, 2}, {0xA4, 1}, {0xA9, 1}, {0xB2, 0}, {0xB8, 3}, {0xB9, 1},
    {0xC1, 1}, {0xC6, 1}, {0xC7, 2}, {0xC8, 1}, {0xCF, 0},
    {0xD1, 1}, {0xD2, 1}, {0xD4, 2}, {0xD7, 1}, {0xD8, 3}, {0xD9, 3}, {0xDB, 0},
    {0xE1, 1}, {0xE2, 2}, {0xE3, 1}, {0xE4, 3}, {0xE5, 3}, {0xE7, 0},
    {0xE9, 1}, {0xEA, 2}, {0xEB, 1}, {0xEC, 3}, {0xED, 3}, {0xEF, 0},
    {0xF4, 1}, {0xF7, 1}, {0xF8, 3}, {0xF9, 2}, {0xFB, 1}, {0xFC, 2}, {0xFD, 1},
};

uint16_t read_u16_le(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
        (static_cast<uint16_t>(data[offset + 1]) << 8);
}

void write_u16_le(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
}

int32_t exact_delta_index(int32_t ticks) {
    for (size_t index = 1; index < kFftSmdDeltaTimeTable.size(); ++index) {
        if (kFftSmdDeltaTimeTable[index] == ticks) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

bool append_serialized_track_event(
    std::vector<uint8_t>& out,
    const FFTSmdTrackEvent& event,
    std::string* error_message
) {
    if (const auto* note = std::get_if<FFTSmdNoteEvent>(&event)) {
        if (note->velocity < 0 || note->velocity > 127) {
            if (error_message != nullptr) {
                *error_message = "SMD note velocity out of range";
            }
            return false;
        }
        if (note->relative_key < 0 || note->relative_key > 13) {
            if (error_message != nullptr) {
                *error_message = "SMD note relative key out of range";
            }
            return false;
        }
        if (note->delta_time <= 0 || note->delta_time > 255) {
            if (error_message != nullptr) {
                *error_message = "SMD note delta time out of range";
            }
            return false;
        }
        if (note->relative_key == 13) {
            if (error_message != nullptr) {
                *error_message = "SMD rests must be encoded with opcode 0x80";
            }
            return false;
        }

        // Symmetrical to the decoder: a note byte encodes
        // `relative_key * 19 + delta_index`. delta_index 1..18 names an
        // entry in kFftSmdDeltaTimeTable; delta_index 0 is the sentinel
        // for "read an extra literal-delta byte". Honor used_literal_
        // encoding so parse→reserialize round-trips exactly even when
        // the source picked the literal form for a table-aligned delta.
        const int32_t delta_index = exact_delta_index(note->delta_time);
        const bool emit_literal = note->used_literal_encoding || delta_index <= 0;
        out.push_back(static_cast<uint8_t>(note->velocity));
        if (emit_literal) {
            out.push_back(static_cast<uint8_t>(note->relative_key * 19));
            out.push_back(static_cast<uint8_t>(note->delta_time));
        } else {
            out.push_back(static_cast<uint8_t>(note->relative_key * 19 + delta_index));
        }
        return true;
    }

    const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&event);
    if (opcode == nullptr || opcode->opcode < 0 || opcode->opcode > 255) {
        if (error_message != nullptr) {
            *error_message = "Invalid SMD opcode event";
        }
        return false;
    }

    out.push_back(static_cast<uint8_t>(opcode->opcode));
    for (const int32_t value : opcode->params) {
        if (value < 0 || value > 255) {
            if (error_message != nullptr) {
                *error_message = "SMD opcode parameter out of range";
            }
            return false;
        }
        out.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

struct DecodedTrack {
    std::vector<FFTSmdTrackEvent> events;
    std::vector<uint8_t> trailing_bytes;  // bytes after END_BAR within track range
};

DecodedTrack decode_track(const std::vector<uint8_t>& data) {
    std::vector<FFTSmdTrackEvent> events;
    size_t pos = 0;

    while (pos < data.size()) {
        const uint8_t byte = data[pos++];
        if (byte < 0x80) {
            if (pos >= data.size()) {
                break;
            }

            const uint8_t data_byte = data[pos++];
            FFTSmdNoteEvent note;
            note.velocity = byte;
            note.relative_key = data_byte / 19;
            const int32_t delta_index = data_byte % 19;
            note.delta_time = kFftSmdDeltaTimeTable[static_cast<size_t>(delta_index)];
            if (delta_index == 0) {
                note.used_literal_encoding = true;
                if (pos < data.size()) {
                    note.delta_time = data[pos++];
                }
            }
            events.emplace_back(note);
            continue;
        }

        FFTSmdOpcodeEvent opcode_event;
        opcode_event.opcode = byte;
        const int32_t param_count = smd_opcode_param_count(byte);
        opcode_event.params.reserve(static_cast<size_t>(std::max(0, param_count)));
        for (int32_t i = 0; i < param_count && pos < data.size(); ++i) {
            opcode_event.params.push_back(data[pos++]);
        }
        events.emplace_back(opcode_event);

        if (byte == 0x90) {
            break;
        }
    }

    DecodedTrack result;
    result.events = std::move(events);
    if (pos < data.size()) {
        result.trailing_bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    }
    return result;
}

}  // namespace

std::optional<FFTSmdFile> load_smd_file(const std::string& path, std::string* error_message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error_message != nullptr) {
            *error_message = "Cannot open SMD: " + path;
        }
        return std::nullopt;
    }

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
    return parse_smd_bytes(data, error_message);
}

std::optional<FFTSmdFile> parse_smd_bytes(const std::vector<uint8_t>& data, std::string* error_message) {
    if (data.size() < 0x22) {
        if (error_message != nullptr) {
            *error_message = "SMD too small";
        }
        return std::nullopt;
    }

    if (data[0] != 's' || data[1] != 'm' || data[2] != 'd' || data[3] != 's') {
        if (error_message != nullptr) {
            *error_message = "Invalid SMD magic";
        }
        return std::nullopt;
    }

    FFTSmdFile smd;
    constexpr size_t kHeaderSize = 0x22;
    if (data.size() >= kHeaderSize) {
        smd.raw_header.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    }
    const int32_t file_size = read_u16_le(data, 0x08);
    smd.track_count = data[0x14];
    smd.assoc_wds_id = read_u16_le(data, 0x16);
    smd.initial_volume = data[0x18];
    smd.initial_tempo = data[0x1B];

    const int32_t song_title_ptr = read_u16_le(data, 0x1E);
    smd.title_offset_override = song_title_ptr;
    const int32_t track_ptr_table = 0x22;

    std::vector<int32_t> track_ptrs;
    track_ptrs.reserve(static_cast<size_t>(std::max(0, smd.track_count)));
    for (int32_t i = 0; i < smd.track_count; ++i) {
        const size_t offset = static_cast<size_t>(track_ptr_table + i * 2);
        if (offset + 2 > data.size()) {
            break;
        }
        track_ptrs.push_back(read_u16_le(data, offset));
    }

    if (song_title_ptr > 0 && song_title_ptr < static_cast<int32_t>(data.size())) {
        int32_t title_end = !track_ptrs.empty() ? track_ptrs.front() : static_cast<int32_t>(data.size());
        title_end = std::min(title_end, static_cast<int32_t>(data.size()));
        std::string title;
        for (int32_t i = song_title_ptr; i < title_end && data[static_cast<size_t>(i)] != 0; ++i) {
            title.push_back(static_cast<char>(data[static_cast<size_t>(i)]));
        }
        smd.song_title = title;

        // Capture the full title region bytes (title + null + any post-
        // title alignment padding) so the serializer can reproduce the
        // exact source layout — including alignment choices.
        smd.title_region_bytes.assign(
            data.begin() + static_cast<std::ptrdiff_t>(song_title_ptr),
            data.begin() + static_cast<std::ptrdiff_t>(title_end));
    }

    smd.track_events.reserve(track_ptrs.size());
    smd.track_trailing_bytes.reserve(track_ptrs.size());
    for (size_t i = 0; i < track_ptrs.size(); ++i) {
        int32_t begin = std::min(track_ptrs[i], static_cast<int32_t>(data.size()));
        int32_t end = 0;
        if (i + 1 < track_ptrs.size()) {
            end = track_ptrs[i + 1];
        } else if (file_size > 0) {
            end = file_size;
        } else {
            end = static_cast<int32_t>(data.size());
        }
        end = std::min(end, static_cast<int32_t>(data.size()));
        if (begin > end) {
            begin = end;
        }

        std::vector<uint8_t> track_data(
            data.begin() + static_cast<std::ptrdiff_t>(begin),
            data.begin() + static_cast<std::ptrdiff_t>(end)
        );
        auto decoded = decode_track(track_data);
        smd.track_events.push_back(std::move(decoded.events));
        smd.track_trailing_bytes.push_back(std::move(decoded.trailing_bytes));
    }

    return smd;
}

std::vector<uint8_t> serialize_smd_file(const FFTSmdFile& smd, std::string* error_message) {
    constexpr size_t kHeaderSize = 0x22;

    std::vector<std::vector<uint8_t>> encoded_tracks;
    encoded_tracks.reserve(smd.track_events.size());
    for (size_t track_idx = 0; track_idx < smd.track_events.size(); ++track_idx) {
        const auto& track = smd.track_events[track_idx];
        std::vector<uint8_t> encoded_track;
        for (const auto& event : track) {
            if (!append_serialized_track_event(encoded_track, event, error_message)) {
                return {};
            }
        }
        // Append parsed trailing bytes (post-END_BAR padding/leftovers)
        // when the source captured them, so parse->reserialize round-trips.
        if (track_idx < smd.track_trailing_bytes.size()) {
            const auto& trailing = smd.track_trailing_bytes[track_idx];
            encoded_track.insert(encoded_track.end(), trailing.begin(), trailing.end());
        }
        encoded_tracks.push_back(std::move(encoded_track));
    }

    const int32_t track_count = static_cast<int32_t>(encoded_tracks.size());
    if (track_count > 255) {
        if (error_message != nullptr) {
            *error_message = "SMD track count exceeds format limit";
        }
        return {};
    }

    // Title region: when the source captured exact bytes (round-trip
    // path), emit them verbatim. Otherwise build title + null and
    // even-align the post-title byte boundary.
    std::vector<uint8_t> title_bytes;
    if (!smd.title_region_bytes.empty()) {
        title_bytes = smd.title_region_bytes;
    } else {
        std::string title = smd.song_title;
        title.erase(std::remove(title.begin(), title.end(), '\0'), title.end());
        title_bytes.assign(title.begin(), title.end());
        title_bytes.push_back(0);
    }

    const size_t offset_table_size = static_cast<size_t>(track_count) * 2U;
    const uint16_t default_title_offset = static_cast<uint16_t>(kHeaderSize + offset_table_size);
    // Honor a parsed source title_offset (may be > default when the source
    // padded between offset table and title — see MUSIC_31).
    uint16_t title_offset = default_title_offset;
    size_t pre_title_pad = 0;
    if (smd.title_offset_override >= static_cast<int32_t>(default_title_offset) &&
        smd.title_offset_override <= std::numeric_limits<uint16_t>::max()) {
        title_offset = static_cast<uint16_t>(smd.title_offset_override);
        pre_title_pad = static_cast<size_t>(title_offset) - default_title_offset;
    }
    size_t data_start = static_cast<size_t>(title_offset) + title_bytes.size();
    // Synthesized title: even-align the post-title boundary. Captured
    // title_region_bytes already encodes the source's alignment choice.
    if (smd.title_region_bytes.empty() && (data_start & 1U) != 0U) {
        title_bytes.push_back(0);
        data_start += 1U;
    }

    std::vector<uint16_t> track_offsets;
    track_offsets.reserve(encoded_tracks.size());
    size_t cursor = data_start;
    for (const auto& encoded_track : encoded_tracks) {
        if (cursor > std::numeric_limits<uint16_t>::max()) {
            if (error_message != nullptr) {
                *error_message = "SMD exceeds 16-bit track offset range";
            }
            return {};
        }
        track_offsets.push_back(static_cast<uint16_t>(cursor));
        cursor += encoded_track.size();
    }
    if (cursor > std::numeric_limits<uint16_t>::max()) {
        if (error_message != nullptr) {
            *error_message = "SMD exceeds 16-bit file size range";
        }
        return {};
    }

    std::vector<uint8_t> out;
    out.reserve(cursor);
    // Start from the captured raw header when the file was parsed off
    // disc, otherwise synthesize a fresh header from known constants.
    // Either way we then overwrite the offsets we explicitly own below.
    if (smd.raw_header.size() == kHeaderSize) {
        out.assign(smd.raw_header.begin(), smd.raw_header.end());
    } else {
        out.resize(kHeaderSize, 0);
    }
    out[0] = 's';
    out[1] = 'm';
    out[2] = 'd';
    out[3] = 's';
    out[0x08] = static_cast<uint8_t>(cursor & 0xFFU);
    out[0x09] = static_cast<uint8_t>((cursor >> 8U) & 0xFFU);
    // Header layout cross-checked against all 100 vanilla MUSIC_*.SMD files:
    //   [0x0d] = 0x01 constant  [0x10] = 0x00 constant  [0x12,0x13] = 0x02,0x01 constant
    //   [0x1a] = 0x04 constant (NOT initial_tempo)   [0x1b] = initial_tempo
    out[0x0D] = 0x01;
    out[0x12] = 0x02;
    out[0x13] = 0x01;
    out[0x14] = static_cast<uint8_t>(track_count);
    out[0x15] = 0;
    out[0x16] = static_cast<uint8_t>(smd.assoc_wds_id & 0xFF);
    out[0x17] = static_cast<uint8_t>((smd.assoc_wds_id >> 8) & 0xFF);
    out[0x18] = static_cast<uint8_t>(std::clamp(smd.initial_volume, 0, 255));
    out[0x1A] = 0x04;
    out[0x1B] = static_cast<uint8_t>(std::clamp(smd.initial_tempo, 0, 255));
    out[0x1E] = static_cast<uint8_t>(title_offset & 0xFFU);
    out[0x1F] = static_cast<uint8_t>((title_offset >> 8U) & 0xFFU);
    const uint16_t drumkit_offset = track_offsets.empty() ? title_offset : track_offsets.front();
    out[0x20] = static_cast<uint8_t>(drumkit_offset & 0xFFU);
    out[0x21] = static_cast<uint8_t>((drumkit_offset >> 8U) & 0xFFU);

    for (const uint16_t offset : track_offsets) {
        write_u16_le(out, offset);
    }
    // Pre-title padding: when the source had unused offset-table slots
    // (e.g. an unused drumkit slot in MUSIC_31), fill them with zeros.
    out.insert(out.end(), pre_title_pad, static_cast<uint8_t>(0));
    out.insert(out.end(), title_bytes.begin(), title_bytes.end());
    for (const auto& encoded_track : encoded_tracks) {
        out.insert(out.end(), encoded_track.begin(), encoded_track.end());
    }

    return out;
}

bool save_smd_file(const std::string& path, const FFTSmdFile& smd, std::string* error_message) {
    const std::vector<uint8_t> bytes = serialize_smd_file(smd, error_message);
    if (bytes.empty()) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Cannot open SMD for write: " + path;
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        if (error_message != nullptr) {
            *error_message = "Failed to write SMD: " + path;
        }
        return false;
    }
    return true;
}

int32_t smd_opcode_param_count(uint8_t opcode) {
    const auto info_it = kOpcodeInfo.find(opcode);
    if (info_it != kOpcodeInfo.end()) {
        return info_it->second.second;
    }

    const auto extra_it = kExtraOpcodes.find(opcode);
    if (extra_it != kExtraOpcodes.end()) {
        return extra_it->second;
    }

    return 0;
}

const char* smd_opcode_name(uint8_t opcode) {
    const auto info_it = kOpcodeInfo.find(opcode);
    if (info_it != kOpcodeInfo.end()) {
        return info_it->second.first;
    }
    return "UnknownOpcode";
}

double fft_tempo_to_bpm(int32_t tempo_value) {
    if (tempo_value == 0) {
        return 120.0;
    }
    return (static_cast<double>(tempo_value) * 256.0) / 218.0;
}

}  // namespace fftplugin
