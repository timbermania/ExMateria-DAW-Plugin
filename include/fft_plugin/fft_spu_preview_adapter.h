#pragma once

#include <string>
#include <vector>

#include "fft_plugin/fft_pitch_tools.h"
#include "fft_plugin/fft_spu_core.h"
#include "fft_plugin/fft_spu_preview_backend.h"
#include "fft_plugin/fft_waveset_service.h"

namespace fftplugin {

class FFTSpuPreviewAdapter final : public IFFTSpuPreviewBackend {
public:
    explicit FFTSpuPreviewAdapter(IFFTSpuCore* spu_core);

    void set_instrument_bank(const IFFTInstrumentBank* instrument_bank) override;
    void set_waveset_service(const IFFTWavesetService* waveset_service) override;
    void note_on(const FFTPreviewNoteRequest& note_request) override;
    void note_off(int16_t midi_note) override;
    void all_notes_off() override;

    void reset_preview_state() override;
    void configure_preview(const FFTPreviewBackendConfig& config) override;
    int32_t active_voice_count() const override;

private:
    bool sync_waveset_into_spu();
    int32_t allocate_voice(int16_t midi_note);
    int32_t velocity_to_volume(int16_t velocity) const;

    IFFTSpuCore* spu_core_ = nullptr;
    const IFFTInstrumentBank* instrument_bank_ = nullptr;
    const IFFTWavesetService* waveset_service_ = nullptr;
    FFTPreviewBackendConfig config_;
    std::vector<int16_t> active_voice_notes_;
    std::string loaded_waveset_path_;
    int32_t next_voice_index_ = 0;
};

}  // namespace fftplugin
