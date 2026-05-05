#include "fft_plugin/fft_smd_sequencer.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "fft_plugin/fft_smd_song_metadata_builder.h"
#include "../vendor/exmateria-spu-core/fft_smd_sequence_model.h"
#include "../vendor/exmateria-spu-core/fft_smd_sequencer_core.h"
#include "../vendor/exmateria-spu-core/fft_smd_sequencer_tools.h"
#include "fft_plugin/fft_smd_presentation_utils.h"
#include "fft_plugin/fft_spu_preview_core.h"

namespace fftplugin {

namespace {

int32_t hash_loop_path_components(
    const std::vector<int32_t>& source_ticks,
    const std::vector<int32_t>* iterations
) {
    if (source_ticks.empty()) {
        return -1;
    }
    uint32_t hash = 2166136261u;
    const auto mix = [&hash](uint32_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    for (size_t i = 0; i < source_ticks.size(); ++i) {
        mix(static_cast<uint32_t>(source_ticks[i]));
        mix(0x9E3779B9u + static_cast<uint32_t>(i));
        if (iterations != nullptr && i < iterations->size()) {
            mix(static_cast<uint32_t>((*iterations)[i]));
            mix(0x85EBCA6Bu + static_cast<uint32_t>(i));
        }
    }
    return static_cast<int32_t>(hash & 0x7fffffffu);
}

int32_t trace_loop_root_id(const fftshared::FFTSmdPlaybackTraceEvent& event) {
    return hash_loop_path_components(event.loop_source_ticks, nullptr);
}

int32_t trace_loop_instance_id(const fftshared::FFTSmdPlaybackTraceEvent& event) {
    return hash_loop_path_components(event.loop_source_ticks, &event.loop_iteration_indices);
}

std::string note_name_for_relative_key(int32_t relative_key) {
    return smd_note_name_for_relative_key(relative_key);
}

FFTSmdLaneCommandBlock command_from_trace_event(
    const fftshared::FFTSmdPlaybackTraceEvent& event,
    int32_t sequence_index
) {
    FFTSmdLaneCommandKind kind = FFTSmdLaneCommandKind::opcode;
    std::string label = event.label;
    int32_t duration_ticks = 0;

    if (event.kind == fftshared::FFTSmdPlaybackTraceKind::note) {
        kind = FFTSmdLaneCommandKind::note;
        label = note_name_for_relative_key(event.relative_key);
        duration_ticks = std::max(1, event.duration_ticks);
    } else if (event.opcode == 0x80) {
        kind = FFTSmdLaneCommandKind::rest;
        label = "Rest " + std::to_string(std::max(1, event.value));
        duration_ticks = std::max(1, event.value);
    } else if (event.opcode == 0x81) {
        kind = FFTSmdLaneCommandKind::hold;
        label = "Hold " + std::to_string(std::max(1, event.value));
        duration_ticks = std::max(1, event.value);
    } else if (event.kind == fftshared::FFTSmdPlaybackTraceKind::structure) {
        kind = FFTSmdLaneCommandKind::structure;
    } else if (event.kind == fftshared::FFTSmdPlaybackTraceKind::tempo || event.opcode == 0x97) {
        kind = FFTSmdLaneCommandKind::tempo;
        if (event.opcode == 0x97) {
            label = "TimeSig " + std::to_string(std::max(1, event.value)) +
                "/" + std::to_string(std::max(1, event.secondary_value));
        } else if (event.opcode == 0xA0) {
            label = "Tempo " + std::to_string(event.value);
        } else if (event.opcode == 0xA2) {
            label = "TmpSl " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
        }
    } else if (event.opcode == 0x98) {
        label = "RptStart " + std::to_string(event.value);
    } else if (event.opcode == 0xA3) {
        label = "A3 " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xA4) {
        label = "A4 " + std::to_string(event.value);
    } else if (event.opcode == 0xA9) {
        label = "A9 " + std::to_string(event.value);
    } else if (event.opcode == 0xAC) {
        label = smd_format_instrument_label_from_opcode_param(event.value);
    } else if (event.opcode == 0xAD) {
        label = "AD? " + std::to_string(event.value);
    } else if (event.opcode == 0x94) {
        label = "Oct " + std::to_string(event.value);
    } else if (event.opcode == 0xAE) {
        label = "Perc+";
    } else if (event.opcode == 0xAF) {
        label = "Perc-";
    } else if (event.opcode == 0xB0) {
        label = "Slur+";
    } else if (event.opcode == 0xB1) {
        label = "Slur-";
    } else if (event.opcode == 0xB2) {
        label = "B2";
    } else if (event.opcode == 0xB9) {
        label = "B9 " + std::to_string(event.value);
    } else if (event.opcode == 0xC0) {
        label = "ADSR Rst";
    } else if (event.opcode == 0xC1) {
        label = "C1 " + std::to_string(event.value);
    } else if (event.opcode == 0xC2) {
        label = "Atk " + std::to_string(event.value);
    } else if (event.opcode == 0xC4) {
        label = "SusRt " + std::to_string(event.value);
    } else if (event.opcode == 0xC5) {
        label = "Rel " + std::to_string(event.value);
    } else if (event.opcode == 0xC6) {
        label = "Slide " + std::to_string(event.value);
    } else if (event.opcode == 0xC7) {
        label = "Dec/Sus " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xC8) {
        label = "C8 " + std::to_string(event.value);
    } else if (event.opcode == 0xC9) {
        label = "Dec " + std::to_string(event.value);
    } else if (event.opcode == 0xCA) {
        label = "SusLv " + std::to_string(event.value);
    } else if (event.opcode == 0xCF) {
        label = "CF";
    } else if (event.opcode == 0xD7) {
        label = "LFO " + std::to_string(event.value);
    } else if (event.opcode == 0xD8) {
        label = "LFOlen " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xD0) {
        label = "Bend " + std::to_string(event.value);
    } else if (event.opcode == 0xD1) {
        label = "Bend+ " + std::to_string(event.value);
    } else if (event.opcode == 0xD2) {
        label = "Cond " + std::to_string(event.value);
    } else if (event.opcode == 0xD4) {
        label = "Port " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xD6) {
        label = "Detune " + std::to_string(event.value);
    } else if (event.opcode == 0xD9) {
        label = "LFOcmd " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xDA) {
        label = "FlgFE+";
    } else if (event.opcode == 0xDB) {
        label = "FlgFE-";
    } else if (event.opcode == 0xE0) {
        label = "Dyn " + std::to_string(event.value);
    } else if (event.opcode == 0xE1) {
        label = "Dyn+ " + std::to_string(event.value);
    } else if (event.opcode == 0xE2) {
        label = "Expr " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xE3) {
        label = "VolLFO " + std::to_string(event.value);
    } else if (event.opcode == 0xE4) {
        label = "VolLFOlen " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xE5) {
        label = "VolLFOcmd " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xE7) {
        label = "E7";
    } else if (event.opcode == 0xE6) {
        label = "FlgE6";
    } else if (event.opcode == 0xE8) {
        label = "Pan " + std::to_string(event.value);
    } else if (event.opcode == 0xE9) {
        label = "Pan? " + std::to_string(event.value);
    } else if (event.opcode == 0xEA) {
        label = "PanSl " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xEB) {
        label = "PanLFO " + std::to_string(event.value);
    } else if (event.opcode == 0xEC) {
        label = "PanLFOLn " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xED) {
        label = "PanLFO3 " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xEF) {
        label = "EF";
    } else if (event.opcode == 0xF4) {
        label = "F4 " + std::to_string(event.value);
    } else if (event.opcode == 0xF7) {
        label = "F7 " + std::to_string(event.value);
    } else if (event.opcode == 0xF8) {
        label = "F8 " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xF9) {
        label = "F9 " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xFB) {
        label = "FB " + std::to_string(event.value);
    } else if (event.opcode == 0xFC) {
        label = "FC " + std::to_string(event.value) + "/" + std::to_string(event.secondary_value);
    } else if (event.opcode == 0xFD) {
        label = "FD " + std::to_string(event.value);
    } else if (event.opcode == 0xFE) {
        label = "Bank " + std::to_string(event.value);
    }

    return FFTSmdLaneCommandBlock {
        .tick = event.tick,
        .authored_tick = event.source_tick,
        .loop_root_id = trace_loop_root_id(event),
        .loop_instance_id = trace_loop_instance_id(event),
        .duration_ticks = duration_ticks,
        .sequence_index = sequence_index,
        .loop_depth = event.loop_depth,
        .source_event_index = event.source_event_index,
        .opcode = event.kind == fftshared::FFTSmdPlaybackTraceKind::note ? -1 : event.opcode,
        .kind = kind,
        .label = std::move(label),
    };
}

fftshared::FFTSmdTrackEvent to_shared_event(const FFTSmdTrackEvent& event) {
    return std::visit([](const auto& value) -> fftshared::FFTSmdTrackEvent {
        using ValueType = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<ValueType, FFTSmdNoteEvent>) {
            return fftshared::FFTSmdNoteEvent {
                .velocity = value.velocity,
                .relative_key = value.relative_key,
                .delta_time = value.delta_time,
            };
        } else {
            return fftshared::FFTSmdOpcodeEvent {
                .opcode = value.opcode,
                .params = value.params,
            };
        }
    }, event);
}

fftshared::FFTSmdSequence to_shared_sequence(const FFTSmdFile& smd) {
    fftshared::FFTSmdSequence sequence;
    sequence.track_count = smd.track_count;
    sequence.initial_tempo = smd.initial_tempo;
    sequence.initial_volume = smd.initial_volume;
    sequence.assoc_wds_id = smd.assoc_wds_id;
    sequence.song_title = smd.song_title;
    sequence.track_events.reserve(smd.track_events.size());

    for (const auto& track : smd.track_events) {
        std::vector<fftshared::FFTSmdTrackEvent> shared_track;
        shared_track.reserve(track.size());
        for (const auto& event : track) {
            shared_track.push_back(to_shared_event(event));
        }
        sequence.track_events.push_back(std::move(shared_track));
    }

    return sequence;
}

std::vector<fftshared::FFTSmdInstrumentInfo> to_shared_instruments(const IFFTWavesetService& waveset_service) {
    std::vector<fftshared::FFTSmdInstrumentInfo> instruments;
    const int32_t count = waveset_service.instrument_count();
    instruments.reserve(static_cast<size_t>(count));
    for (int32_t instrument_id = 0; instrument_id < count; ++instrument_id) {
        const auto instrument = waveset_service.instrument_info(instrument_id);
        if (!instrument.has_value()) {
            instruments.push_back(fftshared::FFTSmdInstrumentInfo {});
            continue;
        }
        instruments.push_back(fftshared::FFTSmdInstrumentInfo {
            .is_null = instrument->is_null,
            .fine_tune = instrument->fine_tune,
            .adsr1 = instrument->adsr1,
            .adsr2 = instrument->adsr2,
            .loop_start = instrument->loop_start,
            .sample_offset = instrument->sample_offset,
            .sample_size = instrument->sample_size,
            .loop_offset_bytes = instrument->loop_offset_bytes,
            .has_explicit_loop_start = instrument->has_explicit_loop_start,
            .has_loop_repeat = instrument->has_loop_repeat,
            .start_offset_bytes = instrument->start_offset_bytes,
            .start_sample_skip = instrument->start_sample_skip,
        });
    }
    return instruments;
}

int32_t ticks_per_beat_for_denominator(int32_t denominator) {
    if (denominator <= 0) {
        denominator = 4;
    }
    return std::max(1, (48 * 4) / denominator);
}

}  // namespace

FFTSmdSequencer::FFTSmdSequencer(IFFTSpuCore* spu_core, const IFFTWavesetService* waveset_service)
    : spu_core_(spu_core)
    , preview_core_(dynamic_cast<FFTSpuPreviewCore*>(spu_core))
    , waveset_service_(waveset_service) {}

FFTSmdSequencer::~FFTSmdSequencer() = default;

bool FFTSmdSequencer::load_smd(const FFTSmdFile& smd, std::string* error_message) {
    if (preview_core_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Shared plugin sequencer requires FFTSpuPreviewCore";
        }
        return false;
    }
    if (waveset_service_ == nullptr || !waveset_service_->is_loaded()) {
        if (error_message != nullptr) {
            *error_message = "Waveset service is not ready";
        }
        return false;
    }

