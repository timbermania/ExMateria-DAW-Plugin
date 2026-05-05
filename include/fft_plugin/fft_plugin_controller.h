#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "fft_plugin/fft_audio_backend.h"
#include "fft_plugin/fft_document.h"
#include "fft_plugin/fft_waveset_service.h"

namespace fftplugin {

struct HostTransportState {
    double bpm = 120.0;
    double ppq_position = 0.0;
    bool playing = false;
    bool looping = false;
};

struct LivePreviewNote {
    int16_t midi_note = 60;
    int16_t velocity = 100;
    int32_t instrument_id = 0;
};

class FFTPluginController {
public:
    FFTPluginController();

    FFTDocument& document();
    const FFTDocument& document() const;

    void set_transport_state(const HostTransportState& transport_state);
    const HostTransportState& transport_state() const;

    void set_preview_backend(IFFTPreviewBackend* preview_backend);
    void set_waveset_service(IFFTWavesetService* waveset_service);
    void set_instrument_bank(const IFFTInstrumentBank* instrument_bank);
    void set_waveset_path(std::string waveset_path);
    FFTWavesetLoadResult load_waveset_path(std::string waveset_path);
    std::optional<LivePreviewNote> begin_live_preview(int16_t midi_note, int16_t velocity);
    std::optional<LivePreviewNote> begin_live_preview(const FFTPreviewNoteRequest& request);
    void end_live_preview(int16_t midi_note);

private:
    FFTDocument document_;
    HostTransportState transport_state_;
    std::optional<LivePreviewNote> active_preview_note_;
    IFFTPreviewBackend* preview_backend_ = nullptr;
    const IFFTInstrumentBank* instrument_bank_ = nullptr;
    IFFTWavesetService* waveset_service_ = nullptr;
};

}  // namespace fftplugin
