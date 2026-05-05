#include "fft_plugin/fft_smd_authoring_codec.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace fftplugin {

namespace {

constexpr char kAuthoringMagic[] = "FFTAUTH";
constexpr size_t kAuthoringMagicSize = sizeof(kAuthoringMagic) - 1;

void write_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
}

void write_i32(std::vector<uint8_t>& out, int32_t value) {
    write_u32(out, static_cast<uint32_t>(value));
}

bool read_u32(const std::vector<uint8_t>& bytes, size_t* offset, uint32_t* value) {
    if (*offset + 4 > bytes.size()) {
        return false;
    }
    *value = static_cast<uint32_t>(bytes[*offset]) |
        (static_cast<uint32_t>(bytes[*offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[*offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[*offset + 3]) << 24);
    *offset += 4;
    return true;
}

bool read_i32(const std::vector<uint8_t>& bytes, size_t* offset, int32_t* value) {
    uint32_t raw = 0;
    if (!read_u32(bytes, offset, &raw)) {
        return false;
    }
    *value = static_cast<int32_t>(raw);
    return true;
}

void write_string(std::vector<uint8_t>& out, const std::string& value) {
    write_u32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

bool read_string(const std::vector<uint8_t>& bytes, size_t* offset, std::string* value) {
    uint32_t size = 0;
    if (!read_u32(bytes, offset, &size)) {
        return false;
    }
    if (*offset + size > bytes.size()) {
        return false;
    }
    value->assign(
        reinterpret_cast<const char*>(bytes.data() + static_cast<std::ptrdiff_t>(*offset)),
        static_cast<size_t>(size));
    *offset += size;
    return true;
}

void write_opcode(std::vector<uint8_t>& out, const FFTSmdAuthoredOpcode& opcode) {
    write_i32(out, opcode.tick);
    write_i32(out, opcode.stack_order);
    write_u32(out, opcode.enabled ? 1U : 0U);
    write_u32(out, opcode.exact_timing ? 1U : 0U);
    write_i32(out, opcode.opcode.opcode);
    write_u32(out, static_cast<uint32_t>(opcode.opcode.params.size()));
    for (const int32_t param : opcode.opcode.params) {
        write_i32(out, param);
    }
}

bool read_opcode(
    const std::vector<uint8_t>& bytes,
    size_t* offset,
    bool has_exact_timing,
    FFTSmdAuthoredOpcode* opcode
) {
    if (opcode == nullptr) {
        return false;
    }
    uint32_t enabled = 0;
    uint32_t exact_timing = 0;
    if (!read_i32(bytes, offset, &opcode->tick) ||
        !read_i32(bytes, offset, &opcode->stack_order) ||
        !read_u32(bytes, offset, &enabled) ||
        (has_exact_timing && !read_u32(bytes, offset, &exact_timing)) ||
        !read_i32(bytes, offset, &opcode->opcode.opcode)) {
        return false;
    }
    opcode->enabled = enabled != 0U;
    opcode->exact_timing = has_exact_timing && exact_timing != 0U;

    uint32_t param_count = 0;
    if (!read_u32(bytes, offset, &param_count)) {
        return false;
    }
    opcode->opcode.params.clear();
    opcode->opcode.params.reserve(static_cast<size_t>(param_count));
    for (uint32_t param_index = 0; param_index < param_count; ++param_index) {
        int32_t param = 0;
        if (!read_i32(bytes, offset, &param)) {
            return false;
        }
        opcode->opcode.params.push_back(param);
    }
    return true;
}

void write_raw_track(std::vector<uint8_t>& out, const FFTSmdAuthoredTrack& track) {
    write_i32(out, track.total_ticks);
    write_i32(out, track.track_transposition);
    write_u32(out, static_cast<uint32_t>(track.spans.size()));
    for (const auto& span : track.spans) {
        write_i32(out, span.start_tick);
        write_i32(out, span.total_ticks);
        write_i32(out, span.base_ticks);
        write_i32(out, span.velocity_hint);
        write_i32(out, span.relative_key);
    }
    write_u32(out, static_cast<uint32_t>(track.opcodes.size()));
    for (const auto& opcode : track.opcodes) {
        write_opcode(out, opcode);
    }
}

bool read_raw_track(
    const std::vector<uint8_t>& bytes,
    size_t* offset,
    bool has_exact_timing,
    bool has_track_transposition,
    FFTSmdAuthoredTrack* track
) {
    if (track == nullptr || !read_i32(bytes, offset, &track->total_ticks)) {
        return false;
    }
    if (has_track_transposition && !read_i32(bytes, offset, &track->track_transposition)) {
        return false;
    }

    uint32_t span_count = 0;
    if (!read_u32(bytes, offset, &span_count)) {
        return false;
    }
    track->spans.clear();
    track->spans.reserve(static_cast<size_t>(span_count));
    for (uint32_t span_index = 0; span_index < span_count; ++span_index) {
        FFTSmdAuthoredSpan span;
        if (!read_i32(bytes, offset, &span.start_tick) ||
            !read_i32(bytes, offset, &span.total_ticks) ||
            !read_i32(bytes, offset, &span.base_ticks) ||
            !read_i32(bytes, offset, &span.velocity_hint) ||
            !read_i32(bytes, offset, &span.relative_key)) {
            return false;
        }
        track->spans.push_back(span);
    }

    uint32_t opcode_count = 0;
    if (!read_u32(bytes, offset, &opcode_count)) {
        return false;
    }
    track->opcodes.clear();
    track->opcodes.reserve(static_cast<size_t>(opcode_count));
    for (uint32_t opcode_index = 0; opcode_index < opcode_count; ++opcode_index) {
        FFTSmdAuthoredOpcode opcode;
        if (!read_opcode(bytes, offset, has_exact_timing, &opcode)) {
            return false;
        }
        track->opcodes.push_back(std::move(opcode));
    }
    return true;
}

void write_poly_track(std::vector<uint8_t>& out, const FFTSmdAuthoredPolyTrack& track) {
    write_i32(out, track.total_ticks);
    write_i32(out, track.track_transposition);
    write_u32(out, static_cast<uint32_t>(track.notes.size()));
    for (const auto& note : track.notes) {
        write_i32(out, note.start_tick);
        write_i32(out, note.total_ticks);
        write_i32(out, note.base_ticks);
        write_i32(out, note.velocity_hint);
        write_i32(out, note.relative_key);
    }
    write_u32(out, static_cast<uint32_t>(track.opcodes.size()));
    for (const auto& opcode : track.opcodes) {
        write_opcode(out, opcode);
    }
}

bool read_poly_track(
    const std::vector<uint8_t>& bytes,
    size_t* offset,
    bool has_track_transposition,
    FFTSmdAuthoredPolyTrack* track
) {
    if (track == nullptr || !read_i32(bytes, offset, &track->total_ticks)) {
        return false;
    }
    if (has_track_transposition && !read_i32(bytes, offset, &track->track_transposition)) {
        return false;
    }

    uint32_t note_count = 0;
    if (!read_u32(bytes, offset, &note_count)) {
        return false;
    }
    track->notes.clear();
    track->notes.reserve(static_cast<size_t>(note_count));
    for (uint32_t note_index = 0; note_index < note_count; ++note_index) {
        FFTSmdAuthoredPolyNote note;
        if (!read_i32(bytes, offset, &note.start_tick) ||
            !read_i32(bytes, offset, &note.total_ticks) ||
            !read_i32(bytes, offset, &note.base_ticks) ||
            !read_i32(bytes, offset, &note.velocity_hint) ||
            !read_i32(bytes, offset, &note.relative_key)) {
            return false;
        }
        track->notes.push_back(note);
    }

    uint32_t opcode_count = 0;
    if (!read_u32(bytes, offset, &opcode_count)) {
        return false;
    }
    track->opcodes.clear();
    track->opcodes.reserve(static_cast<size_t>(opcode_count));
    for (uint32_t opcode_index = 0; opcode_index < opcode_count; ++opcode_index) {
        FFTSmdAuthoredOpcode opcode;
        if (!read_opcode(bytes, offset, true, &opcode)) {
            return false;
        }
        track->opcodes.push_back(std::move(opcode));
    }
    return true;
}

std::vector<uint8_t> serialize_authored_document_impl(const FFTSmdAuthoringDocument& document) {
    std::vector<uint8_t> out;
    out.insert(out.end(), kAuthoringMagic, kAuthoringMagic + kAuthoringMagicSize);
    write_i32(out, document.format_version);
    write_i32(out, document.track_count);
    write_i32(out, document.initial_tempo);
    write_i32(out, document.initial_volume);
    write_i32(out, document.assoc_wds_id);
    write_string(out, document.song_title);
    write_u32(out, static_cast<uint32_t>(document.parts.size()));
    for (const auto& part : document.parts) {
        write_i32(out, static_cast<int32_t>(part.kind));
        write_string(out, part.name);
        if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
            write_poly_track(out, part.poly_track);
        } else {
            write_raw_track(out, part.raw_track);
        }
    }
    return out;
}

std::optional<FFTSmdAuthoringDocument> deserialize_authored_document_impl(
    const std::vector<uint8_t>& bytes,
    std::string* error_message
) {
    if (bytes.size() < kAuthoringMagicSize + 4) {
        if (error_message != nullptr) {
            *error_message = "Authoring document too small";
        }
        return std::nullopt;
    }
    if (std::memcmp(bytes.data(), kAuthoringMagic, kAuthoringMagicSize) != 0) {
        if (error_message != nullptr) {
            *error_message = "Invalid authoring document magic";
        }
        return std::nullopt;
    }

    size_t offset = kAuthoringMagicSize;
    FFTSmdAuthoringDocument document;
    if (!read_i32(bytes, &offset, &document.format_version) ||
        !read_i32(bytes, &offset, &document.track_count) ||
        !read_i32(bytes, &offset, &document.initial_tempo) ||
        !read_i32(bytes, &offset, &document.initial_volume) ||
        !read_i32(bytes, &offset, &document.assoc_wds_id) ||
        !read_string(bytes, &offset, &document.song_title)) {
        if (error_message != nullptr) {
            *error_message = "Malformed authoring document header";
        }
        return std::nullopt;
    }

    uint32_t part_count = 0;
    if (!read_u32(bytes, &offset, &part_count)) {
        if (error_message != nullptr) {
            *error_message = "Malformed authoring part count";
        }
        return std::nullopt;
    }
    document.parts.reserve(static_cast<size_t>(part_count));

    const bool legacy_raw_only = document.format_version < 3;
    const bool has_track_transposition = document.format_version >= 4;
    for (uint32_t part_index = 0; part_index < part_count; ++part_index) {
        FFTSmdAuthoringPart part;
        if (legacy_raw_only) {
            part.kind = FFTSmdAuthoringPartKind::raw_track;
            if (!read_raw_track(bytes, &offset, document.format_version >= 2, false, &part.raw_track)) {
                if (error_message != nullptr) {
                    *error_message = "Malformed authored raw track payload";
                }
                return std::nullopt;
            }
            part.name = "Track " + std::to_string(part_index);
        } else {
            int32_t raw_kind = 0;
            if (!read_i32(bytes, &offset, &raw_kind) || !read_string(bytes, &offset, &part.name)) {
                if (error_message != nullptr) {
                    *error_message = "Malformed authored part header";
                }
                return std::nullopt;
            }
            part.kind = static_cast<FFTSmdAuthoringPartKind>(raw_kind);
            if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
                if (!read_poly_track(bytes, &offset, has_track_transposition, &part.poly_track)) {
                    if (error_message != nullptr) {
                        *error_message = "Malformed authored poly track payload";
                    }
                    return std::nullopt;
                }
            } else {
                part.kind = FFTSmdAuthoringPartKind::raw_track;
                if (!read_raw_track(bytes, &offset, true, has_track_transposition, &part.raw_track)) {
                    if (error_message != nullptr) {
                        *error_message = "Malformed authored raw track payload";
                    }
                    return std::nullopt;
                }
            }
        }
        document.parts.push_back(std::move(part));
    }

    if (document.format_version < 4) {
        document.format_version = 4;
    }
    document.tracks.clear();
    for (const auto& part : document.parts) {
        if (part.kind == FFTSmdAuthoringPartKind::raw_track) {
            document.tracks.push_back(part.raw_track);
        }
    }
    document.track_count = compile_smd_authoring_document(document).smd.track_count;
    return document;
}

std::string lowercase_copy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

}  // namespace