    const FFTSpuLoadResult load_result = preview_core_->load_instruments(
        waveset_service_->spu_instruments(),
        waveset_service_->adpcm_bank()
    );
    if (!load_result.ok) {
        if (error_message != nullptr) {
            *error_message = "Failed to load waveset instruments into shared SPU core";
        }
        return false;
    }

    preview_core_->reset();
    auto shared_core = std::make_unique<fftshared::FFTSmdSequencerCore>(&preview_core_->runtime());
    if (!shared_core->load_sequence(to_shared_sequence(smd), to_shared_instruments(*waveset_service_))) {
        if (error_message != nullptr) {
            *error_message = "Failed to load sequence into shared sequencer core";
        }
        return false;
    }

    shared_core_ = std::move(shared_core);
    if (track_muted_.size() < smd.track_events.size()) {
        track_muted_.resize(smd.track_events.size(), false);
    }
    if (track_soloed_.size() < smd.track_events.size()) {
        track_soloed_.resize(smd.track_events.size(), false);
    }
    for (size_t track_idx = 0; track_idx < smd.track_events.size(); ++track_idx) {
        shared_core_->set_track_muted(static_cast<int32_t>(track_idx), track_muted_[track_idx]);
        shared_core_->set_track_soloed(static_cast<int32_t>(track_idx), track_soloed_[track_idx]);
    }
    return true;
}

