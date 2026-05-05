#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"

namespace fftplugin {

constexpr const char* kFftAuthoringExtension = ".fftauth";

std::vector<uint8_t> serialize_smd_authoring_document(const FFTSmdAuthoringDocument& document);
std::optional<FFTSmdAuthoringDocument> deserialize_smd_authoring_document(
    const std::vector<uint8_t>& bytes,
    std::string* error_message = nullptr);

bool save_smd_authoring_document(
    const std::string& path,
    const FFTSmdAuthoringDocument& document,
    std::string* error_message = nullptr);

std::optional<FFTSmdAuthoringDocument> load_smd_authoring_document(
    const std::string& path,
    std::string* error_message = nullptr);

bool is_smd_authoring_path(const std::string& path);
std::string default_authoring_path_for_smd(const std::string& smd_path);

}  // namespace fftplugin