bool save_smd_authoring_document(
    const std::string& path,
    const FFTSmdAuthoringDocument& document,
    std::string* error_message
) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open authoring document for write";
        }
        return false;
    }

    const std::vector<uint8_t> bytes = serialize_smd_authoring_document(document);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        if (error_message != nullptr) {
            *error_message = "Failed to write authoring document";
        }
        return false;
    }
    return true;
}

std::optional<FFTSmdAuthoringDocument> load_smd_authoring_document(
    const std::string& path,
    std::string* error_message
) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open authoring document";
        }
        return std::nullopt;
    }

    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    return deserialize_smd_authoring_document(bytes, error_message);
}

bool is_smd_authoring_path(const std::string& path) {
    const std::string lowered = lowercase_copy(path);
    const std::string extension = lowercase_copy(kFftAuthoringExtension);
    return lowered.size() >= extension.size() &&
        lowered.compare(lowered.size() - extension.size(), extension.size(), extension) == 0;
}

std::string default_authoring_path_for_smd(const std::string& smd_path) {
    const size_t last_slash = smd_path.find_last_of("/\\");
    const size_t last_dot = smd_path.find_last_of('.');
    if (last_dot == std::string::npos || (last_slash != std::string::npos && last_dot < last_slash)) {
        return smd_path + kFftAuthoringExtension;
    }
    return smd_path.substr(0, last_dot) + kFftAuthoringExtension;
}

std::vector<uint8_t> serialize_smd_authoring_document(const FFTSmdAuthoringDocument& document) {
    return serialize_authored_document_impl(document);
}

std::optional<FFTSmdAuthoringDocument> deserialize_smd_authoring_document(
    const std::vector<uint8_t>& bytes,
    std::string* error_message
) {
    return deserialize_authored_document_impl(bytes, error_message);
}

}  // namespace fftplugin
