#include "fft_plugin/fft_plugin_processor_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_opcodes.h"
#include "fft_plugin/fft_plugin_state_codec.h"
#include "fft_plugin/processor/track_edit_helpers.h"
#include "fft_plugin/processor/track_edit_service.h"
#include "fft_plugin/processor/track_grouping_helpers.h"
#include "fft_plugin/processor/track_grouping_service.h"
#include "fft_plugin/processor/track_note_helpers.h"

namespace fftplugin {

namespace {

FFTSmdAuthoredOpcode make_authored_opcode(
    int32_t tick,
    int32_t stack_order,
    int32_t opcode,
    std::vector<int32_t> params = {}
) {
    return FFTSmdAuthoredOpcode {
        .tick = tick,
        .stack_order = stack_order,
        .enabled = true,
        .exact_timing = false,
        .opcode = FFTSmdOpcodeEvent {
            .opcode = opcode,
            .params = std::move(params),
        },
    };
}

FFTSmdAuthoredTrack make_blank_raw_track(
    int32_t total_ticks,
    std::vector<FFTSmdAuthoredOpcode> opcodes
) {
    FFTSmdAuthoredTrack track;
    track.total_ticks = total_ticks;
    track.spans.push_back(FFTSmdAuthoredSpan {
        .start_tick = 0,
        .total_ticks = total_ticks,
        .base_ticks = total_ticks,
        .velocity_hint = 100,
        .relative_key = 13,
    });
    track.opcodes = std::move(opcodes);
    return track;
}

FFTSmdAuthoringDocument make_new_song_template_document() {
    constexpr int32_t kSongTicks = 192;

    FFTSmdAuthoringDocument document;
    document.format_version = 4;
    document.track_count = 2;
    document.initial_tempo = 4;
    document.initial_volume = 127;
    document.assoc_wds_id = 0;
    document.song_title = "New Song";

    FFTSmdAuthoredTrack conductor_track = make_blank_raw_track(
        kSongTicks,
        {
            make_authored_opcode(0, 0, op_byte(FFTSmdOpcode::REVERB_ON)),
            make_authored_opcode(0, 1, op_byte(FFTSmdOpcode::TEMPO), {102}),
            make_authored_opcode(0, 2, op_byte(FFTSmdOpcode::TIME_SIGNATURE), {4, 4}),
            make_authored_opcode(0, 3, op_byte(FFTSmdOpcode::REPEAT), {4}),
            make_authored_opcode(kSongTicks, 4, op_byte(FFTSmdOpcode::CODA)),
            make_authored_opcode(kSongTicks, 5, op_byte(FFTSmdOpcode::END_BAR)),
        });

    FFTSmdAuthoredTrack musical_track = make_blank_raw_track(
        kSongTicks,
        {
            make_authored_opcode(0, 0, op_byte(FFTSmdOpcode::REVERB_ON)),
            make_authored_opcode(0, 1, op_byte(FFTSmdOpcode::DYNAMICS), {63}),
            make_authored_opcode(0, 2, op_byte(FFTSmdOpcode::PAN), {64}),
            make_authored_opcode(0, 3, op_byte(FFTSmdOpcode::REPEAT), {4}),
            make_authored_opcode(0, 4, op_byte(FFTSmdOpcode::INSTRUMENT), {0}),
            make_authored_opcode(0, 5, op_byte(FFTSmdOpcode::OCTAVE), {3}),
            make_authored_opcode(kSongTicks, 6, op_byte(FFTSmdOpcode::CODA)),
            make_authored_opcode(kSongTicks, 7, op_byte(FFTSmdOpcode::INSTRUMENT), {255}),
            make_authored_opcode(kSongTicks, 8, op_byte(FFTSmdOpcode::END_BAR)),
        });

    document.tracks = {conductor_track, musical_track};
    document.parts.push_back(FFTSmdAuthoringPart {
        .kind = FFTSmdAuthoringPartKind::raw_track,
        .name = "Orchestral",
        .raw_track = conductor_track,
    });
    document.parts.push_back(FFTSmdAuthoringPart {
        .kind = FFTSmdAuthoringPartKind::raw_track,
        .name = "Track 1",
        .raw_track = musical_track,
    });
    return document;
}


}  // namespace

FFTPluginProcessorCore::FFTPluginProcessorCore()
    : state_(FFTDocument::make_default_state()),
      authoring_bridge_(state_, playback_engine_),
      track_control_(state_, playback_engine_),
      transport_(playback_engine_),
      track_edit_(state_, authoring_bridge_,
                  [this](const FFTSmdCompiledDocument& compiled, std::string* err) {
                      return finalize_successful_authored_edit(compiled, err);
                  }),
      track_grouping_(state_, authoring_bridge_,
                  [this](const FFTSmdCompiledDocument& compiled, std::string* err) {
                      return finalize_successful_authored_edit(compiled, err);
                  }),
      track_note_(state_, authoring_bridge_,
                  [this](const FFTSmdCompiledDocument& compiled, std::string* err) {
                      return finalize_successful_authored_edit(compiled, err);
                  }) {}

bool FFTPluginProcessorCore::prepare_to_play(const FFTProcessSetup& setup, std::string* error_message) {
    process_setup_ = setup;
    prepared_ = supports_process_setup(setup);
    if (!prepared_) {
        last_error_ = "Only 44.1 kHz stereo output is supported by the current FFT playback core";
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        return false;
    }

    last_error_.clear();
    if (!state_.waveset_path.empty() &&
        (state_.smd_authoring.has_value() || !state_.authoring_path.empty() || !state_.smd_path.empty())) {
        std::string reload_error;
        if (!reload_from_state(&reload_error)) {
            last_error_ = reload_error;
        }
    }
    return true;
}

const FFTProcessSetup& FFTPluginProcessorCore::process_setup() const {
    return process_setup_;
}

bool FFTPluginProcessorCore::prepared() const {
    return prepared_;
}

bool FFTPluginProcessorCore::supports_process_setup(const FFTProcessSetup& setup) const {
    return setup.output_channels >= 2 &&
        std::abs(setup.sample_rate - static_cast<double>(FFTFilePlaybackEngine::kSampleRate)) < 0.5;
}

FFTFilePlaybackLoadResult FFTPluginProcessorCore::load_waveset_path(const std::string& waveset_path) {
    state_.waveset_path = waveset_path;
    FFTFilePlaybackLoadResult result = playback_engine_.load_waveset_path(waveset_path);
    sync_derived_state_from_engine();
    track_control_.apply_to_engine();
    last_error_ = result.ok ? std::string() : result.message;
    last_reload_report_.waveset_requested = !state_.waveset_path.empty();
    last_reload_report_.waveset_ok = result.ok;
    last_reload_report_.waveset_message = result.message;
    last_reload_report_.ready = playback_engine_.ready();
    return result;
}

FFTFilePlaybackLoadResult FFTPluginProcessorCore::load_smd_path(const std::string& smd_path) {
    state_.smd_path = smd_path;
    state_.authoring_path = default_authoring_path_for_smd(smd_path);
    FFTFilePlaybackLoadResult result = playback_engine_.load_smd_path(smd_path);
    sync_derived_state_from_engine();
    if (result.ok) {
        state_.smd_authoring = build_authoring_document_from_engine();
        std::string save_error;
        save_authored_document_to_disk(&save_error);
    } else {
        state_.smd_authoring.reset();
    }
    track_control_.apply_to_engine();
    last_error_ = result.ok ? std::string() : result.message;
    last_reload_report_.smd_requested = !state_.smd_path.empty();
    last_reload_report_.smd_ok = result.ok;
    last_reload_report_.smd_message = result.message;
    last_reload_report_.ready = playback_engine_.ready();
    return result;
}

FFTFilePlaybackLoadResult FFTPluginProcessorCore::load_music_document_path(const std::string& path) {
    const auto load_authored_path = [this](
        const std::string& authoring_path,
        const std::string& associated_smd_path
    ) -> FFTFilePlaybackLoadResult {
        std::string load_error;
        const auto authored = load_smd_authoring_document(authoring_path, &load_error);
        if (!authored.has_value()) {
            last_error_ = load_error;
            return FFTFilePlaybackLoadResult {
                .ok = false,
                .message = load_error,
            };
        }

        state_.authoring_path = authoring_path;
        state_.smd_authoring = authored;
        state_.smd_path = associated_smd_path;

        std::string engine_error;
        const bool ok = load_engine_from_authored_state(&engine_error);
        sync_derived_state_from_engine();
        track_control_.apply_to_engine();
        last_error_ = ok ? std::string() : engine_error;
        last_reload_report_.smd_requested = true;
        last_reload_report_.smd_ok = ok;
        last_reload_report_.smd_message = ok ? "Loaded authoring document" : engine_error;
        last_reload_report_.ready = playback_engine_.ready();
        return FFTFilePlaybackLoadResult {
            .ok = ok,
            .message = ok ? "Loaded authoring document" : engine_error,
            .instrument_count = 0,
            .track_count = static_cast<int32_t>(authored->parts.size()),
            .tempo_bpm = playback_engine_.tempo_bpm(),
        };
    };

    if (is_smd_authoring_path(path)) {
        return load_authored_path(path, std::string());
    }

    const std::string sibling_authoring_path = default_authoring_path_for_smd(path);
    {
        std::ifstream existing_authored(sibling_authoring_path, std::ios::binary);
        if (existing_authored.good()) {
            FFTFilePlaybackLoadResult result = load_authored_path(sibling_authoring_path, path);
            if (result.ok) {
                result.message = "Loaded existing authoring document";
                last_reload_report_.smd_message = result.message;
            }
            return result;
        }
    }

    return load_smd_path(path);
}

FFTFilePlaybackLoadResult FFTPluginProcessorCore::create_new_music_document_path(const std::string& authoring_path) {
    return create_music_document_from_authoring_document(
        authoring_path,
        make_new_song_template_document(),
        "Created new song document");
}

FFTFilePlaybackLoadResult FFTPluginProcessorCore::create_music_document_from_authoring_document(
    const std::string& authoring_path,
    FFTSmdAuthoringDocument document,
    const std::string& success_message
) {
    sync_legacy_raw_tracks_from_parts(document);
    document.track_count = compile_smd_authoring_document(document).smd.track_count;

    state_.authoring_path = authoring_path;
    state_.smd_path.clear();
    state_.smd_authoring = std::move(document);
    state_.editor_view.selected_track_id =
        state_.smd_authoring->parts.size() > 1 ? 1 : 0;

    std::string save_error;
    if (!save_authored_document_to_disk(&save_error)) {
        last_error_ = save_error;
        last_reload_report_.smd_requested = true;
        last_reload_report_.smd_ok = false;
        last_reload_report_.smd_message = save_error;
        last_reload_report_.ready = playback_engine_.ready();
        return FFTFilePlaybackLoadResult {
            .ok = false,
            .message = save_error,
            .track_count = 0,
            .tempo_bpm = playback_engine_.tempo_bpm(),
        };
    }

    std::string engine_error;
    const bool ok = load_engine_from_authored_state(&engine_error);
    sync_derived_state_from_engine();
    track_control_.apply_to_engine();
    last_error_ = ok ? std::string() : engine_error;
    last_reload_report_.smd_requested = true;
    last_reload_report_.smd_ok = ok;
    last_reload_report_.smd_message = ok ? success_message : engine_error;
    last_reload_report_.ready = playback_engine_.ready();
    return FFTFilePlaybackLoadResult {
        .ok = ok,
        .message = ok ? success_message : engine_error,
        .track_count = static_cast<int32_t>(state_.smd_authoring->parts.size()),
        .tempo_bpm = playback_engine_.tempo_bpm(),
    };
}

bool FFTPluginProcessorCore::export_smd_path(const std::string& path, std::string* error_message) {
    const FFTSmdGameCompileReport report = export_smd_path_for_game(path);
    if (!report.error.empty() && error_message != nullptr) {
        *error_message = report.error;
    }
    return report.error.empty();
}

FFTSmdGameCompileReport FFTPluginProcessorCore::export_smd_path_for_game(const std::string& path) {
    return export_smd_path_for_game(path, FFTSmdGameCompileBudget{});
}

FFTSmdGameCompileReport FFTPluginProcessorCore::export_smd_path_for_game(
    const std::string& path,
    const FFTSmdGameCompileBudget& budget) {
    FFTSmdGameCompileReport report;
    std::string load_error;

    if (state_.smd_authoring.has_value() || !state_.authoring_path.empty()) {
        if (!state_.smd_authoring.has_value() && !load_engine_from_authored_state(&load_error)) {
            report.error = load_error.empty() ? std::string("Failed to load authoring document") : load_error;
            report.summary = report.error;
            last_error_ = report.error;
            return report;
        }
        FFTSmdGameCompileResult compile_result =
            compile_smd_authoring_document_for_game(*state_.smd_authoring, budget);
        report = compile_result.report;
        if (!compile_result.ok) {
            last_error_ = report.error;
            return report;
        }
        std::string save_error;
        if (!save_smd_file(path, compile_result.compiled.smd, &save_error)) {
            report.error = save_error.empty() ? std::string("Failed to write SMD file") : save_error;
            report.summary += " " + report.error;
            last_error_ = report.error;
            return report;
        }
    } else if (playback_engine_.smd_file().has_value()) {
        // No authoring doc — just write the loaded SMD as-is. No reduction
        // possible; populate report with the size we shipped.
        const FFTSmdFile& smd = *playback_engine_.smd_file();
        std::string save_error;
        if (!save_smd_file(path, smd, &save_error)) {
            report.error = save_error.empty() ? std::string("Failed to write SMD file") : save_error;
            report.summary = report.error;
            last_error_ = report.error;
            return report;
        }
        std::string serialize_error;
        const auto bytes = serialize_smd_file(smd, &serialize_error);
        report.encoded_bytes = bytes.size();
        report.final_track_count = smd.track_count;
        report.pre_reduction_track_count = smd.track_count;
        report.fits_track_budget = smd.track_count <= 24;
        report.fits_byte_budget = !bytes.empty() && bytes.size() <= 65536;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "Wrote loaded SMD as-is: %d tracks, %zu bytes.",
            smd.track_count, bytes.size());
        report.summary = buf;
    } else {
        report.error = "No SMD or authoring document loaded";
        report.summary = report.error;
        last_error_ = report.error;
        return report;
    }

