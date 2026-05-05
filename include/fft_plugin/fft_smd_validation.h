#pragma once

#include <vector>

#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_inspector.h"

namespace fftplugin {

std::vector<std::vector<FFTSmdLaneDiagnostic>> validate_smd_authoring_document(
    const FFTSmdAuthoringDocument& document);

}  // namespace fftplugin