bool FFTSmdSequencer::tick() {
    return shared_core_ != nullptr && shared_core_->tick();
}

std::vector<int16_t> FFTSmdSequencer::render_tick_pcm16() {
    if (shared_core_ == nullptr) {
        return {};
    }
    return shared_core_->render_tick_pcm16();
}

std::vector<int16_t> FFTSmdSequencer::render_frames_only_pcm16(int32_t frame_count) {
    if (shared_core_ == nullptr) {
        return {};
    }
    return shared_core_->render_frames_only_pcm16(frame_count);
}

bool FFTSmdSequencer::has_active_audio() const {
    return shared_core_ != nullptr && shared_core_->has_active_audio();
}

bool FFTSmdSequencer::all_done() const {
    return shared_core_ == nullptr || shared_core_->all_done();
}

double FFTSmdSequencer::tempo_bpm() const {
    return shared_core_ != nullptr ? shared_core_->tempo_bpm() : 120.0;
}

double FFTSmdSequencer::samples_per_tick() const {
    return shared_core_ != nullptr ? shared_core_->samples_per_tick() : 0.0;
}

double FFTSmdSequencer::tick_accumulator() const {
    return shared_core_ != nullptr ? shared_core_->tick_accumulator() : 0.0;
}

int32_t FFTSmdSequencer::total_ticks() const {
    return shared_core_ != nullptr ? shared_core_->total_ticks() : 0;
}