    last_error_.clear();
    return report;
}

bool FFTPluginProcessorCore::reload_from_state(std::string* error_message) {
    const FFTStateReloadReport report = reload_from_state_report();
    if (!report.ready) {
        last_error_ = report.summary;
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        return false;
    }
    last_error_.clear();
    return true;
}

FFTStateReloadReport FFTPluginProcessorCore::reload_from_state_report() {
    if (!prepared_) {
        last_error_ = "Processor must be prepared before loading state";
        last_reload_report_ = FFTStateReloadReport {
            .summary = last_error_,
        };
        return last_reload_report_;
    }

    FFTStateReloadReport report;
    report.waveset_requested = !state_.waveset_path.empty();
    report.smd_requested = state_.smd_authoring.has_value() || !state_.authoring_path.empty() || !state_.smd_path.empty();

    if (!state_.waveset_path.empty()) {
        const FFTFilePlaybackLoadResult waveset_result = playback_engine_.load_waveset_path(state_.waveset_path);
        report.waveset_ok = waveset_result.ok;
        report.waveset_message = waveset_result.message;
    } else {
        report.waveset_message = "No WAVESET selected";
    }

    if (state_.smd_authoring.has_value() || !state_.authoring_path.empty()) {
        std::string authored_error;
        report.smd_ok = load_engine_from_authored_state(&authored_error);
        report.smd_message = report.smd_ok ? "Loaded authored SMD document" : authored_error;
    } else if (!state_.smd_path.empty()) {
        const FFTFilePlaybackLoadResult smd_result = playback_engine_.load_smd_path(state_.smd_path);
        report.smd_ok = smd_result.ok;
        report.smd_message = smd_result.message;
        if (report.smd_ok) {
            state_.smd_authoring = build_authoring_document_from_engine();
        }
    } else {
        report.smd_message = "No SMD selected";
    }

    sync_derived_state_from_engine();
    track_control_.apply_to_engine();
    report.ready = playback_engine_.ready();

    std::ostringstream summary;
    bool wrote_summary = false;
    if (!report.waveset_requested) {
        summary << "No WAVESET selected";
        wrote_summary = true;
    } else if (!report.waveset_ok) {
        summary << "WAVESET: " << report.waveset_message;
        wrote_summary = true;
    }

    if (!report.smd_requested) {
        if (wrote_summary) {
            summary << " | ";
        }
        summary << "No SMD selected";
        wrote_summary = true;
    } else if (!report.smd_ok) {
        if (wrote_summary) {
            summary << " | ";
        }
        summary << "SMD: " << report.smd_message;
        wrote_summary = true;
    }

    if (!wrote_summary) {
        summary << (report.ready ? "Ready" : "Files loaded but playback is not ready");
    }

    report.summary = summary.str();
    last_reload_report_ = report;
    last_error_ = report.ready ? std::string() : report.summary;
    return last_reload_report_;
}

