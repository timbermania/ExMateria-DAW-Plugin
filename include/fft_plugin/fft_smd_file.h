#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fftplugin {

struct FFTSmdNoteEvent {
    int32_t velocity = 0;
    int32_t relative_key = 0;  // 0-11 note, 12 tie, 13 rest
    int32_t delta_time = 0;

    // The wire format has two encodings for any delta_time that happens to
    // appear in kFftSmdDeltaTimeTable:
    //   - one-byte form: `key*19 + delta_index` (delta_index in 1..18)
    //   - literal-byte form: `key*19` followed by an explicit delta byte
    // Real-disc SMDs occasionally use the literal form for table-aligned
    // deltas. The parser sets this flag so the serializer can reproduce
    // the exact source bytes; freshly-built notes default to false (the
    // encoder picks the most compact form).
    bool used_literal_encoding = false;
};

struct FFTSmdOpcodeEvent {
    int32_t opcode = 0;
    std::vector<int32_t> params;
};

using FFTSmdTrackEvent = std::variant<FFTSmdNoteEvent, FFTSmdOpcodeEvent>;

struct FFTSmdFile {
    int32_t track_count = 0;
    int32_t initial_tempo = 0;
    int32_t initial_volume = 0;
    int32_t assoc_wds_id = 0;
    std::string song_title;
    std::vector<std::vector<FFTSmdTrackEvent>> track_events;

    // Bytes that follow the END_BAR (0x90) terminator within each track's
    // byte range, captured at parse time so the serializer can reproduce
    // them. Real-disc SMDs (e.g. MUSIC_31) have alignment / leftover-data
    // bytes here that are otherwise dropped on round-trip.
    // Parallel to track_events: track_trailing_bytes[i] applies to track i.
    // Default-constructed empty vector means "no trailing bytes" (current
    // behavior for freshly-compiled SMDs).
    std::vector<std::vector<uint8_t>> track_trailing_bytes;

    // Raw 0x22-byte file header captured at parse time. The serializer
    // starts from this when present and overwrites only the offsets it
    // explicitly knows (magic, file_size, constants, track_count,
    // assoc_wds_id, initial_volume, initial_tempo, title_offset,
    // drumkit_offset). All other bytes — many of which carry song-level
    // data we don't yet decode (offsets 0x04-0x07, 0x0E-0x0F, etc.) —
    // are preserved. Empty vector = use the synthesized header (default
    // for freshly-compiled SMDs).
    std::vector<uint8_t> raw_header;

    // Source title_offset captured at parse time. Real-disc SMDs sometimes
    // pad the offset table out (e.g. MUSIC_31 has 2 zero bytes between the
    // 17-entry offset table at 0x44 and the title at 0x46), so the title
    // doesn't always sit immediately after the offset table. -1 = let the
    // serializer compute the default packed layout.
    int32_t title_offset_override = -1;

    // Bytes from title_offset up to the first track's offset, captured at
    // parse time. Includes the song title, its null terminator, and any
    // post-title alignment padding the source kept. When set, the
    // serializer emits these bytes verbatim instead of synthesizing them
    // from song_title; otherwise it builds them from song_title + a
    // possible even-alignment pad. Empty = synthesize.
    std::vector<uint8_t> title_region_bytes;
};

constexpr int32_t kFftSmdPpq = 48;
extern const std::array<int32_t, 19> kFftSmdDeltaTimeTable;

std::optional<FFTSmdFile> load_smd_file(const std::string& path, std::string* error_message = nullptr);
std::optional<FFTSmdFile> parse_smd_bytes(const std::vector<uint8_t>& data, std::string* error_message = nullptr);
std::vector<uint8_t> serialize_smd_file(const FFTSmdFile& smd, std::string* error_message = nullptr);
bool save_smd_file(const std::string& path, const FFTSmdFile& smd, std::string* error_message = nullptr);
int32_t smd_opcode_param_count(uint8_t opcode);
const char* smd_opcode_name(uint8_t opcode);
double fft_tempo_to_bpm(int32_t tempo_value);

}  // namespace fftplugin