std::vector<int32_t> FFTSmdSequencer::source_cursor_ticks() const {
    return shared_core_ != nullptr ? shared_core_->source_cursor_ticks() : std::vector<int32_t> {};
}

void FFTSmdSequencer::set_track_muted(int32_t track_idx, bool muted) {
    if (track_idx < 0) {
        return;
    }
    if (static_cast<size_t>(track_idx) >= track_muted_.size()) {
        track_muted_.resize(static_cast<size_t>(track_idx) + 1U, false);
    }
    track_muted_[static_cast<size_t>(track_idx)] = muted;
    if (shared_core_ != nullptr) {
        shared_core_->set_track_muted(track_idx, muted);
    }
}

void FFTSmdSequencer::set_track_soloed(int32_t track_idx, bool soloed) {
    if (track_idx < 0) {
        return;
    }
    if (static_cast<size_t>(track_idx) >= track_soloed_.size()) {
        track_soloed_.resize(static_cast<size_t>(track_idx) + 1U, false);
    }
    track_soloed_[static_cast<size_t>(track_idx)] = soloed;
    if (shared_core_ != nullptr) {
        shared_core_->set_track_soloed(track_idx, soloed);
    }
}

bool FFTSmdSequencer::track_muted(int32_t track_idx) const {
    return track_idx >= 0 &&
        static_cast<size_t>(track_idx) < track_muted_.size() &&
        track_muted_[static_cast<size_t>(track_idx)];
}

