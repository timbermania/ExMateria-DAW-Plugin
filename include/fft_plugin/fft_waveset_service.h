#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fft_plugin/fft_audio_backend.h"
#include "fft_plugin/fft_spu_core.h"

namespace fftplugin {

struct FFTInstrumentInfo {
    int32_t instrument_id = 0;
    std::string name;
    bool is_null = true;
    int32_t fine_tune = 0;
    int32_t adsr1 = 0;
    int32_t adsr2 = 0;
    int32_t sample_offset = 0;
    int32_t sample_size = 0;
    int32_t loop_start = -1;
    int32_t loop_offset_bytes = -1;
    bool has_explicit_loop_start = false;
    bool has_loop_repeat = false;
    int32_t start_offset_bytes = 0;
    int32_t start_sample_skip = 0;
};

struct FFTWavesetLoadResult {
    bool ok = false;
    std::string message;
    int32_t instrument_count = 0;
};

class IFFTWavesetService : public IFFTInstrumentBank {
public:
    ~IFFTWavesetService() override = default;

    virtual FFTWavesetLoadResult load_from_path(const std::string& waveset_path) = 0;
    virtual bool is_loaded() const = 0;
    virtual std::string loaded_path() const = 0;
    virtual std::string bank_name() const = 0;
    virtual int32_t instrument_count() const = 0;
    virtual std::optional<FFTInstrumentInfo> instrument_info(int32_t instrument_id) const = 0;
    virtual const std::vector<FFTSpuInstrumentData>& spu_instruments() const = 0;
    virtual const std::vector<uint8_t>& adpcm_bank() const = 0;
};

}  // namespace fftplugin
