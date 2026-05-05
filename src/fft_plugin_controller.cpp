#include "fft_plugin/fft_plugin_controller.h"

#include <utility>

namespace fftplugin {

FFTPluginController::FFTPluginController() = default;

FFTDocument& FFTPluginController::document() {
    return document_;
}

const FFTDocument& FFTPluginController::document() const {
    return document_;
}

void FFTPluginController::set_transport_state(const HostTransportState& transport_state) {
    transport_state_ = transport_state;
}

const HostTransportState& FFTPluginController::transport_state() const {
    return transport_state_;
}

void FFTPluginController::set_preview_backend(IFFTPreviewBackend* preview_backend) {
    preview_backend_ = preview_backend;
    if (preview_backend_ != nullptr) {
        preview_backend_->set_instrument_bank(instrument_bank_);
        preview_backend_->set_waveset_service(waveset_service_);
    }
}

void FFTPluginController::set_waveset_service(IFFTWavesetService* waveset_service) {
    waveset_service_ = waveset_service;
    if (waveset_service_ != nullptr && waveset_service_->is_loaded()) {
        document_.state().selected_bank_name = waveset_service_->bank_name();
        set_instrument_bank(waveset_service_);
    }
}

void FFTPluginController::set_instrument_bank(const IFFTInstrumentBank* instrument_bank) {
    instrument_bank_ = instrument_bank;
    if (preview_backend_ != nullptr) {
        preview_backend_->set_instrument_bank(instrument_bank_);
    }
}

void FFTPluginController::set_waveset_path(std::string waveset_path) {
    document_.state().waveset_path = std::move(waveset_path);
}

FFTWavesetLoadResult FFTPluginController::load_waveset_path(std::string waveset_path) {
    set_waveset_path(std::move(waveset_path));

    if (waveset_service_ == nullptr) {
        return FFTWavesetLoadResult {
            .ok = false,
            .message = "No waveset service attached",
            .instrument_count = 0,
        };
    }

    FFTWavesetLoadResult result = waveset_service_->load_from_path(document_.state().waveset_path);
    if (result.ok) {
        document_.state().selected_bank_name = waveset_service_->bank_name();
        set_instrument_bank(waveset_service_);
    }
    return result;
}

std::optional<LivePreviewNote> FFTPluginController::begin_live_preview(int16_t midi_note, int16_t velocity) {
    const int32_t selected_track_id = document_.state().editor_view.selected_track_id;
    const FFTTrack* track = document_.find_track(selected_track_id);
    if (track == nullptr) {
        return std::nullopt;
    }

    return begin_live_preview(FFTPreviewNoteRequest {
        .midi_note = midi_note,
        .velocity = velocity,
        .instrument_id = track->default_instrument,
    });
}

std::optional<LivePreviewNote> FFTPluginController::begin_live_preview(const FFTPreviewNoteRequest& request) {
    active_preview_note_ = LivePreviewNote {
        .midi_note = request.midi_note,
        .velocity = request.velocity,
        .instrument_id = request.instrument_id,
    };
    if (preview_backend_ != nullptr) {
        preview_backend_->note_on(FFTPreviewNoteRequest {
            .midi_note = active_preview_note_->midi_note,
            .velocity = active_preview_note_->velocity,
            .instrument_id = active_preview_note_->instrument_id,
            .left_volume_override = request.left_volume_override,
            .right_volume_override = request.right_volume_override,
            .adsr1_override = request.adsr1_override,
            .adsr2_override = request.adsr2_override,
        });
    }
    return active_preview_note_;
}

void FFTPluginController::end_live_preview(int16_t midi_note) {
    if (active_preview_note_.has_value() && active_preview_note_->midi_note == midi_note) {
        if (preview_backend_ != nullptr) {
            preview_backend_->note_off(midi_note);
        }
        active_preview_note_.reset();
    }
}

}  // namespace fftplugin