bool FFTSmdSequencer::track_soloed(int32_t track_idx) const {
    return track_idx >= 0 &&
        static_cast<size_t>(track_idx) < track_soloed_.size() &&
        track_soloed_[static_cast<size_t>(track_idx)];
}

FFTSmdSongPresentation FFTSmdSequencer::build_playback_presentation(
    const FFTSmdFile& smd,
    int32_t max_ticks,
    int32_t max_trace_events
) const {
    FFTSmdSongPresentation presentation;
    if (waveset_service_ == nullptr || !waveset_service_->is_loaded()) {
        return presentation;
    }

    FFTSpuPreviewCore trace_preview_core;
    const FFTSpuLoadResult load_result = trace_preview_core.load_instruments(
        waveset_service_->spu_instruments(),
        waveset_service_->adpcm_bank()
    );
    if (!load_result.ok) {
        return presentation;
    }

    auto shared_core = std::make_unique<fftshared::FFTSmdSequencerCore>(&trace_preview_core.runtime());
    std::vector<fftshared::FFTSmdPlaybackTraceEvent> trace_events;
    trace_events.reserve(static_cast<size_t>(std::max(0, max_trace_events)));
    shared_core->set_trace_callback([&trace_events, max_trace_events](const fftshared::FFTSmdPlaybackTraceEvent& event) {
        if (static_cast<int32_t>(trace_events.size()) < max_trace_events) {
            trace_events.push_back(event);
        }
    });

    if (!shared_core->load_sequence(to_shared_sequence(smd), to_shared_instruments(*waveset_service_))) {
        return {};
    }

    const std::vector<FFTSmdTrackSummary> summaries = build_smd_track_summaries(smd);
    presentation.tracks.reserve(smd.track_events.empty() ? 0 : (smd.track_events.size() - 1));
    std::vector<int32_t> track_slot(smd.track_events.size(), -1);
    for (size_t track_index = 0; track_index < smd.track_events.size(); ++track_index) {
        if (track_index == 0) {
            continue;
        }
        FFTSmdTrackLanePresentation track_presentation;
        track_presentation.track_index = static_cast<int32_t>(track_index);
        track_presentation.summary = summaries[track_index];
        track_slot[track_index] = static_cast<int32_t>(presentation.tracks.size());
        presentation.tracks.push_back(std::move(track_presentation));
    }

    std::vector<std::pair<int32_t, std::pair<int32_t, int32_t>>> conductor_time_sigs;
    std::vector<std::pair<int32_t, int32_t>> conductor_tempos;
    int32_t trace_sequence_index = 0;

    int32_t safety_counter = 0;
    while (shared_core->total_ticks() <= max_ticks && safety_counter < max_ticks + 1024) {
        const bool advanced = shared_core->tick();
        safety_counter += 1;
        if (!advanced) {
            break;
        }
        if (shared_core->all_done() && !shared_core->has_active_audio()) {
            break;
        }
    }

    for (const auto& event : trace_events) {
        presentation.total_ticks = std::max(
            presentation.total_ticks,
            event.tick + std::max(event.duration_ticks, 0));
        if (event.track_idx < 0 || static_cast<size_t>(event.track_idx) >= smd.track_events.size()) {
            continue;
        }

        if (event.track_idx == 0) {
            if (event.kind == fftshared::FFTSmdPlaybackTraceKind::opcode ||
                event.kind == fftshared::FFTSmdPlaybackTraceKind::structure ||
                event.kind == fftshared::FFTSmdPlaybackTraceKind::tempo) {
                presentation.conductor_markers.push_back(FFTSmdLaneMarker {
                    .tick = event.tick,
                    .authored_tick = event.source_tick,
                    .kind = event.kind == fftshared::FFTSmdPlaybackTraceKind::tempo
                        ? FFTSmdLaneMarkerKind::tempo
                        : (event.kind == fftshared::FFTSmdPlaybackTraceKind::structure
                            ? FFTSmdLaneMarkerKind::structure
                            : FFTSmdLaneMarkerKind::opcode),
                    .loop_depth = event.loop_depth,
                    .label = command_from_trace_event(event, trace_sequence_index++).label,
                });
            }
            if (event.kind == fftshared::FFTSmdPlaybackTraceKind::tempo) {
                conductor_tempos.push_back({event.tick, std::max(1, event.value)});
            } else if (event.kind == fftshared::FFTSmdPlaybackTraceKind::structure) {
            } else if (event.kind == fftshared::FFTSmdPlaybackTraceKind::opcode) {
                if (event.opcode == 0x97) {
                    conductor_time_sigs.push_back({event.tick, {std::max(1, event.value), std::max(1, event.secondary_value)}});
                }
            }
            continue;
        }

        const int32_t slot = track_slot[static_cast<size_t>(event.track_idx)];
        if (slot < 0 || static_cast<size_t>(slot) >= presentation.tracks.size()) {
            continue;
        }
        auto& track_presentation = presentation.tracks[static_cast<size_t>(slot)];

        if (event.kind == fftshared::FFTSmdPlaybackTraceKind::note) {
            track_presentation.commands.push_back(command_from_trace_event(event, trace_sequence_index++));
            if (event.relative_key >= 0 && (event.relative_key < 12 || event.relative_key == 13)) {
                track_presentation.notes.push_back(FFTSmdLaneNoteBlock {
                    .start_tick = event.tick,
                    .authored_start_tick = event.source_tick,
                    .loop_root_id = trace_loop_root_id(event),
                    .loop_instance_id = trace_loop_instance_id(event),
                    .duration_ticks = std::max(1, event.duration_ticks),
                    .relative_key = event.relative_key,
                    .has_fermata = event.has_fermata,
                    .fermata_extension_ticks = event.fermata_extension_ticks,
                    .loop_depth = event.loop_depth,
                    .source_event_index = event.source_event_index,
                });
            }
            continue;
        }

        if (event.kind == fftshared::FFTSmdPlaybackTraceKind::opcode ||
            event.kind == fftshared::FFTSmdPlaybackTraceKind::structure ||
            event.kind == fftshared::FFTSmdPlaybackTraceKind::tempo) {
            track_presentation.commands.push_back(command_from_trace_event(event, trace_sequence_index++));
        }

        FFTSmdLaneMarker marker {
            .tick = event.tick,
            .authored_tick = event.source_tick,
            .kind = event.kind == fftshared::FFTSmdPlaybackTraceKind::tempo
                ? FFTSmdLaneMarkerKind::tempo
                : (event.kind == fftshared::FFTSmdPlaybackTraceKind::structure
                    ? FFTSmdLaneMarkerKind::structure
                    : FFTSmdLaneMarkerKind::opcode),
            .loop_depth = event.loop_depth,
            .label = event.label,
        };

        if (event.kind == fftshared::FFTSmdPlaybackTraceKind::opcode && event.opcode == 0x80) {
            track_presentation.notes.push_back(FFTSmdLaneNoteBlock {
                .start_tick = event.tick,
                .authored_start_tick = event.source_tick,
                .loop_root_id = trace_loop_root_id(event),
                .loop_instance_id = trace_loop_instance_id(event),
                .duration_ticks = std::max(1, event.value),
                .relative_key = 13,
                .loop_depth = event.loop_depth,
                .source_event_index = event.source_event_index,
            });
        }

        if (event.kind == fftshared::FFTSmdPlaybackTraceKind::structure &&
            (event.opcode == 0x91 || event.opcode == 0x98 || event.opcode == 0x99 || event.opcode == 0x9A)) {
            track_presentation.loop_boundaries.push_back(FFTSmdLaneLoopBoundary {
                .tick = event.tick,
                .authored_tick = event.source_tick,
                .loop_depth = std::max(0, event.loop_depth),
                .label = event.opcode == 0x98 && event.value > 0
                    ? ("RptStart " + std::to_string(event.value))
                    : event.label,
            });
        }

        if (event.kind == fftshared::FFTSmdPlaybackTraceKind::opcode &&
            (event.opcode == 0x80 || event.opcode == 0x81)) {
            continue;
        }

        if (marker.kind == FFTSmdLaneMarkerKind::tempo) {
            presentation.conductor_markers.push_back(marker);
        } else {
            track_presentation.markers.push_back(std::move(marker));
        }
    }

    for (auto& track : presentation.tracks) {
        for (const auto& note : track.notes) {
            track.total_ticks = std::max(track.total_ticks, note.start_tick + note.duration_ticks);
        }
        for (const auto& marker : track.markers) {
            track.total_ticks = std::max(track.total_ticks, marker.tick);
        }
        presentation.total_ticks = std::max(presentation.total_ticks, track.total_ticks);
    }

    if (presentation.conductor_markers.empty()) {
        presentation.conductor_markers.push_back(FFTSmdLaneMarker {
            .tick = 0,
            .kind = FFTSmdLaneMarkerKind::tempo,
            .label = "T" + std::to_string(smd.initial_tempo),
        });
    }

    if (conductor_time_sigs.empty()) {
        conductor_time_sigs.push_back({0, {4, 4}});
    } else if (conductor_time_sigs.front().first > 0) {
        conductor_time_sigs.insert(conductor_time_sigs.begin(), {0, {4, 4}});
    }

    for (size_t i = 0; i < conductor_time_sigs.size(); ++i) {
        const int32_t start_tick = std::max(0, conductor_time_sigs[i].first);
        const int32_t end_tick = (i + 1 < conductor_time_sigs.size()) ? conductor_time_sigs[i + 1].first : presentation.total_ticks;
        if (end_tick <= start_tick) {
            continue;
        }
        const int32_t numerator = std::max(1, conductor_time_sigs[i].second.first);
        const int32_t denominator = std::max(1, conductor_time_sigs[i].second.second);
        const int32_t ticks_per_beat = ticks_per_beat_for_denominator(denominator);
        presentation.grid_segments.push_back(FFTSmdGridSegment {
            .start_tick = start_tick,
            .end_tick = end_tick,
            .numerator = numerator,
            .denominator = denominator,
            .ticks_per_beat = ticks_per_beat,
            .ticks_per_bar = std::max(1, numerator * ticks_per_beat),
        });
    }
    presentation.second_markers = build_second_markers_from_tempo_changes(
        conductor_tempos,
        presentation.total_ticks,
        smd.initial_tempo);

    return presentation;
}

}  // namespace fftplugin