bool FFTPluginProcessorCore::convert_tracks_to_poly_track(
    const std::vector<int32_t>& track_indices,
    std::string* error_message
) {
    const bool ok = track_grouping_.convert_to_poly(track_indices, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::ungroup_poly_track(int32_t track_idx, std::string* error_message) {
    const bool ok = track_grouping_.ungroup(track_idx, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::authored_track_is_poly_track(int32_t track_idx) const {
    return track_grouping_.is_poly(track_idx);
}

void FFTPluginProcessorCore::set_transport_playing(bool playing) {
    transport_.set_transport_playing(playing);
    if (!prepared_) {
        return;
    }
    if (playing) {
        if (!playback_engine_.ready() &&
            !state_.waveset_path.empty() &&
            (state_.smd_authoring.has_value() || !state_.authoring_path.empty() || !state_.smd_path.empty())) {
            std::string reload_error;
            if (!reload_from_state(&reload_error)) {
                last_error_ = reload_error;
                transport_.set_transport_playing(false);
                return;
            }
        }
        playback_engine_.play();
        return;
    }

    playback_engine_.stop(false);
}

bool FFTPluginProcessorCore::start_local_playback(int32_t start_tick, int32_t end_tick) {
    transport_.set_local_transport_active(false);
    const int32_t clamped_start = std::max(0, start_tick);
    transport_.set_local_range(clamped_start, end_tick > clamped_start ? end_tick : -1);

    if (!prepared_) {
        last_error_ = "Playback engine is not prepared";
        return false;
    }
    if (!playback_engine_.ready() &&
        !state_.waveset_path.empty() &&
        (state_.smd_authoring.has_value() || !state_.authoring_path.empty() || !state_.smd_path.empty())) {
        std::string reload_error;
        if (!reload_from_state(&reload_error)) {
            last_error_ = reload_error;
            return false;
        }
    }
    if (!playback_engine_.seek_to_tick(transport_.local_playback_start_tick())) {
        last_error_ = playback_engine_.last_error();
        return false;
    }

    transport_.set_transport_playing(true);
    transport_.set_local_transport_active(true);
    playback_engine_.play();
    return true;
}

void FFTPluginProcessorCore::stop_local_playback(bool rewind_to_start) {
    transport_.stop_local_playback(rewind_to_start);
}

void FFTPluginProcessorCore::sync_host_transport(bool playing, int64_t host_sample_position) {
    constexpr int64_t kRestartToleranceFrames = 16;

    if (transport_.local_transport_active()) {
        transport_.set_last_host_playing(playing);
        transport_.set_last_host_sample_position(host_sample_position);
        return;
    }

    const bool have_host_position = host_sample_position >= 0;
    const bool host_jumped_backwards =
        transport_.host_jumped_backwards(host_sample_position, kRestartToleranceFrames);
    const bool host_at_start = have_host_position && host_sample_position <= kRestartToleranceFrames;

    if (!prepared_) {
        transport_.set_last_host_playing(playing);
        transport_.set_last_host_sample_position(host_sample_position);
        return;
    }

    if (!playing) {
        const bool should_rewind =
            transport_.last_host_playing() && (host_at_start || host_jumped_backwards);
        playback_engine_.stop(should_rewind);
        transport_.set_transport_playing(false);
        transport_.set_last_host_playing(false);
        transport_.set_last_host_sample_position(host_sample_position);
        return;
    }

    if (host_at_start || host_jumped_backwards) {
        if (!rewind()) {
            transport_.set_last_host_playing(playing);
            transport_.set_last_host_sample_position(host_sample_position);
            return;
        }
    }

    set_transport_playing(true);
    transport_.set_last_host_playing(true);
    transport_.set_last_host_sample_position(host_sample_position);
}

bool FFTPluginProcessorCore::transport_playing() const {
    return transport_.transport_playing();
}

bool FFTPluginProcessorCore::local_transport_active() const {
    return transport_.local_transport_active();
}

bool FFTPluginProcessorCore::rewind() {
    const bool ok = transport_.rewind(last_error_);
    if (ok) {
        track_control_.apply_to_engine();
    }
    return ok;
}

void FFTPluginProcessorCore::set_track_muted(int32_t track_idx, bool muted) {
    track_control_.set_track_muted(track_idx, muted);
}

void FFTPluginProcessorCore::set_track_soloed(int32_t track_idx, bool soloed) {
    track_control_.set_track_soloed(track_idx, soloed);
}

bool FFTPluginProcessorCore::track_muted(int32_t track_idx) const {
    return track_control_.track_muted(track_idx);
}

bool FFTPluginProcessorCore::track_soloed(int32_t track_idx) const {
    return track_control_.track_soloed(track_idx);
}

int32_t FFTPluginProcessorCore::selected_track_id() const {
    return track_control_.selected_track_id();
}

void FFTPluginProcessorCore::set_selected_track_id(int32_t track_idx) {
    track_control_.set_selected_track_id(track_idx);
}

bool FFTPluginProcessorCore::set_track_opcode_code(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    int32_t new_opcode,
    std::string* error_message
) {
    const bool ok = track_edit_.set_opcode_code(
        track_idx, source_event_index, expected_opcode, new_opcode, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::delete_track_opcode(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    std::string* error_message
) {
    const bool ok = track_edit_.delete_opcode_by_source_event(
        track_idx, source_event_index, expected_opcode, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::delete_authored_opcode(
    int32_t track_idx,
    int32_t authored_opcode_index,
    int32_t expected_opcode,
    std::string* error_message
) {
    const bool ok = track_edit_.delete_opcode_by_authored_index(
        track_idx, authored_opcode_index, expected_opcode, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::move_track_opcode(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    int32_t* moved_source_event_index,
    std::string* error_message
) {
    const bool ok = track_edit_.move_opcode_by_source_event(
        track_idx, source_event_index, expected_opcode, target_tick,
        insertion_sequence_index, moved_source_event_index, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::move_authored_opcode(
    int32_t track_idx,
    int32_t authored_opcode_index,
    int32_t expected_opcode,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    int32_t* moved_authored_opcode_index,
    std::string* error_message
) {
    const bool ok = track_edit_.move_opcode_by_authored_index(
        track_idx, authored_opcode_index, expected_opcode, target_tick,
        insertion_sequence_index, moved_authored_opcode_index, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::move_track_opcodes(
    int32_t track_idx,
    const std::vector<int32_t>& source_event_indices,
    int32_t dragged_source_event_index,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    std::vector<int32_t>* moved_source_event_indices,
    int32_t* moved_dragged_source_event_index,
    std::string* error_message
) {
    const bool ok = track_edit_.move_opcodes_by_source_events(
        track_idx, source_event_indices, dragged_source_event_index, target_tick,
        insertion_sequence_index, moved_source_event_indices, moved_dragged_source_event_index,
        error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::move_authored_opcodes(
    int32_t track_idx,
    const std::vector<int32_t>& authored_opcode_indices,
    int32_t dragged_authored_opcode_index,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    std::vector<int32_t>* moved_authored_opcode_indices,
    int32_t* moved_dragged_authored_opcode_index,
    std::string* error_message
) {
    const bool ok = track_edit_.move_opcodes_by_authored_indices(
        track_idx, authored_opcode_indices, dragged_authored_opcode_index, target_tick,
        insertion_sequence_index, moved_authored_opcode_indices, moved_dragged_authored_opcode_index,
        error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::insert_track_time(
    int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    const bool ok = track_note_.insert_track_time(track_idx, start_tick, duration_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::delete_track_time(
    int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    const bool ok = track_note_.delete_track_time(track_idx, start_tick, duration_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::replace_track_note_with_rest(
    int32_t track_idx, int32_t source_event_index, std::string* error_message
) {
    const bool ok = track_note_.replace_note_with_rest_by_source_event(track_idx, source_event_index, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::replace_authored_note_with_rest(
    int32_t track_idx, int32_t authored_span_index, std::string* error_message
) {
    const bool ok = track_note_.replace_note_with_rest_by_authored_index(track_idx, authored_span_index, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::insert_authored_poly_note(
    int32_t track_idx, int32_t note_relative_key,
    int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    const bool ok = track_note_.insert_authored_poly_note(
        track_idx, note_relative_key, start_tick, duration_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::insert_authored_poly_rest(
    int32_t track_idx, int32_t start_tick, int32_t duration_ticks, std::string* error_message
) {
    const bool ok = track_note_.insert_authored_poly_rest(track_idx, start_tick, duration_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::replace_track_rest_with_note(
    int32_t track_idx, int32_t source_event_index,
    int32_t note_relative_key, int32_t start_offset_ticks, int32_t duration_ticks,
    std::string* error_message
) {
    const bool ok = track_note_.replace_rest_with_note_by_source_event(
        track_idx, source_event_index, note_relative_key, start_offset_ticks, duration_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::replace_authored_rest_with_note(
    int32_t track_idx, int32_t authored_span_index,
    int32_t note_relative_key, int32_t start_offset_ticks, int32_t duration_ticks,
    std::string* error_message
) {
    const bool ok = track_note_.replace_rest_with_note_by_authored_index(
        track_idx, authored_span_index, note_relative_key, start_offset_ticks, duration_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::set_track_note_fermata_extension(
    int32_t track_idx, int32_t source_event_index, int32_t extension_ticks, std::string* error_message
) {
    const bool ok = track_note_.set_fermata_extension_by_source_event(
        track_idx, source_event_index, extension_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::set_authored_note_fermata_extension(
    int32_t track_idx, int32_t authored_span_index, int32_t extension_ticks, std::string* error_message
) {
    const bool ok = track_note_.set_fermata_extension_by_authored_index(
        track_idx, authored_span_index, extension_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::set_track_note_geometry(
    int32_t track_idx, int32_t source_event_index,
    int32_t start_tick, int32_t base_duration_ticks, int32_t extension_ticks,
    std::string* error_message
) {
    const bool ok = track_note_.set_note_geometry_by_source_event(
        track_idx, source_event_index, start_tick, base_duration_ticks, extension_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::set_authored_note_geometry(
    int32_t track_idx, int32_t authored_span_index,
    int32_t start_tick, int32_t base_duration_ticks, int32_t extension_ticks,
    std::string* error_message
) {
    const bool ok = track_note_.set_note_geometry_by_authored_index(
        track_idx, authored_span_index, start_tick, base_duration_ticks, extension_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::resize_track_rest_duration(
    int32_t track_idx, int32_t source_event_index, int32_t delta_ticks, std::string* error_message
) {
    const bool ok = track_note_.resize_rest_duration_by_source_event(
        track_idx, source_event_index, delta_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::resize_authored_rest_duration(
    int32_t track_idx, int32_t authored_span_index, int32_t delta_ticks, std::string* error_message
) {
    const bool ok = track_note_.resize_rest_duration_by_authored_index(
        track_idx, authored_span_index, delta_ticks, error_message);
    if (!ok && error_message != nullptr) last_error_ = *error_message;
    return ok;
}

bool FFTPluginProcessorCore::insert_track_opcode(
    int32_t track_idx,
    int32_t target_tick,
    int32_t insertion_sequence_index,
    int32_t opcode,
    const std::vector<int32_t>& params,
    int32_t* inserted_source_event_index,
    std::string* error_message
) {
    const bool ok = track_edit_.insert_opcode(
        track_idx, target_tick, insertion_sequence_index, opcode, params,
        inserted_source_event_index, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::set_track_source_opcode_disabled(
    int32_t track_idx,
    int32_t source_event_index,
    bool disabled,
    std::string* error_message
) {
    const bool ok = track_edit_.set_source_opcode_disabled(
        track_idx, source_event_index, disabled, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::track_source_opcode_disabled(int32_t track_idx, int32_t source_event_index) const {
    return playback_engine_.track_source_opcode_disabled(track_idx, source_event_index);
}

bool FFTPluginProcessorCore::set_track_generic_opcode_param_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    int32_t value,
    int32_t min_value,
    int32_t max_value,
    std::string* error_message
) {
    const bool ok = track_edit_.set_param(
        track_idx, source_event_index, expected_opcode, value, min_value, max_value, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::set_track_generic_opcode_param_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t expected_opcode,
    const std::vector<int32_t>& values,
    std::string* error_message
) {
    const bool ok = track_edit_.set_param_values(
        track_idx, source_event_index, expected_opcode, values, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

bool FFTPluginProcessorCore::set_track_dynamics_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t dynamics_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::DYNAMICS), dynamics_value, 0, 127, error_message);
}

bool FFTPluginProcessorCore::set_track_pan_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t pan_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::PAN), pan_value, 0, 127, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_attack_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t attack_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR_ATTACK), attack_value, 0, 127, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_sustain_rate_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t sustain_rate_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR_SUSTAIN_RATE), sustain_rate_value, 0, 127, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_release_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t release_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR_RELEASE), release_value, 0, 31, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_decay_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t decay_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR_DECAY), decay_value, 0, 15, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_sustain_level_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t sustain_level_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR_SUSTAIN_LEVEL), sustain_level_value, 0, 15, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_decay_sustain_opcode_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t decay_value,
    int32_t sustain_level_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_values(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR_DECAY_AND_SUSTAIN_LEVEL),
        {decay_value, sustain_level_value}, error_message);
}

bool FFTPluginProcessorCore::set_track_pitch_lfo_depth_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t depth_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::LFO_DEPTH_PITCH), depth_value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_pitch_lfo_opcode_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t length_value,
    int32_t signed_shape_value,
    int32_t depth_value,
    std::string* error_message
) {
    const int32_t raw_shape_value = (signed_shape_value < 0)
        ? (signed_shape_value + 256)
        : signed_shape_value;
    return set_track_generic_opcode_param_values(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::LFO_LENGTH_PITCH),
        {length_value, raw_shape_value, depth_value}, error_message);
}

bool FFTPluginProcessorCore::set_track_pitch_bend_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t bend_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::SET_PITCH_BEND), bend_value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_conditional_seq_flag_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t flag_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::CONDITIONAL_SEQ_FLAG), flag_value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_detune_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t detune_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::DETUNE), detune_value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_unknown_ad_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::UNKNOWN_AD), value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_adsr_slide_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::ADSR1_LOWNIBBLE_SLIDE_TARGET), value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_volume_lfo_depth_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t depth_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::LFO_DEPTH_VOLUME), depth_value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_tempo_slide_opcode_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t first_value,
    int32_t second_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_values(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::TEMPO_SLIDE),
        {first_value, second_value}, error_message);
}

bool FFTPluginProcessorCore::set_track_volume_lfo_opcode_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t length_value,
    int32_t signed_shape_value,
    int32_t depth_value,
    std::string* error_message
) {
    const int32_t raw_shape_value = (signed_shape_value < 0)
        ? (256 + signed_shape_value)
        : signed_shape_value;
    return set_track_generic_opcode_param_values(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::LFO_LENGTH_VOLUME),
        {length_value, raw_shape_value, depth_value}, error_message);
}

bool FFTPluginProcessorCore::set_track_octave_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t octave_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::OCTAVE), octave_value, 0, 8, error_message);
}

bool FFTPluginProcessorCore::set_track_bank_select_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t bank_value,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::BANK_SELECT), bank_value, 0, 255, error_message);
}

bool FFTPluginProcessorCore::set_track_tempo_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t tempo_value,
    std::string* error_message
) {
    if (!set_track_generic_opcode_param_value(
            track_idx, source_event_index, op_byte(FFTSmdOpcode::TEMPO),
            tempo_value, 1, 255, error_message)) {
        return false;
    }
    // SMD header byte 0x1B = document.initial_tempo is what the engine
    // reads as the starting tempo; TEMPO opcodes only fire when the
    // sequencer's playhead reaches their tick. If the user edits the
    // tempo opcode at tick 0 (or the earliest one in the conductor) and
    // initial_tempo isn't kept in sync, playback starts at the old
    // tempo and the user perceives no change.
    if (state_.smd_authoring.has_value()) {
        const auto* opcodes = authored_part_opcodes(&*state_.smd_authoring, track_idx);
        if (opcodes != nullptr) {
            int32_t earliest_tempo_tick = std::numeric_limits<int32_t>::max();
            for (const auto& op : *opcodes) {
                if (op.opcode.opcode == op_byte(FFTSmdOpcode::TEMPO)) {
                    earliest_tempo_tick = std::min(earliest_tempo_tick, op.tick);
                }
            }
            for (const auto& op : *opcodes) {
                if (op.opcode.opcode == op_byte(FFTSmdOpcode::TEMPO) &&
                    op.tick == earliest_tempo_tick &&
                    !op.opcode.params.empty() &&
                    op.opcode.params[0] == tempo_value &&
                    state_.smd_authoring->initial_tempo != tempo_value) {
                    state_.smd_authoring->initial_tempo = tempo_value;
                    sync_legacy_raw_tracks_from_parts(*state_.smd_authoring);
                    const auto compiled = compile_smd_authoring_document(*state_.smd_authoring);
                    finalize_successful_authored_edit(compiled, error_message);
                    break;
                }
            }
        }
    }
    return true;
}

bool FFTPluginProcessorCore::set_track_time_signature_opcode_values(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t numerator,
    int32_t denominator,
    std::string* error_message
) {
    return set_track_generic_opcode_param_values(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::TIME_SIGNATURE),
        {numerator, denominator}, error_message);
}

bool FFTPluginProcessorCore::set_track_instrument_opcode_value(
    int32_t track_idx,
    int32_t source_event_index,
    int32_t instrument_id,
    std::string* error_message
) {
    return set_track_generic_opcode_param_value(
        track_idx, source_event_index, op_byte(FFTSmdOpcode::INSTRUMENT),
        instrument_id, 0, 255, error_message);
}

int32_t FFTPluginProcessorCore::track_transposition(int32_t track_idx) const {
    return track_edit_.track_transposition(track_idx);
}

bool FFTPluginProcessorCore::set_track_transposition(
    int32_t track_idx,
    int32_t semitones,
    std::string* error_message
) {
    const bool ok = track_edit_.set_track_transposition(track_idx, semitones, error_message);
    if (!ok && error_message != nullptr) {
        last_error_ = *error_message;
    }
    return ok;
}

void FFTPluginProcessorCore::process(float* output_left, float* output_right, int32_t frame_count) {
    zero_outputs(output_left, output_right, frame_count);
    if (!prepared_ || frame_count <= 0 || output_left == nullptr || output_right == nullptr) {
        return;
    }

    const std::vector<float> interleaved = playback_engine_.render_interleaved_f32(frame_count);
    for (int32_t frame = 0; frame < frame_count; ++frame) {
        const size_t base = static_cast<size_t>(frame) * 2U;
        output_left[frame] = base < interleaved.size() ? interleaved[base] : 0.0F;
        output_right[frame] = (base + 1) < interleaved.size() ? interleaved[base + 1] : 0.0F;
    }
    transport_.post_render_check();
}

void FFTPluginProcessorCore::process_interleaved(float* output, int32_t frame_count, int32_t output_channels) {
    if (output == nullptr || frame_count <= 0 || output_channels <= 0) {
        return;
    }
    std::fill_n(output, static_cast<size_t>(frame_count) * static_cast<size_t>(output_channels), 0.0F);
    if (!prepared_) {
        return;
    }

    const std::vector<float> interleaved = playback_engine_.render_interleaved_f32(frame_count);
    for (int32_t frame = 0; frame < frame_count; ++frame) {
        const size_t src_base = static_cast<size_t>(frame) * 2U;
        const size_t dst_base = static_cast<size_t>(frame) * static_cast<size_t>(output_channels);
        if (src_base < interleaved.size()) {
            output[dst_base] = interleaved[src_base];
        }
        if (output_channels > 1 && (src_base + 1) < interleaved.size()) {
            output[dst_base + 1] = interleaved[src_base + 1];
        }
    }
    transport_.post_render_check();
}

std::vector<uint8_t> FFTPluginProcessorCore::serialize_state() const {
    FFTPluginState serialized_state = state_;
    if (const auto authored = build_authoring_document_from_engine(); authored.has_value()) {
        serialized_state.smd_authoring = std::move(authored);
    }
    return serialize_plugin_state_minimal(serialized_state);
}

bool FFTPluginProcessorCore::restore_state(const std::vector<uint8_t>& bytes, std::string* error_message) {
    if (!deserialize_plugin_state_minimal(bytes, &state_, error_message)) {
        last_error_ = error_message != nullptr ? *error_message : "Failed to deserialize plugin state";
        return false;
    }
    if (prepared_ &&
        (!state_.waveset_path.empty() ||
         state_.smd_authoring.has_value() ||
         !state_.authoring_path.empty() ||
         !state_.smd_path.empty())) {
        return reload_from_state(error_message);
    }
    return true;
}

const FFTPluginState& FFTPluginProcessorCore::state() const {
    return state_;
}

const FFTFilePlaybackEngine& FFTPluginProcessorCore::playback_engine() const {
    return playback_engine_;
}

void FFTPluginProcessorCore::set_waveset_path_in_state(std::string path) {
    state_.waveset_path = std::move(path);
}

void FFTPluginProcessorCore::set_smd_path_in_state(std::string path) {
    state_.smd_path = std::move(path);
}

void FFTPluginProcessorCore::clear_authoring_path_in_state() {
    state_.authoring_path.clear();
}

int32_t FFTPluginProcessorCore::current_playback_tick() const {
    return playback_engine_.current_playback_tick();
}

std::vector<int32_t> FFTPluginProcessorCore::current_source_track_ticks() const {
    return playback_engine_.current_source_track_ticks();
}

const std::string& FFTPluginProcessorCore::last_error() const {
    return last_error_;
}

const FFTStateReloadReport& FFTPluginProcessorCore::last_reload_report() const {
    return last_reload_report_;
}

void FFTPluginProcessorCore::zero_outputs(float* output_left, float* output_right, int32_t frame_count) const {
    if (frame_count <= 0) {
        return;
    }
    if (output_left != nullptr) {
        std::fill_n(output_left, frame_count, 0.0F);
    }
    if (output_right != nullptr) {
        std::fill_n(output_right, frame_count, 0.0F);
    }
}

void FFTPluginProcessorCore::sync_state_from_engine() {
    state_.waveset_path = playback_engine_.waveset_path();
    state_.smd_path = playback_engine_.smd_path();
    state_.smd_authoring = build_authoring_document_from_engine();
    sync_derived_state_from_engine();
}

void FFTPluginProcessorCore::sync_derived_state_from_engine() {
    state_.selected_bank_name = playback_engine_.has_waveset() ? playback_engine_.bank_name() : std::string();
}

std::optional<FFTSmdAuthoringDocument> FFTPluginProcessorCore::build_authoring_document_from_engine() const {
    return authoring_bridge_.build_from_engine();
}

bool FFTPluginProcessorCore::ensure_authored_document_loaded(std::string* error_message) {
    return authoring_bridge_.ensure_loaded(error_message);
}

bool FFTPluginProcessorCore::load_engine_from_authored_state(std::string* error_message) {
    const bool ok = authoring_bridge_.load_engine_from_state(error_message);
    if (!ok) {
        last_error_ = error_message != nullptr ? *error_message : "Failed to load authored SMD";
    }
    return ok;
}

bool FFTPluginProcessorCore::save_authored_document_to_disk(std::string* error_message) const {
    return authoring_bridge_.save_to_disk(error_message);
}

bool FFTPluginProcessorCore::finalize_successful_authored_edit(
    const FFTSmdCompiledDocument& compiled,
    std::string* error_message
) {
    const bool ok = playback_engine_.load_compiled_smd_document(compiled, state_.smd_path, error_message);
    if (!ok) {
        last_error_ = error_message != nullptr ? *error_message : "Failed to load authored SMD";
        return false;
    }

    sync_derived_state_from_engine();
    std::string save_error;
    if (!save_authored_document_to_disk(&save_error) && !state_.authoring_path.empty()) {
        last_error_ = save_error;
    } else {
        last_error_.clear();
    }
    track_control_.apply_to_engine();
    if (transport_.transport_playing()) {
        playback_engine_.play();
    }
    return true;
}

void FFTPluginProcessorCore::finalize_successful_edit() {
    sync_state_from_engine();
    std::string save_error;
    if (!save_authored_document_to_disk(&save_error) && !state_.authoring_path.empty()) {
        last_error_ = save_error;
    } else {
        last_error_.clear();
    }
    track_control_.apply_to_engine();
    if (transport_.transport_playing()) {
        playback_engine_.play();
    }
}

}  // namespace fftplugin
