#pragma once

#include <vector>

#include "fft_plugin/fft_waveset_service.h"

namespace fftplugin {

class FFTWavesetFileService final : public IFFTWavesetService {
public:
    FFTWavesetLoadResult load_from_path(const std::string& waveset_path) override;
    bool is_loaded() const override;
    std::string loaded_path() const override;
    std::string bank_name() const override;
    int32_t instrument_count() const override;
    std::optional<FFTInstrumentInfo> instrument_info(int32_t instrument_id) const override;
    bool has_instrument(int32_t instrument_id) const override;
    std::string describe_instrument(int32_t instrument_id) const override;
    const std::vector<FFTSpuInstrumentData>& spu_instruments() const override;
    const std::vector<uint8_t>& adpcm_bank() const override;

private:
    void clear();

    bool loaded_ = false;
    std::string loaded_path_;
    std::string bank_name_ = "FFT WAVESET";
    std::vector<FFTInstrumentInfo> instruments_;
    std::vector<FFTSpuInstrumentData> spu_instruments_;
    std::vector<uint8_t> adpcm_bank_;
};

}  // namespace fftplugin
