#include "fft_plugin/fft_plugin_state_codec.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "fft_plugin/fft_smd_authoring_codec.h"

namespace fftplugin {

namespace {

constexpr char kStateMagic[] = "FFTPS1";
constexpr size_t kStateMagicSize = sizeof(kStateMagic) - 1;

void write_u32(std::vector<uint8_t>& out, uint32_t value);
bool read_u32(const std::vector<uint8_t>& bytes, size_t* offset, uint32_t* value);

void write_i32(std::vector<uint8_t>& out, int32_t value) {
    write_u32(out, static_cast<uint32_t>(value));
}

bool read_i32(const std::vector<uint8_t>& bytes, size_t* offset, int32_t* value) {
    uint32_t raw = 0;
    if (!read_u32(bytes, offset, &raw)) {
        return false;
    }
    *value = static_cast<int32_t>(raw);
    return true;
}

void write_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
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
        static_cast<size_t>(size)
    );
    *offset += size;
    return true;
}

}  // namespace

std::vector<uint8_t> serialize_plugin_state_minimal(const FFTPluginState& state) {
    std::vector<uint8_t> out;
    out.reserve(kStateMagicSize + 32 + state.waveset_path.size() + state.smd_path.size() + state.selected_bank_name.size());
    out.insert(out.end(), kStateMagic, kStateMagic + kStateMagicSize);
    write_u32(out, static_cast<uint32_t>(state.state_version));
    write_string(out, state.waveset_path);
    write_string(out, state.smd_path);
    write_string(out, state.authoring_path);
    write_string(out, state.selected_bank_name);
    write_u32(out, static_cast<uint32_t>(state.editor_view.selected_track_id));
    write_u32(out, static_cast<uint32_t>(state.editor_view.muted_track_ids.size()));
    for (const int32_t track_id : state.editor_view.muted_track_ids) {
        write_u32(out, static_cast<uint32_t>(track_id));
    }
    write_u32(out, static_cast<uint32_t>(state.editor_view.solo_track_ids.size()));
    for (const int32_t track_id : state.editor_view.solo_track_ids) {
        write_u32(out, static_cast<uint32_t>(track_id));
    }
    write_u32(out, state.smd_authoring.has_value() ? 1U : 0U);
    if (state.smd_authoring.has_value()) {
        const auto bytes = serialize_smd_authoring_document(*state.smd_authoring);
        write_u32(out, static_cast<uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

bool deserialize_plugin_state_minimal(
    const std::vector<uint8_t>& bytes,
    FFTPluginState* state,
    std::string* error_message
) {
    if (state == nullptr) {
        if (error_message != nullptr) {
            *error_message = "No output state provided";
        }
        return false;
    }
    if (bytes.size() < kStateMagicSize + 4) {
        if (error_message != nullptr) {
            *error_message = "Plugin state blob too small";
        }
        return false;
    }
    if (std::memcmp(bytes.data(), kStateMagic, kStateMagicSize) != 0) {
        if (error_message != nullptr) {
            *error_message = "Invalid plugin state magic";
        }
        return false;
    }

    size_t offset = kStateMagicSize;
    uint32_t state_version = 0;
    if (!read_u32(bytes, &offset, &state_version)) {
        if (error_message != nullptr) {
            *error_message = "Missing state version";
        }
        return false;
    }

    FFTPluginState decoded = *state;
    decoded.state_version = static_cast<int32_t>(state_version);
    if (!read_string(bytes, &offset, &decoded.waveset_path) ||
        !read_string(bytes, &offset, &decoded.smd_path)) {
        if (error_message != nullptr) {
            *error_message = "Malformed plugin state payload";
        }
        return false;
    }
    if (decoded.state_version >= 3) {
        if (!read_string(bytes, &offset, &decoded.authoring_path)) {
            if (error_message != nullptr) {
                *error_message = "Malformed authoring path payload";
            }
            return false;
        }
    } else {
        decoded.authoring_path.clear();
    }
    if (!read_string(bytes, &offset, &decoded.selected_bank_name)) {
        if (error_message != nullptr) {
            *error_message = "Malformed bank-name payload";
        }
        return false;
    }

    if (offset < bytes.size()) {
        uint32_t selected_track_id = 0;
        if (!read_u32(bytes, &offset, &selected_track_id)) {
            if (error_message != nullptr) {
                *error_message = "Malformed selected track payload";
            }
            return false;
        }
        decoded.editor_view.selected_track_id = static_cast<int32_t>(selected_track_id);
    }

    if (offset < bytes.size()) {
        uint32_t muted_count = 0;
        if (!read_u32(bytes, &offset, &muted_count)) {
            if (error_message != nullptr) {
                *error_message = "Malformed muted track payload";
            }
            return false;
        }
        decoded.editor_view.muted_track_ids.clear();
        decoded.editor_view.muted_track_ids.reserve(static_cast<size_t>(muted_count));
        for (uint32_t i = 0; i < muted_count; ++i) {
            uint32_t track_id = 0;
            if (!read_u32(bytes, &offset, &track_id)) {
                if (error_message != nullptr) {
                    *error_message = "Malformed muted track list";
                }
                return false;
            }
            decoded.editor_view.muted_track_ids.push_back(static_cast<int32_t>(track_id));
        }
    }

    if (offset < bytes.size()) {
        uint32_t solo_count = 0;
        if (!read_u32(bytes, &offset, &solo_count)) {
            if (error_message != nullptr) {
                *error_message = "Malformed solo track payload";
            }
            return false;
        }
        decoded.editor_view.solo_track_ids.clear();
        decoded.editor_view.solo_track_ids.reserve(static_cast<size_t>(solo_count));
        for (uint32_t i = 0; i < solo_count; ++i) {
            uint32_t track_id = 0;
            if (!read_u32(bytes, &offset, &track_id)) {
                if (error_message != nullptr) {
                    *error_message = "Malformed solo track list";
                }
                return false;
            }
            decoded.editor_view.solo_track_ids.push_back(static_cast<int32_t>(track_id));
        }
    }

    if (offset < bytes.size()) {
        uint32_t has_authoring = 0;
        if (!read_u32(bytes, &offset, &has_authoring)) {
            if (error_message != nullptr) {
                *error_message = "Malformed authoring-document flag";
            }
            return false;
        }
        if (has_authoring != 0U) {
            uint32_t authoring_size = 0;
            if (!read_u32(bytes, &offset, &authoring_size) ||
                offset + authoring_size > bytes.size()) {
                if (error_message != nullptr) {
                    *error_message = "Malformed authoring-document payload";
                }
                return false;
            }
            std::vector<uint8_t> authored_bytes(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + authoring_size));
            offset += authoring_size;
            auto authored = deserialize_smd_authoring_document(authored_bytes, error_message);
            if (!authored.has_value()) {
                if (error_message != nullptr && error_message->empty()) {
                    *error_message = "Malformed authoring-document payload";
                }
                return false;
            }
            decoded.smd_authoring = std::move(*authored);
        } else {
            decoded.smd_authoring.reset();
        }
    }

    *state = std::move(decoded);
    return true;
}

}  // namespace fftplugin
