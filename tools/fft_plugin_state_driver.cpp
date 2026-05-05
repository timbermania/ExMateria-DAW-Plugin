#include <algorithm>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "fft_plugin/fft_audio_backend.h"
#include "fft_plugin/fft_plugin_controller.h"
#include "fft_plugin/fft_spu_core.h"
#include "fft_plugin/fft_spu_preview_adapter.h"
#include "fft_plugin/fft_spu_preview_core.h"
#include "fft_plugin/fft_waveset_file_service.h"

namespace {

class DummyInstrumentBank final : public fftplugin::IFFTInstrumentBank {
public:
    bool has_instrument(int32_t instrument_id) const override {
        return instrument_id >= 0;
    }

    std::string describe_instrument(int32_t instrument_id) const override {
        return "Instrument " + std::to_string(instrument_id);
    }
};

class LoggingPreviewBackend final : public fftplugin::IFFTPreviewBackend {
public:
    void set_instrument_bank(const fftplugin::IFFTInstrumentBank* instrument_bank) override {
        instrument_bank_ = instrument_bank;
    }

    void set_waveset_service(const fftplugin::IFFTWavesetService* waveset_service) override {
        waveset_service_ = waveset_service;
    }

    void note_on(const fftplugin::FFTPreviewNoteRequest& note_request) override {
        std::cout << "note_on="
                  << note_request.midi_note
                  << " vel=" << note_request.velocity
                  << " inst=" << note_request.instrument_id;
        if (instrument_bank_ != nullptr) {
            std::cout << " label=" << instrument_bank_->describe_instrument(note_request.instrument_id);
        }
        std::cout << "\n";
    }

    void note_off(int16_t midi_note) override {
        std::cout << "note_off=" << midi_note << "\n";
    }

    void all_notes_off() override {
        std::cout << "all_notes_off\n";
    }

private:
    const fftplugin::IFFTInstrumentBank* instrument_bank_ = nullptr;
    const fftplugin::IFFTWavesetService* waveset_service_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    fftplugin::FFTPluginController controller;
    DummyInstrumentBank instrument_bank;
    fftplugin::FFTWavesetFileService waveset_service;
    fftplugin::FFTSpuPreviewCore spu_core;
    fftplugin::FFTSpuPreviewAdapter preview_backend(&spu_core);
    const std::string waveset_path = argc > 1 ? argv[1] : fftplugin::asset_paths::default_waveset();

    controller.set_instrument_bank(&instrument_bank);
    controller.set_waveset_service(&waveset_service);
    controller.set_preview_backend(&preview_backend);

    const auto load_result = controller.load_waveset_path(waveset_path);
    controller.document().append_instrument_change(1, 0, 42);
    controller.document().append_note(1, 0, 48, 60, 100);

    const auto preview = controller.begin_live_preview(64, 112);
    const auto& state = controller.document().state();
    const auto pcm = spu_core.render_interleaved_pcm16(512);
    const int64_t abs_sum = std::accumulate(
        pcm.begin(),
        pcm.end(),
        int64_t {0},
        [](int64_t acc, int16_t sample) { return acc + std::abs(static_cast<int>(sample)); }
    );
    const int16_t peak = pcm.empty()
        ? 0
        : *std::max_element(
            pcm.begin(),
            pcm.end(),
            [](int16_t lhs, int16_t rhs) {
                return std::abs(static_cast<int>(lhs)) < std::abs(static_cast<int>(rhs));
            }
        );

    std::cout << "waveset_loaded=" << (load_result.ok ? "yes" : "no") << "\n";
    std::cout << "load_message=" << load_result.message << "\n";
    std::cout << "bank=" << state.selected_bank_name << "\n";
    std::cout << "sequence=" << state.sequence.name << "\n";
    std::cout << "waveset=" << state.waveset_path << "\n";
    std::cout << "instrument_count=" << waveset_service.instrument_count() << "\n";
    std::cout << "adpcm_bytes=" << waveset_service.adpcm_bank().size() << "\n";
    std::cout << "tracks=" << state.sequence.tracks.size() << "\n";
    std::cout << "track0_events=" << state.sequence.tracks.front().events.size() << "\n";
    std::cout << "preview_active=" << (preview.has_value() ? "yes" : "no") << "\n";
    std::cout << "spu_active_voices=" << preview_backend.active_voice_count() << "\n";
    std::cout << "pcm_frames=" << (pcm.size() / 2) << "\n";
    std::cout << "pcm_abs_sum=" << abs_sum << "\n";
    std::cout << "pcm_peak=" << peak << "\n";
    return 0;
}
