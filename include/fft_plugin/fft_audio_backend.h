#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fftplugin {

class IFFTWavesetService;

struct FFTPreviewNoteRequest {
    int16_t midi_note = 60;
    int16_t velocity = 100;
    int32_t instrument_id = 0;
    int32_t octave_shift = 0;
    std::optional<int32_t> left_volume_override;
    std::optional<int32_t> right_volume_override;
    std::optional<int32_t> adsr1_override;
    std::optional<int32_t> adsr2_override;
};

class IFFTInstrumentBank {
public:
    virtual ~IFFTInstrumentBank() = default;

    virtual bool has_instrument(int32_t instrument_id) const = 0;
    virtual std::string describe_instrument(int32_t instrument_id) const = 0;
};

class IFFTPreviewBackend {
public:
    virtual ~IFFTPreviewBackend() = default;

    virtual void set_instrument_bank(const IFFTInstrumentBank* instrument_bank) = 0;
    virtual void set_waveset_service(const IFFTWavesetService* waveset_service) = 0;
    virtual void note_on(const FFTPreviewNoteRequest& note_request) = 0;
    virtual void note_off(int16_t midi_note) = 0;
    virtual void all_notes_off() = 0;
};

}  // namespace fftplugin
