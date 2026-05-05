#include "FFTJuceAudioProcessor.h"

#include "FFTMidiImport.h"
#include "FFTJuceAudioProcessorEditor.h"
#include "FFTJucePaths.h"
#include "fft_plugin/fft_pitch_tools.h"
#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_smd_inspector.h"
#include "fft_plugin/fft_smd_presentation_utils.h"
#include "fft_plugin/fft_smd_validation.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace fftplugin {
namespace jucewrap {

namespace {

// Envelope for getStateInformation/setStateInformation: lets us prepend the
// JUCE wrapper's MIDI provenance to the core's serialized state without
// breaking saves written before this version. Old saves are raw core bytes;
// new saves start with kStateEnvelopeMagic.
constexpr char kStateEnvelopeMagic[8] = {'F', 'F', 'T', 'P', 'L', 'G', '\0', '\0'};
constexpr uint32_t kStateEnvelopeVersion = 1;
constexpr uint32_t kProvenanceVersion = 1;

void write_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void write_i32_le(std::vector<uint8_t>& out, int32_t value) {
    write_u32_le(out, static_cast<uint32_t>(value));
}

void write_string(std::vector<uint8_t>& out, const std::string& value) {
    write_u32_le(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

bool read_u32_le(const uint8_t* data, size_t size, size_t& cursor, uint32_t& out) {
    if (cursor + 4 > size) {
        return false;
    }
    out = static_cast<uint32_t>(data[cursor])
        | (static_cast<uint32_t>(data[cursor + 1]) << 8)
        | (static_cast<uint32_t>(data[cursor + 2]) << 16)
        | (static_cast<uint32_t>(data[cursor + 3]) << 24);
    cursor += 4;
    return true;
}

bool read_i32_le(const uint8_t* data, size_t size, size_t& cursor, int32_t& out) {
    uint32_t raw = 0;
    if (!read_u32_le(data, size, cursor, raw)) {
        return false;
    }
    out = static_cast<int32_t>(raw);
    return true;
}

bool read_string(const uint8_t* data, size_t size, size_t& cursor, std::string& out) {
    uint32_t length = 0;
    if (!read_u32_le(data, size, cursor, length)) {
        return false;
    }
    if (cursor + length > size) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data + cursor), length);
    cursor += length;
    return true;
}

std::vector<uint8_t> serialize_provenance(
    const std::vector<FFTImportedMidiPartProvenance>& parts) {
    std::vector<uint8_t> out;
    write_u32_le(out, kProvenanceVersion);
    write_u32_le(out, static_cast<uint32_t>(parts.size()));
    for (const auto& part : parts) {
        write_string(out, part.part_name);
        write_string(out, part.source_name);
        write_i32_le(out, part.gm_program);
        write_u32_le(out, static_cast<uint32_t>(part.notes_by_authored_index.size()));
        for (const auto& note : part.notes_by_authored_index) {
            write_i32_le(out, note.source_track_index);
            write_i32_le(out, note.channel);
            write_i32_le(out, note.gm_program);
            write_i32_le(out, note.midi_note);
            write_i32_le(out, note.velocity);
            write_i32_le(out, note.gm_volume);
            write_i32_le(out, note.gm_pan);
            write_i32_le(out, note.gm_expression);
            write_i32_le(out, note.start_tick);
            write_i32_le(out, note.duration_ticks);
            write_string(out, note.source_name);
        }
    }
    return out;
}

bool deserialize_provenance(
    const uint8_t* data,
    size_t size,
    std::vector<FFTImportedMidiPartProvenance>& out) {
    out.clear();
    size_t cursor = 0;
    uint32_t version = 0;
    if (!read_u32_le(data, size, cursor, version) || version != kProvenanceVersion) {
        return false;
    }
    uint32_t part_count = 0;
    if (!read_u32_le(data, size, cursor, part_count)) {
        return false;
    }
    out.reserve(part_count);
    for (uint32_t i = 0; i < part_count; ++i) {
        FFTImportedMidiPartProvenance part;
        if (!read_string(data, size, cursor, part.part_name) ||
            !read_string(data, size, cursor, part.source_name) ||
            !read_i32_le(data, size, cursor, part.gm_program)) {
            return false;
        }
        uint32_t note_count = 0;
        if (!read_u32_le(data, size, cursor, note_count)) {
            return false;
        }
        part.notes_by_authored_index.reserve(note_count);
        for (uint32_t n = 0; n < note_count; ++n) {
            FFTImportedMidiNoteProvenance note;
            if (!read_i32_le(data, size, cursor, note.source_track_index) ||
                !read_i32_le(data, size, cursor, note.channel) ||
                !read_i32_le(data, size, cursor, note.gm_program) ||
                !read_i32_le(data, size, cursor, note.midi_note) ||
                !read_i32_le(data, size, cursor, note.velocity) ||
                !read_i32_le(data, size, cursor, note.gm_volume) ||
                !read_i32_le(data, size, cursor, note.gm_pan) ||
                !read_i32_le(data, size, cursor, note.gm_expression) ||
                !read_i32_le(data, size, cursor, note.start_tick) ||
                !read_i32_le(data, size, cursor, note.duration_ticks) ||
                !read_string(data, size, cursor, note.source_name)) {
                return false;
            }
            part.notes_by_authored_index.push_back(std::move(note));
        }
        out.push_back(std::move(part));
    }
    return true;
}

std::string unwound_insert_debug_log_path() {
    if (const char* temp = std::getenv("TEMP"); temp != nullptr && *temp != '\0') {
        return std::string(temp) + "\\fft_unwound_insert_debug.log";
    }
    return "C:\\Windows\\Temp\\fft_unwound_insert_debug.log";
}

void reset_unwound_insert_debug_log(const std::string& header) {
    const std::filesystem::path path(unwound_insert_debug_log_path());
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << header << "\n";
}

void append_unwound_insert_debug_line(const std::string& line) {
    const std::filesystem::path path(unwound_insert_debug_log_path());
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
        return;
    }
    out << line << "\n";
}

std::string make_unwound_debug_session_header(const char* origin) {
    std::ostringstream header;
    header << "=== SESSION origin=" << (origin != nullptr ? origin : "unknown")
           << " epoch=" << static_cast<long long>(std::time(nullptr))
           << " ===";
    return header.str();
}

std::vector<float> load_wav_interleaved_f32(const juce::File& file) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) {
        return {};
    }

    juce::AudioBuffer<float> pcm(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&pcm, 0, static_cast<int>(reader->lengthInSamples), 0, true, true)) {
        return {};
    }

    std::vector<float> interleaved(static_cast<size_t>(reader->lengthInSamples) * 2U, 0.0f);
    const float* left = pcm.getReadPointer(0);
    const float* right = pcm.getNumChannels() > 1 ? pcm.getReadPointer(1) : left;
    for (int64_t frame = 0; frame < reader->lengthInSamples; ++frame) {
        const size_t base = static_cast<size_t>(frame) * 2U;
        interleaved[base] = left[frame];
        interleaved[base + 1] = right[frame];
    }
    return interleaved;
}

int32_t preview_velocity_to_volume(int16_t velocity) {
    const int32_t clamped = std::clamp(static_cast<int32_t>(velocity), 0, 127);
    return std::min(0x3FFF, clamped << 7);
}

std::vector<float> render_fft_preview_note_buffer(
    const FFTWavesetFileService& waveset_service,
    const FFTPreviewNoteRequest& request,
    int32_t sample_rate,
    int duration_ms,
    int release_ms
) {
    if (!waveset_service.is_loaded()) {
        return {};
    }

    const auto instrument_info = waveset_service.instrument_info(request.instrument_id);
    if (!instrument_info.has_value() || instrument_info->is_null) {
        return {};
    }

    FFTSpuPreviewCore preview_core;
    const FFTSpuLoadResult load_result =
        preview_core.load_instruments(waveset_service.spu_instruments(), waveset_service.adpcm_bank());
    if (!load_result.ok) {
        return {};
    }

    preview_core.reset();
    preview_core.set_reverb_enabled(true);

    const int32_t voice_index = 0;
    const int32_t shifted_midi_note = std::clamp(
        static_cast<int32_t>(request.midi_note) + request.octave_shift,
        0,
        127);
    const int32_t pre_pitch = fft_pre_pitch_from_note(shifted_midi_note, instrument_info->fine_tune);
    const int32_t raw_pitch = fft_raw_pitch_from_pre_pitch(pre_pitch);
    const int32_t default_volume = preview_velocity_to_volume(request.velocity);
    const int32_t left_volume = request.left_volume_override.value_or(default_volume);
    const int32_t right_volume = request.right_volume_override.value_or(default_volume);
    const int32_t adsr1 = request.adsr1_override.value_or(instrument_info->adsr1);
    const int32_t adsr2 = request.adsr2_override.value_or(instrument_info->adsr2);

    preview_core.set_voice_pre_pitch(voice_index, pre_pitch);
    preview_core.key_on(FFTSpuVoiceStartRequest {
        .voice_index = voice_index,
        .instrument_index = request.instrument_id,
        .pitch = raw_pitch,
        .left_volume = left_volume,
        .right_volume = right_volume,
        .adsr1 = adsr1,
        .adsr2 = adsr2,
        .reverb = true,
    });

    const int32_t sustain_frames = std::max(1, (sample_rate * std::max(1, duration_ms)) / 1000);
    std::vector<int16_t> sustain_pcm = preview_core.render_interleaved_pcm16(sustain_frames);
    preview_core.key_off(voice_index);
    const int32_t release_frames = std::max(0, (sample_rate * std::max(0, release_ms)) / 1000);
    std::vector<int16_t> release_pcm = release_frames > 0
        ? preview_core.render_interleaved_pcm16(release_frames)
        : std::vector<int16_t> {};

    std::vector<float> interleaved;
    interleaved.reserve(static_cast<size_t>(sustain_pcm.size() + release_pcm.size()));
    const auto append_pcm = [&interleaved](const std::vector<int16_t>& pcm) {
        for (const int16_t sample : pcm) {
            interleaved.push_back(static_cast<float>(sample) / 32768.0f);
        }
    };
    append_pcm(sustain_pcm);
    append_pcm(release_pcm);
    return interleaved;
}

bool is_non_time_opcode_code(int opcode) {
    return opcode >= 0 && opcode != 0x80 && opcode != 0x81;
}

FFTSmdLaneInsertAnchor build_insert_anchor_local(
    int32_t tick,
    int32_t insertion_sequence_index,
    int32_t source_event_index,
    int32_t authored_opcode_index,
    int32_t authored_span_index,
    int32_t opcode,
    FFTSmdLaneInsertAnchorKind kind,
    std::string label
) {
    return FFTSmdLaneInsertAnchor {
        .tick = tick,
        .insertion_sequence_index = insertion_sequence_index,
        .authored_opcode_index = authored_opcode_index,
        .authored_span_index = authored_span_index,
        .source_event_index = source_event_index,
        .opcode = opcode,
        .kind = kind,
        .label = std::move(label),
    };
}

void append_measure_insert_anchors_local(
    FFTSmdTrackLanePresentation& track_presentation,
    const std::vector<FFTSmdGridSegment>& grid_segments
) {
    int32_t bar_number = 1;
    for (const auto& segment : grid_segments) {
        for (int32_t bar_tick = segment.start_tick; bar_tick < segment.end_tick; bar_tick += segment.ticks_per_bar) {
            if (bar_tick <= 0) {
                bar_number += 1;
                continue;
            }
            const auto insertion_it = std::lower_bound(
                track_presentation.commands.begin(),
                track_presentation.commands.end(),
                bar_tick,
                [](const FFTSmdLaneCommandBlock& command, int32_t tick) {
                    return command.tick < tick;
                });
            const int32_t insertion_index = static_cast<int32_t>(std::distance(track_presentation.commands.begin(), insertion_it));
            track_presentation.insert_anchors.push_back(build_insert_anchor_local(
                bar_tick,
                insertion_index,
                -1,
                -1,
                -1,
                -1,
                FFTSmdLaneInsertAnchorKind::measure,
                "Bar " + std::to_string(bar_number)));
            bar_number += 1;
        }
    }
}

void annotate_raw_source_track_presentation(
    FFTSmdTrackLanePresentation& track,
    const FFTSmdAuthoredTrack& authored_track,
    const FFTSmdCompiledDocument& compiled,
    size_t part_index
) {
    const auto authored_rest_index_for_tick = [&authored_track](int32_t tick) -> int32_t {
        for (size_t authored_index = 0; authored_index < authored_track.spans.size(); ++authored_index) {
            const auto& span = authored_track.spans[authored_index];
            if (span.relative_key != 13) {
                continue;
            }
            if (tick >= span.start_tick && tick < span.start_tick + span.total_ticks) {
                return static_cast<int32_t>(authored_index);
            }
        }
        return -1;
    };

    std::unordered_map<int32_t, int32_t> authored_opcode_index_by_source;
    std::unordered_map<int32_t, int32_t> authored_span_index_by_source;
    if (part_index < compiled.authored_opcode_source_indices.size()) {
        for (size_t authored_index = 0; authored_index < compiled.authored_opcode_source_indices[part_index].size(); ++authored_index) {
            authored_opcode_index_by_source[compiled.authored_opcode_source_indices[part_index][authored_index]] =
                static_cast<int32_t>(authored_index);
        }
    }
    if (part_index < compiled.authored_span_source_indices.size()) {
        for (size_t authored_index = 0; authored_index < compiled.authored_span_source_indices[part_index].size(); ++authored_index) {
            authored_span_index_by_source[compiled.authored_span_source_indices[part_index][authored_index]] =
                static_cast<int32_t>(authored_index);
        }
    }

    for (auto& command : track.commands) {
        if (command.source_event_index < 0) {
            continue;
        }
        const auto opcode_it = authored_opcode_index_by_source.find(command.source_event_index);
        if (opcode_it != authored_opcode_index_by_source.end()) {
            command.authored_opcode_index = opcode_it->second;
            if (static_cast<size_t>(opcode_it->second) < authored_track.opcodes.size()) {
                command.authored_tick = authored_track.opcodes[static_cast<size_t>(opcode_it->second)].tick;
                command.tick = command.authored_tick;
            }
        }
    }
    for (auto& note : track.notes) {
        int32_t authored_index = -1;
        if (note.source_event_index >= 0) {
            const auto span_it = authored_span_index_by_source.find(note.source_event_index);
            if (span_it != authored_span_index_by_source.end()) {
                authored_index = span_it->second;
            }
        }
        if (authored_index < 0 && note.relative_key == 13) {
            authored_index = authored_rest_index_for_tick(note.start_tick);
        }
        if (authored_index >= 0 &&
            static_cast<size_t>(authored_index) < authored_track.spans.size()) {
            note.authored_span_index = authored_index;
            note.authored_start_tick = authored_track.spans[static_cast<size_t>(authored_index)].start_tick;
        }
    }
    for (auto& anchor : track.insert_anchors) {
        if (anchor.source_event_index < 0) {
            continue;
        }
        if (anchor.kind == FFTSmdLaneInsertAnchorKind::command) {
            const auto opcode_it = authored_opcode_index_by_source.find(anchor.source_event_index);
            if (opcode_it != authored_opcode_index_by_source.end()) {
                anchor.authored_opcode_index = opcode_it->second;
                if (static_cast<size_t>(opcode_it->second) < authored_track.opcodes.size()) {
                    anchor.tick = authored_track.opcodes[static_cast<size_t>(opcode_it->second)].tick;
                }
            }
        } else {
            int32_t authored_index = -1;
            const auto span_it = authored_span_index_by_source.find(anchor.source_event_index);
            if (span_it != authored_span_index_by_source.end()) {
                authored_index = span_it->second;
            } else if (anchor.kind == FFTSmdLaneInsertAnchorKind::rest_start) {
                authored_index = authored_rest_index_for_tick(anchor.tick);
            }
            if (authored_index >= 0 &&
                static_cast<size_t>(authored_index) < authored_track.spans.size()) {
                anchor.authored_span_index = authored_index;
                anchor.tick = authored_track.spans[static_cast<size_t>(authored_index)].start_tick;
            }
        }
    }
    for (auto& marker : track.markers) {
        const auto matched_command = std::find_if(
            track.commands.begin(),
            track.commands.end(),
            [&marker](const FFTSmdLaneCommandBlock& command) {
                return command.tick == marker.tick && command.label == marker.label;
            });
        if (matched_command != track.commands.end()) {
            marker.authored_tick = matched_command->authored_tick;
            marker.tick = matched_command->tick;
        }
    }
}

FFTSmdTrackLanePresentation build_poly_source_track_presentation(
    const FFTSmdAuthoredPolyTrack& poly_track,
    int32_t part_index,
    const std::vector<FFTSmdGridSegment>& grid_segments
) {
    struct PolyCommandEntry {
        bool is_opcode = false;
        int32_t tick = 0;
        int32_t secondary_order = 0;
        int32_t authored_index = -1;
        int32_t relative_key = 13;
        int32_t duration_ticks = 0;
    };

    FFTSmdTrackLanePresentation track;
    track.track_index = part_index;
    track.total_ticks = poly_track.total_ticks;
    track.polyphonic_notes = true;
    track.summary.track_index = part_index;
    track.summary.note_count = static_cast<int32_t>(poly_track.notes.size());
    track.summary.opcode_count = static_cast<int32_t>(poly_track.opcodes.size());
    track.summary.event_count = track.summary.note_count + track.summary.opcode_count;
    track.insert_anchors.push_back(build_insert_anchor_local(
        0,
        0,
        -1,
        -1,
        -1,
        -1,
        FFTSmdLaneInsertAnchorKind::track_start,
        "Track Start"));

    std::vector<PolyCommandEntry> command_entries;
    command_entries.reserve(poly_track.notes.size() + poly_track.opcodes.size());

    for (size_t note_index = 0; note_index < poly_track.notes.size(); ++note_index) {
        const auto& note = poly_track.notes[note_index];
        track.notes.push_back(FFTSmdLaneNoteBlock {
            .start_tick = note.start_tick,
            .authored_start_tick = note.start_tick,
            .authored_span_index = static_cast<int32_t>(note_index),
            .duration_ticks = note.total_ticks,
            .relative_key = note.relative_key,
            .has_fermata = note.base_ticks < note.total_ticks,
            .fermata_extension_ticks = std::max(0, note.total_ticks - note.base_ticks),
            .source_event_index = static_cast<int32_t>(note_index),
        });
        command_entries.push_back(PolyCommandEntry {
            .is_opcode = false,
            .tick = note.start_tick,
            .secondary_order = static_cast<int32_t>(note_index),
            .authored_index = static_cast<int32_t>(note_index),
            .relative_key = note.relative_key,
            .duration_ticks = note.total_ticks,
        });
    }

    std::vector<std::pair<int32_t, int32_t>> active_ranges;
    active_ranges.reserve(poly_track.notes.size());
    for (const auto& note : poly_track.notes) {
        const int32_t end_tick = note.start_tick + note.total_ticks;
        if (end_tick <= note.start_tick) {
            continue;
        }
        active_ranges.emplace_back(note.start_tick, end_tick);
    }
    std::stable_sort(
        active_ranges.begin(),
        active_ranges.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
            }
            return lhs.second < rhs.second;
        });

    std::vector<std::pair<int32_t, int32_t>> merged_ranges;
    for (const auto& range : active_ranges) {
        if (merged_ranges.empty() || range.first > merged_ranges.back().second) {
            merged_ranges.push_back(range);
        } else {
            merged_ranges.back().second = std::max(merged_ranges.back().second, range.second);
        }
    }

    int32_t gap_start = 0;
    for (const auto& range : merged_ranges) {
        if (gap_start < range.first) {
            track.notes.push_back(FFTSmdLaneNoteBlock {
                .start_tick = gap_start,
                .authored_start_tick = gap_start,
                .authored_span_index = -1,
                .duration_ticks = range.first - gap_start,
                .relative_key = 13,
                .has_fermata = false,
                .fermata_extension_ticks = 0,
                .source_event_index = -1,
            });
        }
        gap_start = std::max(gap_start, range.second);
    }
    if (gap_start < poly_track.total_ticks) {
        track.notes.push_back(FFTSmdLaneNoteBlock {
            .start_tick = gap_start,
            .authored_start_tick = gap_start,
            .authored_span_index = -1,
            .duration_ticks = poly_track.total_ticks - gap_start,
            .relative_key = 13,
            .has_fermata = false,
            .fermata_extension_ticks = 0,
            .source_event_index = -1,
        });
    }
    std::stable_sort(
        track.notes.begin(),
        track.notes.end(),
        [](const FFTSmdLaneNoteBlock& lhs, const FFTSmdLaneNoteBlock& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            return lhs.relative_key < rhs.relative_key;
        });

    std::vector<size_t> opcode_order(poly_track.opcodes.size());
    std::iota(opcode_order.begin(), opcode_order.end(), 0U);
    std::stable_sort(
        opcode_order.begin(),
        opcode_order.end(),
        [&poly_track](size_t lhs, size_t rhs) {
            if (poly_track.opcodes[lhs].tick != poly_track.opcodes[rhs].tick) {
                return poly_track.opcodes[lhs].tick < poly_track.opcodes[rhs].tick;
            }
            return poly_track.opcodes[lhs].stack_order < poly_track.opcodes[rhs].stack_order;
        });
    for (const size_t authored_index : opcode_order) {
        const auto& opcode = poly_track.opcodes[authored_index];
        track.markers.push_back(FFTSmdLaneMarker {
            .tick = opcode.tick,
            .authored_tick = opcode.tick,
            .kind = smd_is_tempo_opcode(opcode.opcode.opcode)
                ? FFTSmdLaneMarkerKind::tempo
                : (smd_is_structure_opcode(opcode.opcode.opcode)
                    ? FFTSmdLaneMarkerKind::structure
                    : FFTSmdLaneMarkerKind::opcode),
            .label = smd_short_opcode_label(opcode.opcode),
        });
        command_entries.push_back(PolyCommandEntry {
            .is_opcode = true,
            .tick = opcode.tick,
            .secondary_order = opcode.stack_order,
            .authored_index = static_cast<int32_t>(authored_index),
        });
    }

    std::stable_sort(
        command_entries.begin(),
        command_entries.end(),
        [](const PolyCommandEntry& lhs, const PolyCommandEntry& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            if (lhs.is_opcode != rhs.is_opcode) {
                return lhs.is_opcode && !rhs.is_opcode;
            }
            return lhs.secondary_order < rhs.secondary_order;
        });

    for (size_t sequence_index = 0; sequence_index < command_entries.size(); ++sequence_index) {
        const auto& entry = command_entries[sequence_index];
        if (entry.is_opcode) {
            const auto& opcode = poly_track.opcodes[static_cast<size_t>(entry.authored_index)];
            FFTSmdLaneCommandBlock command = smd_build_opcode_command(
                opcode.tick,
                static_cast<int32_t>(sequence_index),
                entry.authored_index,
                entry.authored_index,
                opcode.opcode,
                opcode.enabled);
            command.authored_tick = opcode.tick;
            track.commands.push_back(std::move(command));
            track.insert_anchors.push_back(build_insert_anchor_local(
                opcode.tick,
                static_cast<int32_t>(sequence_index),
                entry.authored_index,
                entry.authored_index,
                -1,
                opcode.opcode.opcode,
                FFTSmdLaneInsertAnchorKind::command,
                "Before " + smd_short_opcode_label(opcode.opcode)));
            continue;
        }

        const auto& note = poly_track.notes[static_cast<size_t>(entry.authored_index)];
        FFTSmdNoteEvent note_event {
            .velocity = 100,
            .relative_key = note.relative_key,
            .delta_time = std::max(note.total_ticks, 1),
        };
        FFTSmdLaneCommandBlock command = smd_build_note_command(
            note.start_tick,
            static_cast<int32_t>(sequence_index),
            entry.authored_index,
            note_event);
        command.authored_tick = note.start_tick;
        track.commands.push_back(std::move(command));
        track.insert_anchors.push_back(build_insert_anchor_local(
            note.start_tick,
            static_cast<int32_t>(sequence_index),
            entry.authored_index,
            -1,
            entry.authored_index,
            -1,
            FFTSmdLaneInsertAnchorKind::note_start,
            "Note " + smd_note_name_for_relative_key(note.relative_key)));
    }

    append_measure_insert_anchors_local(track, grid_segments);
    return track;
}

FFTSmdSongPresentation build_authored_source_presentation(
    const FFTSmdAuthoringDocument& authored_document,
    std::function<bool(int32_t, int32_t)> is_source_event_disabled
) {
    const FFTSmdCompiledDocument compiled = compile_smd_authoring_document(authored_document);
    FFTSmdSongPresentation compiled_source = build_smd_song_presentation(
        compiled.smd,
        FFTSmdPresentationMode::source,
        std::move(is_source_event_disabled));

    FFTSmdSongPresentation presentation;
    presentation.total_ticks = compiled_source.total_ticks;
    presentation.conductor_markers = compiled_source.conductor_markers;
    presentation.grid_segments = compiled_source.grid_segments;
    presentation.second_markers = compiled_source.second_markers;
    presentation.tracks.reserve(authored_document.parts.size());

    const auto diagnostics = validate_smd_authoring_document(authored_document);
    for (size_t part_index = 0; part_index < authored_document.parts.size(); ++part_index) {
        const auto& part = authored_document.parts[part_index];
        FFTSmdTrackLanePresentation track_presentation;
        if (part.kind == FFTSmdAuthoringPartKind::raw_track &&
            part_index < compiled.authored_part_compiled_track_indices.size() &&
            !compiled.authored_part_compiled_track_indices[part_index].empty()) {
            const int32_t compiled_track_index = compiled.authored_part_compiled_track_indices[part_index].front();
            if (compiled_track_index >= 0 &&
                static_cast<size_t>(compiled_track_index) < compiled_source.tracks.size()) {
                track_presentation = compiled_source.tracks[static_cast<size_t>(compiled_track_index)];
                annotate_raw_source_track_presentation(track_presentation, part.raw_track, compiled, part_index);
            }
            track_presentation.track_index = static_cast<int32_t>(part_index);
            track_presentation.summary.track_index = static_cast<int32_t>(part_index);
        } else {
            track_presentation = build_poly_source_track_presentation(
                part.poly_track,
                static_cast<int32_t>(part_index),
                presentation.grid_segments);
        }
        if (part_index < diagnostics.size()) {
            track_presentation.diagnostics = diagnostics[part_index];
        }
        presentation.total_ticks = std::max(presentation.total_ticks, track_presentation.total_ticks);
        presentation.tracks.push_back(std::move(track_presentation));
    }
    return presentation;
}

const FFTSmdTrackLanePresentation* find_track_presentation(
    const FFTSmdSongPresentation& presentation,
    int32_t track_index
) {
    const auto it = std::find_if(
        presentation.tracks.begin(),
        presentation.tracks.end(),
        [track_index](const FFTSmdTrackLanePresentation& track) {
            return track.track_index == track_index;
        });
    return it == presentation.tracks.end() ? nullptr : &(*it);
}

std::unordered_map<int32_t, int32_t> compiled_tick_by_source_index(
    const FFTSmdTrackLanePresentation* compiled_source_track
) {
    std::unordered_map<int32_t, int32_t> ticks;
    if (compiled_source_track == nullptr) {
        return ticks;
    }
    for (const auto& command : compiled_source_track->commands) {
        if (!is_non_time_opcode_code(command.opcode) || command.source_event_index < 0) {
            continue;
        }
        ticks[command.source_event_index] = command.tick;
    }
    return ticks;
}

std::unordered_map<int32_t, int32_t> authored_opcode_index_by_source_index(
    const FFTSmdCompiledDocument& compiled,
    size_t part_index
) {
    std::unordered_map<int32_t, int32_t> authored_indices;
    if (part_index >= compiled.authored_opcode_source_indices.size()) {
        return authored_indices;
    }
    const auto& source_indices = compiled.authored_opcode_source_indices[part_index];
    for (size_t authored_index = 0; authored_index < source_indices.size(); ++authored_index) {
        if (source_indices[authored_index] >= 0) {
            authored_indices[source_indices[authored_index]] = static_cast<int32_t>(authored_index);
        }
    }
    return authored_indices;
}

std::unordered_map<uint64_t, int32_t> authored_opcode_index_by_source_key(
    const FFTSmdCompiledDocument& compiled,
    size_t part_index
) {
    std::unordered_map<uint64_t, int32_t> indices;
    if (part_index >= compiled.authored_opcode_source_keys.size()) {
        return indices;
    }
    const auto& source_keys = compiled.authored_opcode_source_keys[part_index];
    indices.reserve(source_keys.size());
    for (size_t authored_index = 0; authored_index < source_keys.size(); ++authored_index) {
        if (source_keys[authored_index] == 0) {
            continue;
        }
        indices[source_keys[authored_index]] = static_cast<int32_t>(authored_index);
    }
    return indices;
}

std::unordered_map<int32_t, int32_t> authored_span_index_by_source_index(
    const FFTSmdCompiledDocument& compiled,
    size_t part_index
) {
    std::unordered_map<int32_t, int32_t> authored_indices;
    if (part_index >= compiled.authored_span_source_indices.size()) {
        return authored_indices;
    }
    const auto& source_indices = compiled.authored_span_source_indices[part_index];
    for (size_t authored_index = 0; authored_index < source_indices.size(); ++authored_index) {
        if (source_indices[authored_index] >= 0) {
            authored_indices[source_indices[authored_index]] = static_cast<int32_t>(authored_index);
        }
    }
    return authored_indices;
}

std::unordered_map<uint64_t, int32_t> authored_span_index_by_source_key(
    const FFTSmdCompiledDocument& compiled,
    size_t part_index
) {
    std::unordered_map<uint64_t, int32_t> indices;
    if (part_index >= compiled.authored_span_source_keys.size()) {
        return indices;
    }
    const auto& source_keys = compiled.authored_span_source_keys[part_index];
    indices.reserve(source_keys.size());
    for (size_t authored_index = 0; authored_index < source_keys.size(); ++authored_index) {
        if (source_keys[authored_index] == 0) {
            continue;
        }
        indices[source_keys[authored_index]] = static_cast<int32_t>(authored_index);
    }
    return indices;
}

void append_measure_insert_anchors_for_playback(
    FFTSmdTrackLanePresentation& track_presentation,
    const std::vector<FFTSmdGridSegment>& grid_segments
) {
    int32_t bar_number = 1;
    for (const auto& segment : grid_segments) {
        for (int32_t bar_tick = segment.start_tick; bar_tick < segment.end_tick; bar_tick += segment.ticks_per_bar) {
            if (bar_tick <= 0) {
                bar_number += 1;
                continue;
            }
            track_presentation.insert_anchors.push_back(build_insert_anchor_local(
                bar_tick,
                static_cast<int32_t>(track_presentation.commands.size()),
                -1,
                -1,
                -1,
                -1,
                FFTSmdLaneInsertAnchorKind::measure,
                "Bar " + std::to_string(bar_number)));
            bar_number += 1;
        }
    }
}

void append_poly_playback_rests(
    FFTSmdTrackLanePresentation& track_presentation,
    int32_t total_ticks
) {
    std::vector<int32_t> cut_ticks;
    cut_ticks.reserve(track_presentation.loop_boundaries.size());
    for (const auto& boundary : track_presentation.loop_boundaries) {
        if (boundary.tick > 0 && boundary.tick < total_ticks) {
            cut_ticks.push_back(boundary.tick);
        }
    }
    std::stable_sort(cut_ticks.begin(), cut_ticks.end());
    cut_ticks.erase(std::unique(cut_ticks.begin(), cut_ticks.end()), cut_ticks.end());

    auto append_rest_segments = [&](std::vector<FFTSmdLaneNoteBlock>& rests, int32_t start_tick, int32_t end_tick) {
        if (end_tick <= start_tick) {
            return;
        }
        int32_t segment_start = start_tick;
        for (const int32_t cut_tick : cut_ticks) {
            if (cut_tick <= segment_start) {
                continue;
            }
            if (cut_tick >= end_tick) {
                break;
            }
            rests.push_back(FFTSmdLaneNoteBlock {
                .start_tick = segment_start,
                .authored_start_tick = segment_start,
                .authored_span_index = -1,
                .duration_ticks = cut_tick - segment_start,
                .relative_key = 13,
                .has_fermata = false,
                .fermata_extension_ticks = 0,
                .source_event_index = -1,
            });
            segment_start = cut_tick;
        }
        rests.push_back(FFTSmdLaneNoteBlock {
            .start_tick = segment_start,
            .authored_start_tick = segment_start,
            .authored_span_index = -1,
            .duration_ticks = end_tick - segment_start,
            .relative_key = 13,
            .has_fermata = false,
            .fermata_extension_ticks = 0,
            .source_event_index = -1,
        });
    };

    std::vector<std::pair<int32_t, int32_t>> active_ranges;
    for (const auto& note : track_presentation.notes) {
        if (note.relative_key < 0 || note.relative_key >= 12) {
            continue;
        }
        const int32_t end_tick = note.start_tick + std::max(1, note.duration_ticks);
        if (end_tick <= note.start_tick) {
            continue;
        }
        active_ranges.emplace_back(note.start_tick, end_tick);
    }

    std::stable_sort(
        active_ranges.begin(),
        active_ranges.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
            }
            return lhs.second < rhs.second;
        });

    std::vector<std::pair<int32_t, int32_t>> merged_ranges;
    for (const auto& range : active_ranges) {
        if (merged_ranges.empty() || range.first > merged_ranges.back().second) {
            merged_ranges.push_back(range);
        } else {
            merged_ranges.back().second = std::max(merged_ranges.back().second, range.second);
        }
    }

    std::vector<FFTSmdLaneNoteBlock> rests;
    int32_t gap_start = 0;
    for (const auto& range : merged_ranges) {
        if (gap_start < range.first) {
            append_rest_segments(rests, gap_start, range.first);
        }
        gap_start = std::max(gap_start, range.second);
    }
    if (gap_start < total_ticks) {
        append_rest_segments(rests, gap_start, total_ticks);
    }

    track_presentation.notes.insert(
        track_presentation.notes.end(),
        rests.begin(),
        rests.end());
}

void remap_poly_derived_rests_to_authored_ticks(FFTSmdTrackLanePresentation& track_presentation) {
    for (auto& note : track_presentation.notes) {
        if (note.relative_key != 13 || note.source_event_index >= 0) {
            continue;
        }
        note.authored_start_tick = smd_map_visible_tick_to_authored_tick(track_presentation, note.start_tick);
    }
}

void append_poly_playback_note_anchors(FFTSmdTrackLanePresentation& track_presentation) {
    for (const auto& note : track_presentation.notes) {
        track_presentation.insert_anchors.push_back(build_insert_anchor_local(
            note.start_tick,
            0,
            note.source_event_index,
            -1,
            note.authored_span_index,
            -1,
            note.relative_key == 13
                ? FFTSmdLaneInsertAnchorKind::rest_start
                : FFTSmdLaneInsertAnchorKind::note_start,
            note.relative_key == 13
                ? ("Rest " + std::to_string(std::max(1, note.duration_ticks)))
                : ("Note " + smd_note_name_for_relative_key(note.relative_key))));
    }
}

void annotate_time_map_segment_instances(FFTSmdTrackLanePresentation& track_presentation) {
    std::unordered_map<int32_t, std::vector<size_t>> grouped_segments;
    grouped_segments.reserve(track_presentation.time_map_segments.size());
    int32_t next_synthetic_group_id = 1;
    for (size_t index = 0; index < track_presentation.time_map_segments.size(); ++index) {
        auto& segment = track_presentation.time_map_segments[index];
        if (segment.authored_start_tick < 0 || segment.duration_ticks <= 0) {
            continue;
        }
        if (segment.root_group_id < 0) {
            segment.root_group_id = 0x40000000 + next_synthetic_group_id;
            segment.loop_instance_id = segment.root_group_id;
            next_synthetic_group_id += 1;
        } else if (segment.loop_instance_id < 0) {
            segment.loop_instance_id = segment.root_group_id;
        }
        grouped_segments[segment.root_group_id].push_back(index);
    }

    for (auto& [group_id, indices] : grouped_segments) {
        (void)group_id;
        std::unordered_map<int32_t, int32_t> instance_order;
        std::vector<int32_t> ordered_instances;
        std::stable_sort(
            indices.begin(),
            indices.end(),
            [&track_presentation](size_t lhs, size_t rhs) {
                const auto& a = track_presentation.time_map_segments[lhs];
                const auto& b = track_presentation.time_map_segments[rhs];
                if (a.start_tick != b.start_tick) {
                    return a.start_tick < b.start_tick;
                }
                if (a.loop_instance_id != b.loop_instance_id) {
                    return a.loop_instance_id < b.loop_instance_id;
                }
                return a.authored_start_tick < b.authored_start_tick;
            });
        for (const size_t index : indices) {
            const int32_t instance_id = track_presentation.time_map_segments[index].loop_instance_id;
            if (instance_order.contains(instance_id)) {
                continue;
            }
            instance_order[instance_id] = static_cast<int32_t>(ordered_instances.size());
            ordered_instances.push_back(instance_id);
        }
        const int32_t occurrence_count = static_cast<int32_t>(ordered_instances.size());
        for (const size_t index : indices) {
            auto& segment = track_presentation.time_map_segments[index];
            segment.occurrence_index = instance_order[segment.loop_instance_id];
            segment.occurrence_count = occurrence_count;
            segment.repeated = occurrence_count > 1;
        }
    }
}

template <typename BoundaryContainer>
void append_split_time_map_segments_from_track_notes(
    std::vector<FFTSmdLaneTimeMapSegment>& out_segments,
    const FFTSmdTrackLanePresentation& source_track,
    const BoundaryContainer& loop_boundaries
) {
    std::vector<FFTSmdLaneLoopBoundary> boundaries;
    boundaries.reserve(loop_boundaries.size());
    for (const auto& boundary : loop_boundaries) {
        if (boundary.tick <= 0 || boundary.tick >= source_track.total_ticks) {
            continue;
        }
        boundaries.push_back(boundary);
    }
    std::stable_sort(
        boundaries.begin(),
        boundaries.end(),
        [](const FFTSmdLaneLoopBoundary& lhs, const FFTSmdLaneLoopBoundary& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.authored_tick < rhs.authored_tick;
        });

    for (const auto& note : source_track.notes) {
        if (note.authored_start_tick < 0 || note.duration_ticks <= 0) {
            continue;
        }

        int32_t segment_start = note.start_tick;
        int32_t authored_start = note.authored_start_tick;
        const int32_t note_end = note.start_tick + note.duration_ticks;
        for (const auto& boundary : boundaries) {
            if (boundary.tick <= segment_start) {
                continue;
            }
            if (boundary.tick >= note_end) {
                break;
            }
            const int32_t segment_duration = boundary.tick - segment_start;
            out_segments.push_back(FFTSmdLaneTimeMapSegment {
                .start_tick = segment_start,
                .authored_start_tick = authored_start,
                .duration_ticks = segment_duration,
                .root_group_id = note.loop_root_id,
                .loop_instance_id = note.loop_instance_id,
            });
            segment_start = boundary.tick;
            if (boundary.authored_tick >= 0) {
                authored_start = boundary.authored_tick;
            } else {
                authored_start += segment_duration;
            }
        }

        if (note_end > segment_start) {
            out_segments.push_back(FFTSmdLaneTimeMapSegment {
                .start_tick = segment_start,
                .authored_start_tick = authored_start,
                .duration_ticks = note_end - segment_start,
                .root_group_id = note.loop_root_id,
                .loop_instance_id = note.loop_instance_id,
            });
        }
    }
}

void append_time_map_segments_from_notes(FFTSmdTrackLanePresentation& track_presentation) {
    track_presentation.time_map_segments.clear();
    track_presentation.time_map_segments.reserve(track_presentation.notes.size() * 2U);
    append_split_time_map_segments_from_track_notes(
        track_presentation.time_map_segments,
        track_presentation,
        track_presentation.loop_boundaries);
    annotate_time_map_segment_instances(track_presentation);
}

void append_time_map_segments_from_authoritative_playback_track(
    FFTSmdTrackLanePresentation& track_presentation,
    const FFTSmdTrackLanePresentation& authoritative_track
) {
    track_presentation.time_map_segments.clear();
    track_presentation.time_map_segments.reserve(authoritative_track.notes.size() * 2U);
    append_split_time_map_segments_from_track_notes(
        track_presentation.time_map_segments,
        authoritative_track,
        authoritative_track.loop_boundaries);
    annotate_time_map_segment_instances(track_presentation);
}

void append_time_map_segments_from_playback_tracks(
    FFTSmdTrackLanePresentation& track_presentation,
    const std::vector<const FFTSmdTrackLanePresentation*>& playback_tracks
) {
    track_presentation.time_map_segments.clear();
    size_t reserve_count = 0;
    for (const auto* playback_track : playback_tracks) {
        if (playback_track == nullptr) {
            continue;
        }
        reserve_count += playback_track->notes.size() * 2U;
    }
    track_presentation.time_map_segments.reserve(reserve_count);
    for (const auto* playback_track : playback_tracks) {
        if (playback_track == nullptr) {
            continue;
        }
        append_split_time_map_segments_from_track_notes(
            track_presentation.time_map_segments,
            *playback_track,
            playback_track->loop_boundaries);
    }
    annotate_time_map_segment_instances(track_presentation);
}

FFTSmdTrackLanePresentation build_authored_playback_raw_track_presentation(
    const FFTSmdAuthoringDocument& authored_document,
    const FFTSmdCompiledDocument& compiled,
    const FFTSmdSongPresentation& playback_presentation,
    const FFTSmdSongPresentation& compiled_source_presentation,
    size_t part_index
) {
    FFTSmdTrackLanePresentation track_presentation;
    track_presentation.track_index = static_cast<int32_t>(part_index);
    track_presentation.summary.track_index = static_cast<int32_t>(part_index);

    if (part_index >= authored_document.parts.size() ||
        part_index >= compiled.authored_part_compiled_track_indices.size() ||
        compiled.authored_part_compiled_track_indices[part_index].empty()) {
        return track_presentation;
    }

    const auto& authored_track = authored_document.parts[part_index].raw_track;
    const int32_t compiled_track_index = compiled.authored_part_compiled_track_indices[part_index].front();
    const auto* playback_track = find_track_presentation(playback_presentation, compiled_track_index);
    if (playback_track == nullptr) {
        track_presentation.total_ticks = authored_track.total_ticks;
        return track_presentation;
    }

    track_presentation = *playback_track;
    track_presentation.track_index = static_cast<int32_t>(part_index);
    track_presentation.summary.track_index = static_cast<int32_t>(part_index);

    const auto authored_opcode_indices = authored_opcode_index_by_source_index(compiled, part_index);
    const auto authored_span_indices = authored_span_index_by_source_index(compiled, part_index);
    const auto compiled_ticks = compiled_tick_by_source_index(
        find_track_presentation(compiled_source_presentation, compiled_track_index));

    for (auto& command : track_presentation.commands) {
        if (!is_non_time_opcode_code(command.opcode) || command.source_event_index < 0) {
            continue;
        }
        const auto authored_it = authored_opcode_indices.find(command.source_event_index);
        if (authored_it == authored_opcode_indices.end() ||
            static_cast<size_t>(authored_it->second) >= authored_track.opcodes.size()) {
            continue;
        }
        const int32_t authored_index = authored_it->second;
        const int32_t authored_tick = authored_track.opcodes[static_cast<size_t>(authored_index)].tick;
        command.authored_opcode_index = authored_index;
        command.authored_tick = authored_tick;
        const auto compiled_tick_it = compiled_ticks.find(command.source_event_index);
        if (compiled_tick_it != compiled_ticks.end()) {
            command.tick = std::max(0, command.tick + (authored_tick - compiled_tick_it->second));
        }
    }

    for (auto& note : track_presentation.notes) {
        if (note.source_event_index < 0) {
            continue;
        }
        const auto authored_it = authored_span_indices.find(note.source_event_index);
        if (authored_it == authored_span_indices.end() ||
            static_cast<size_t>(authored_it->second) >= authored_track.spans.size()) {
            continue;
        }
        const int32_t authored_index = authored_it->second;
        note.authored_span_index = authored_index;
        note.authored_start_tick = authored_track.spans[static_cast<size_t>(authored_index)].start_tick;
        note.source_event_index = authored_index;
    }

    for (auto& anchor : track_presentation.insert_anchors) {
        if (anchor.kind == FFTSmdLaneInsertAnchorKind::command && anchor.source_event_index >= 0) {
            const int32_t compiled_source_index = anchor.source_event_index;
            const auto authored_it = authored_opcode_indices.find(compiled_source_index);
            if (authored_it != authored_opcode_indices.end() &&
                static_cast<size_t>(authored_it->second) < authored_track.opcodes.size()) {
                const int32_t authored_index = authored_it->second;
                anchor.authored_opcode_index = authored_index;
                anchor.tick = authored_track.opcodes[static_cast<size_t>(authored_index)].tick;
            }
            continue;
        }
        if ((anchor.kind == FFTSmdLaneInsertAnchorKind::note_start ||
             anchor.kind == FFTSmdLaneInsertAnchorKind::rest_start) &&
            anchor.source_event_index >= 0) {
            const int32_t compiled_source_index = anchor.source_event_index;
            const auto authored_it = authored_span_indices.find(compiled_source_index);
            if (authored_it != authored_span_indices.end() &&
                static_cast<size_t>(authored_it->second) < authored_track.spans.size()) {
                const int32_t authored_index = authored_it->second;
                anchor.authored_span_index = authored_index;
                anchor.source_event_index = authored_index;
                anchor.tick = authored_track.spans[static_cast<size_t>(authored_index)].start_tick;
            }
        }
    }

    append_time_map_segments_from_notes(track_presentation);

    return track_presentation;
}

FFTSmdTrackLanePresentation build_authored_playback_poly_track_presentation(
    const FFTSmdAuthoringDocument& authored_document,
    const FFTSmdCompiledDocument& compiled,
    const FFTSmdSongPresentation& playback_presentation,
    const FFTSmdSongPresentation& compiled_source_presentation,
    size_t part_index,
    const std::vector<FFTSmdGridSegment>& grid_segments
) {
    FFTSmdTrackLanePresentation track_presentation;
    track_presentation.track_index = static_cast<int32_t>(part_index);
    track_presentation.summary.track_index = static_cast<int32_t>(part_index);
    track_presentation.polyphonic_notes = true;

    if (part_index >= authored_document.parts.size() ||
        part_index >= compiled.authored_part_compiled_track_indices.size()) {
        return track_presentation;
    }

    const auto& poly_track = authored_document.parts[part_index].poly_track;
    const auto& compiled_track_indices = compiled.authored_part_compiled_track_indices[part_index];
    if (compiled_track_indices.empty()) {
        track_presentation.total_ticks = poly_track.total_ticks;
        return track_presentation;
    }

    const auto* first_playback_track = find_track_presentation(playback_presentation, compiled_track_indices.front());
    const auto* first_compiled_source_track =
        find_track_presentation(compiled_source_presentation, compiled_track_indices.front());
    if (first_playback_track != nullptr) {
        track_presentation.total_ticks = first_playback_track->total_ticks;
        track_presentation.markers = first_playback_track->markers;
        track_presentation.loop_boundaries = first_playback_track->loop_boundaries;
        track_presentation.summary = first_playback_track->summary;
        track_presentation.summary.track_index = static_cast<int32_t>(part_index);
    } else {
        track_presentation.total_ticks = poly_track.total_ticks;
    }

    const auto authored_opcode_indices = authored_opcode_index_by_source_key(compiled, part_index);
    const auto authored_span_indices = authored_span_index_by_source_key(compiled, part_index);
    const auto compiled_ticks = compiled_tick_by_source_index(first_compiled_source_track);
    std::vector<const FFTSmdTrackLanePresentation*> playback_tracks_for_time_map;
    playback_tracks_for_time_map.reserve(compiled_track_indices.size());

    if (first_playback_track != nullptr) {
        track_presentation.insert_anchors.push_back(build_insert_anchor_local(
            0,
            0,
            -1,
            -1,
            -1,
            -1,
            FFTSmdLaneInsertAnchorKind::track_start,
            "Track Start"));

        for (const auto& command : first_playback_track->commands) {
            if (!is_non_time_opcode_code(command.opcode) || command.source_event_index < 0) {
                continue;
            }
            const uint64_t source_key =
                smd_track_event_key(compiled_track_indices.front(), command.source_event_index);
            const auto authored_it = authored_opcode_indices.find(source_key);
            if (authored_it == authored_opcode_indices.end() ||
                static_cast<size_t>(authored_it->second) >= poly_track.opcodes.size()) {
                continue;
            }
            FFTSmdLaneCommandBlock rewritten = command;
            const int32_t authored_index = authored_it->second;
            const int32_t authored_tick = poly_track.opcodes[static_cast<size_t>(authored_index)].tick;
            rewritten.authored_opcode_index = authored_index;
            rewritten.authored_tick = authored_tick;
            rewritten.source_event_index = authored_index;
            const auto compiled_tick_it = compiled_ticks.find(command.source_event_index);
            if (compiled_tick_it != compiled_ticks.end()) {
                rewritten.tick = std::max(0, rewritten.tick + (authored_tick - compiled_tick_it->second));
            }
            track_presentation.commands.push_back(rewritten);
            track_presentation.insert_anchors.push_back(build_insert_anchor_local(
                rewritten.tick,
                rewritten.sequence_index,
                rewritten.source_event_index,
                rewritten.authored_opcode_index,
                -1,
                rewritten.opcode,
                FFTSmdLaneInsertAnchorKind::command,
                "Before " + rewritten.label));
        }
    }

    for (const int32_t compiled_track_index : compiled_track_indices) {
        const auto* playback_track = find_track_presentation(playback_presentation, compiled_track_index);
        if (playback_track == nullptr) {
            continue;
        }
        playback_tracks_for_time_map.push_back(playback_track);
        track_presentation.total_ticks = std::max(track_presentation.total_ticks, playback_track->total_ticks);
        for (const auto& note : playback_track->notes) {
            if (note.relative_key < 0 || note.relative_key >= 12 || note.source_event_index < 0) {
                continue;
            }
            const uint64_t source_key = smd_track_event_key(compiled_track_index, note.source_event_index);
            const auto authored_it = authored_span_indices.find(source_key);
            if (authored_it == authored_span_indices.end() ||
                static_cast<size_t>(authored_it->second) >= poly_track.notes.size()) {
                continue;
            }
            const int32_t authored_index = authored_it->second;
            FFTSmdLaneNoteBlock rewritten = note;
            rewritten.authored_span_index = authored_index;
            rewritten.authored_start_tick = poly_track.notes[static_cast<size_t>(authored_index)].start_tick;
            rewritten.source_event_index = authored_index;
            track_presentation.notes.push_back(std::move(rewritten));
        }
    }

    append_time_map_segments_from_playback_tracks(track_presentation, playback_tracks_for_time_map);

    append_poly_playback_rests(track_presentation, track_presentation.total_ticks);
    remap_poly_derived_rests_to_authored_ticks(track_presentation);
    std::stable_sort(
        track_presentation.notes.begin(),
        track_presentation.notes.end(),
        [](const FFTSmdLaneNoteBlock& lhs, const FFTSmdLaneNoteBlock& rhs) {
            if (lhs.start_tick != rhs.start_tick) {
                return lhs.start_tick < rhs.start_tick;
            }
            if (lhs.relative_key != rhs.relative_key) {
                return lhs.relative_key < rhs.relative_key;
            }
            return lhs.duration_ticks < rhs.duration_ticks;
        });
    append_poly_playback_note_anchors(track_presentation);
    append_measure_insert_anchors_for_playback(track_presentation, grid_segments);

    std::ostringstream dbg;
    dbg << "PRES track=" << part_index
        << " total=" << track_presentation.total_ticks
        << " visible_notes=" << track_presentation.notes.size()
        << " visible_cmds=" << track_presentation.commands.size()
        << " loops=" << track_presentation.loop_boundaries.size()
        << " segs=" << track_presentation.time_map_segments.size()
        << " authored_notes=" << poly_track.notes.size()
        << " compiled_lanes=" << compiled_track_indices.size();
    int shown = 0;
    for (size_t note_index = 0; note_index < poly_track.notes.size() && shown < 8; ++note_index) {
        const auto& note = poly_track.notes[note_index];
        dbg << " | a" << shown
            << "#" << note_index
            << "=" << note.start_tick
            << "+" << note.total_ticks
            << "/" << note.base_ticks
            << " k" << note.relative_key;
        shown += 1;
    }
    shown = 0;
    for (const auto& note : track_presentation.notes) {
        if (note.relative_key < 0 || note.relative_key >= 12) {
            continue;
        }
        dbg << " | v" << shown
            << "=" << note.start_tick
            << "->" << note.authored_start_tick
            << " k" << note.relative_key;
        shown += 1;
        if (shown >= 8) {
            break;
        }
    }
    shown = 0;
    for (const auto& segment : track_presentation.time_map_segments) {
        dbg << " | s" << shown
            << "=" << segment.start_tick
            << "->" << segment.authored_start_tick
            << "+" << segment.duration_ticks
            << " g" << segment.root_group_id
            << " i" << segment.loop_instance_id
            << " o" << segment.occurrence_index
            << "/" << segment.occurrence_count;
        shown += 1;
        if (shown >= 8) {
            break;
        }
    }
    append_unwound_insert_debug_line(dbg.str());

    return track_presentation;
}

FFTSmdSongPresentation build_authored_playback_presentation(
    const FFTSmdAuthoringDocument& authored_document,
    const FFTSmdSongPresentation& playback_presentation
) {
    const FFTSmdCompiledDocument compiled = compile_smd_authoring_document(authored_document);
    const FFTSmdSongPresentation compiled_source_presentation =
        build_smd_song_presentation(compiled.smd, FFTSmdPresentationMode::source);

    FFTSmdSongPresentation presentation;
    presentation.total_ticks = playback_presentation.total_ticks;
    presentation.conductor_markers = playback_presentation.conductor_markers;
    presentation.grid_segments = playback_presentation.grid_segments;
    presentation.second_markers = playback_presentation.second_markers;
    presentation.tracks.reserve(authored_document.parts.size());

    const auto diagnostics = validate_smd_authoring_document(authored_document);
    for (size_t part_index = 0; part_index < authored_document.parts.size(); ++part_index) {
        FFTSmdTrackLanePresentation track_presentation =
            authored_document.parts[part_index].kind == FFTSmdAuthoringPartKind::poly_track
            ? build_authored_playback_poly_track_presentation(
                authored_document,
                compiled,
                playback_presentation,
                compiled_source_presentation,
                part_index,
                presentation.grid_segments)
            : build_authored_playback_raw_track_presentation(
                authored_document,
                compiled,
                playback_presentation,
                compiled_source_presentation,
                part_index);
        if (part_index < diagnostics.size()) {
            track_presentation.diagnostics = diagnostics[part_index];
        }
        presentation.total_ticks = std::max(presentation.total_ticks, track_presentation.total_ticks);
        presentation.tracks.push_back(std::move(track_presentation));
    }
    return presentation;
}

void annotate_source_presentation_from_authored_document(
    FFTSmdSongPresentation& presentation,
    const FFTSmdAuthoringDocument& authored_document
) {
    const FFTSmdCompiledDocument compiled = compile_smd_authoring_document(authored_document);
    std::vector<std::unordered_map<int32_t, int32_t>> authored_opcode_index_by_source(authored_document.tracks.size());
    std::vector<std::unordered_map<int32_t, int32_t>> authored_span_index_by_source(authored_document.tracks.size());

    for (size_t track_index = 0; track_index < authored_document.tracks.size(); ++track_index) {
        if (track_index < compiled.authored_opcode_source_indices.size()) {
            const auto& source_indices = compiled.authored_opcode_source_indices[track_index];
            for (size_t authored_index = 0; authored_index < source_indices.size(); ++authored_index) {
                authored_opcode_index_by_source[track_index][source_indices[authored_index]] =
                    static_cast<int32_t>(authored_index);
            }
        }
        if (track_index < compiled.authored_span_source_indices.size()) {
            const auto& source_indices = compiled.authored_span_source_indices[track_index];
            for (size_t authored_index = 0; authored_index < source_indices.size(); ++authored_index) {
                authored_span_index_by_source[track_index][source_indices[authored_index]] =
                    static_cast<int32_t>(authored_index);
            }
        }
    }

    const size_t track_count = std::min(presentation.tracks.size(), authored_document.tracks.size());
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        auto& track = presentation.tracks[track_index];
        const auto& authored_track = authored_document.tracks[track_index];
        for (auto& command : track.commands) {
            if (command.source_event_index >= 0) {
                const auto opcode_it = authored_opcode_index_by_source[track_index].find(command.source_event_index);
                if (opcode_it != authored_opcode_index_by_source[track_index].end()) {
                    command.authored_opcode_index = opcode_it->second;
                    if (static_cast<size_t>(opcode_it->second) < authored_track.opcodes.size()) {
                        command.authored_tick = authored_track.opcodes[static_cast<size_t>(opcode_it->second)].tick;
                    }
                }
            }
        }
        for (auto& note : track.notes) {
            if (note.source_event_index >= 0) {
                const auto span_it = authored_span_index_by_source[track_index].find(note.source_event_index);
                if (span_it != authored_span_index_by_source[track_index].end()) {
                    note.authored_span_index = span_it->second;
                    if (static_cast<size_t>(span_it->second) < authored_track.spans.size()) {
                        note.authored_start_tick = authored_track.spans[static_cast<size_t>(span_it->second)].start_tick;
                    }
                }
            }
        }
        for (auto& anchor : track.insert_anchors) {
            if (anchor.source_event_index < 0) {
                continue;
            }
            if (anchor.kind == FFTSmdLaneInsertAnchorKind::command) {
                const auto opcode_it = authored_opcode_index_by_source[track_index].find(anchor.source_event_index);
                if (opcode_it != authored_opcode_index_by_source[track_index].end()) {
                    anchor.authored_opcode_index = opcode_it->second;
                }
            } else if (anchor.kind == FFTSmdLaneInsertAnchorKind::note_start ||
                       anchor.kind == FFTSmdLaneInsertAnchorKind::rest_start) {
                const auto span_it = authored_span_index_by_source[track_index].find(anchor.source_event_index);
                if (span_it != authored_span_index_by_source[track_index].end()) {
                    anchor.authored_span_index = span_it->second;
                }
            }
        }
    }
}

void remap_source_track_to_authored_ticks(
    FFTSmdTrackLanePresentation& presentation_track,
    const FFTSmdAuthoredTrack& authored_track
) {
    std::vector<FFTSmdAuthoredOpcode> authored_opcodes = authored_track.opcodes;
    std::stable_sort(
        authored_opcodes.begin(),
        authored_opcodes.end(),
        [](const FFTSmdAuthoredOpcode& lhs, const FFTSmdAuthoredOpcode& rhs) {
            if (lhs.tick != rhs.tick) {
                return lhs.tick < rhs.tick;
            }
            return lhs.stack_order < rhs.stack_order;
        });

    size_t opcode_index = 0;
    for (auto& command : presentation_track.commands) {
        if (!is_non_time_opcode_code(command.opcode)) {
            continue;
        }
        if (opcode_index >= authored_opcodes.size()) {
            break;
        }
        command.tick = authored_opcodes[opcode_index].tick;
        opcode_index += 1;
    }

    opcode_index = 0;
    for (auto& anchor : presentation_track.insert_anchors) {
        if (anchor.kind != FFTSmdLaneInsertAnchorKind::command) {
            continue;
        }
        if (opcode_index >= authored_opcodes.size()) {
            break;
        }
        anchor.tick = authored_opcodes[opcode_index].tick;
        opcode_index += 1;
    }

    opcode_index = 0;
    for (auto& marker : presentation_track.markers) {
        if (marker.kind != FFTSmdLaneMarkerKind::opcode &&
            marker.kind != FFTSmdLaneMarkerKind::structure &&
            marker.kind != FFTSmdLaneMarkerKind::tempo) {
            continue;
        }
        if (opcode_index >= authored_opcodes.size()) {
            break;
        }
        marker.tick = authored_opcodes[opcode_index].tick;
        opcode_index += 1;
    }
}

void remap_source_presentation_to_authored_ticks(
    FFTSmdSongPresentation& presentation,
    const FFTSmdAuthoringDocument& authored_document
) {
    const size_t track_count = std::min(presentation.tracks.size(), authored_document.tracks.size());
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        remap_source_track_to_authored_ticks(
            presentation.tracks[track_index],
            authored_document.tracks[track_index]);
    }

    if (!presentation.tracks.empty()) {
        presentation.conductor_markers = presentation.tracks.front().markers;
    }
}

void append_authored_diagnostics_to_presentation(
    FFTSmdSongPresentation& presentation,
    const FFTSmdAuthoringDocument& authored_document
) {
    const auto diagnostics = validate_smd_authoring_document(authored_document);
    const size_t track_count = std::min(presentation.tracks.size(), diagnostics.size());
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        presentation.tracks[track_index].diagnostics = diagnostics[track_index];
    }
}

void annotate_playback_presentation_from_authored_document(
    FFTSmdSongPresentation& presentation,
    const FFTSmdAuthoringDocument& authored_document
) {
    const FFTSmdCompiledDocument compiled = compile_smd_authoring_document(authored_document);
    const FFTSmdSongPresentation compiled_source_presentation =
        build_smd_song_presentation(compiled.smd, FFTSmdPresentationMode::source);

    std::vector<std::unordered_map<int32_t, int32_t>> authored_tick_by_source_index(authored_document.tracks.size());
    std::vector<std::unordered_map<int32_t, int32_t>> authored_opcode_index_by_source_index(authored_document.tracks.size());
    for (size_t track_index = 0; track_index < authored_document.tracks.size(); ++track_index) {
        if (track_index >= compiled.authored_opcode_source_indices.size()) {
            continue;
        }
        const auto& source_indices = compiled.authored_opcode_source_indices[track_index];
        const auto& authored_track = authored_document.tracks[track_index];
        for (size_t authored_index = 0; authored_index < source_indices.size() && authored_index < authored_track.opcodes.size();
             ++authored_index) {
            authored_tick_by_source_index[track_index][source_indices[authored_index]] =
                authored_track.opcodes[authored_index].tick;
            authored_opcode_index_by_source_index[track_index][source_indices[authored_index]] =
                static_cast<int32_t>(authored_index);
        }
    }

    for (auto& track : presentation.tracks) {
        if (track.track_index < 0 ||
            static_cast<size_t>(track.track_index) >= authored_document.tracks.size()) {
            continue;
        }

        const auto& authored_track = authored_document.tracks[static_cast<size_t>(track.track_index)];
        const auto compiled_track_it = std::find_if(
            compiled_source_presentation.tracks.begin(),
            compiled_source_presentation.tracks.end(),
            [&track](const FFTSmdTrackLanePresentation& compiled_track) {
                return compiled_track.track_index == track.track_index;
            });

        using OpcodeKey = std::pair<int32_t, int32_t>;
        struct OpcodeKeyHash {
            size_t operator()(const OpcodeKey& key) const noexcept {
                const uint64_t packed =
                    (static_cast<uint64_t>(static_cast<uint32_t>(key.first)) << 32) |
                    static_cast<uint32_t>(key.second);
                return std::hash<uint64_t>{}(packed);
            }
        };

        std::unordered_map<int32_t, int32_t> compiled_tick_by_source_index;
        if (compiled_track_it != compiled_source_presentation.tracks.end()) {
            for (const auto& compiled_command : compiled_track_it->commands) {
                if (!is_non_time_opcode_code(compiled_command.opcode) || compiled_command.source_event_index < 0) {
                    continue;
                }
                compiled_tick_by_source_index[compiled_command.source_event_index] = compiled_command.tick;
            }
        }

        std::unordered_map<OpcodeKey, std::vector<int32_t>, OpcodeKeyHash> authored_indices_by_tick_opcode;
        std::unordered_map<OpcodeKey, size_t, OpcodeKeyHash> playback_key_cursors;
        for (size_t authored_index = 0; authored_index < authored_track.opcodes.size(); ++authored_index) {
            const auto& authored_opcode = authored_track.opcodes[authored_index];
            authored_indices_by_tick_opcode[{authored_opcode.tick, authored_opcode.opcode.opcode}].push_back(
                static_cast<int32_t>(authored_index));
        }

        for (auto& command : track.commands) {
            if (!is_non_time_opcode_code(command.opcode) || command.authored_tick < 0) {
                continue;
            }

            if (command.source_event_index >= 0 &&
                static_cast<size_t>(track.track_index) < authored_tick_by_source_index.size()) {
                const auto authored_tick_it =
                    authored_tick_by_source_index[static_cast<size_t>(track.track_index)].find(command.source_event_index);
                if (authored_tick_it != authored_tick_by_source_index[static_cast<size_t>(track.track_index)].end()) {
                    const int32_t authored_tick = authored_tick_it->second;
                    command.authored_tick = authored_tick;
                    const auto authored_index_it =
                        authored_opcode_index_by_source_index[static_cast<size_t>(track.track_index)].find(
                            command.source_event_index);
                    if (authored_index_it !=
                        authored_opcode_index_by_source_index[static_cast<size_t>(track.track_index)].end()) {
                        command.authored_opcode_index = authored_index_it->second;
                    }
                    const auto compiled_tick_it = compiled_tick_by_source_index.find(command.source_event_index);
                    if (compiled_tick_it != compiled_tick_by_source_index.end()) {
                        const int32_t compiled_tick = compiled_tick_it->second;
                        command.tick = std::max(0, command.tick + (authored_tick - compiled_tick));
                    }
                    continue;
                }
            }

            const OpcodeKey key {command.authored_tick, command.opcode};
            const auto candidates_it = authored_indices_by_tick_opcode.find(key);
            if (candidates_it == authored_indices_by_tick_opcode.end() || candidates_it->second.empty()) {
                continue;
            }
            const auto& candidates = candidates_it->second;
            size_t& cursor = playback_key_cursors[key];
            const int32_t authored_opcode_index = candidates[cursor % candidates.size()];
            cursor += 1;
            command.authored_opcode_index = authored_opcode_index;
            if (authored_opcode_index >= 0 &&
                static_cast<size_t>(authored_opcode_index) < authored_track.opcodes.size()) {
                command.authored_tick = authored_track.opcodes[static_cast<size_t>(authored_opcode_index)].tick;
            }
        }

        for (auto& note : track.notes) {
            if (note.authored_start_tick < 0) {
                continue;
            }
            for (size_t authored_index = 0; authored_index < authored_track.spans.size(); ++authored_index) {
                const auto& span = authored_track.spans[authored_index];
                if (span.start_tick == note.authored_start_tick && span.relative_key == note.relative_key) {
                    note.authored_span_index = static_cast<int32_t>(authored_index);
                    note.source_event_index = static_cast<int32_t>(authored_index);
                    break;
                }
            }
        }
    }
}

}  // namespace

FFTJuceAudioProcessor::FFTJuceAudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    reset_unwound_insert_debug_log(make_unwound_debug_session_header("processor_ctor"));
    ensure_default_paths();
}

FFTJuceAudioProcessor::~FFTJuceAudioProcessor() = default;

void FFTJuceAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    host_sample_rate_ = sampleRate;

    // Always prepare the core at 44100 Hz — the SPU is hardwired to PSX rate.
    // processBlock handles resampling when the host rate differs.
    std::string error_message;
    if (!core_.prepare_to_play(
            FFTProcessSetup {
                .sample_rate = FFTFilePlaybackEngine::kSampleRate,
                .max_block_size = samplesPerBlock,
                .output_channels = getTotalNumOutputChannels(),
            },
            &error_message)) {
        status_text_ = juce::String(error_message);
        return;
    }

    resamp_left_.reset();
    resamp_right_.reset();
    // Pre-size the intermediate buffer for the worst-case block at 44100.
    const int spu_block = static_cast<int>(
        std::ceil(samplesPerBlock * FFTFilePlaybackEngine::kSampleRate / sampleRate) + 4);
    resamp_buf_left_.resize(static_cast<size_t>(spu_block));
    resamp_buf_right_.resize(static_cast<size_t>(spu_block));

    const FFTStateReloadReport report = core_.reload_from_state_report();
    apply_reload_report(report, "Ready");
}

void FFTJuceAudioProcessor::releaseResources() {
    core_.set_transport_playing(false);
}

bool FFTJuceAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void FFTJuceAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals no_denormals;

    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel) {
        buffer.clear(channel, 0, buffer.getNumSamples());
    }

    if (!core_.local_transport_active()) {
        bool playing = true;
        int64_t host_sample_position = -1;
        if (auto* playhead = getPlayHead()) {
            if (auto position = playhead->getPosition()) {
                playing = position->getIsPlaying();
                if (const auto time_in_samples = position->getTimeInSamples()) {
                    host_sample_position = *time_in_samples;
                }
            }
        }
        core_.sync_host_transport(playing, host_sample_position);
    }

    if (buffer.getNumChannels() >= 2) {
        const int host_frames = buffer.getNumSamples();
        const double ratio = FFTFilePlaybackEngine::kSampleRate / host_sample_rate_;
        if (std::abs(host_sample_rate_ - FFTFilePlaybackEngine::kSampleRate) < 0.5) {
            core_.process(buffer.getWritePointer(0), buffer.getWritePointer(1), host_frames);
        } else {
            const int spu_frames = static_cast<int>(std::ceil(host_frames * ratio)) + 2;
            if (static_cast<int>(resamp_buf_left_.size()) < spu_frames) {
                resamp_buf_left_.resize(static_cast<size_t>(spu_frames));
                resamp_buf_right_.resize(static_cast<size_t>(spu_frames));
            }
            core_.process(resamp_buf_left_.data(), resamp_buf_right_.data(), spu_frames);
            resamp_left_.processAdding(ratio, resamp_buf_left_.data(), buffer.getWritePointer(0), host_frames, 1.0f);
            resamp_right_.processAdding(ratio, resamp_buf_right_.data(), buffer.getWritePointer(1), host_frames, 1.0f);
        }
    }

    float* left = buffer.getNumChannels() > 0 ? buffer.getWritePointer(0) : nullptr;
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : left;
    if (left == nullptr || right == nullptr) {
        return;
    }

    const auto mix_match_lab_playback = [&](MatchLabSamplePlayback& playback) {
        const juce::SpinLock::ScopedTryLockType lock(playback.lock);
        if (!lock.isLocked() || !playback.active || playback.interleaved.empty()) {
            return;
        }

        const int64_t total_frames = static_cast<int64_t>(playback.interleaved.size() / 2U);
        int64_t frame_position = playback.frame_position;
        for (int frame = 0; frame < buffer.getNumSamples() && frame_position < total_frames; ++frame, ++frame_position) {
            const size_t base = static_cast<size_t>(frame_position) * 2U;
            left[frame] += playback.interleaved[base];
            right[frame] += playback.interleaved[base + 1];
        }
        playback.frame_position = frame_position;
        if (playback.frame_position >= total_frames) {
            playback.active = false;
        }
    };

    mix_match_lab_playback(match_lab_reference_);
    mix_match_lab_playback(match_lab_fft_preview_);
}

juce::AudioProcessorEditor* FFTJuceAudioProcessor::createEditor() {
    return new FFTJuceAudioProcessorEditor(*this);
}

bool FFTJuceAudioProcessor::hasEditor() const { return true; }

const juce::String FFTJuceAudioProcessor::getName() const { return "FFT SMD Editor"; }
bool FFTJuceAudioProcessor::acceptsMidi() const { return false; }
bool FFTJuceAudioProcessor::producesMidi() const { return false; }
bool FFTJuceAudioProcessor::isMidiEffect() const { return false; }
double FFTJuceAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int FFTJuceAudioProcessor::getNumPrograms() { return 1; }
int FFTJuceAudioProcessor::getCurrentProgram() { return 0; }
void FFTJuceAudioProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }
const juce::String FFTJuceAudioProcessor::getProgramName(int index) {
    juce::ignoreUnused(index);
    return {};
}
void FFTJuceAudioProcessor::changeProgramName(int index, const juce::String& newName) {
    juce::ignoreUnused(index, newName);
}

void FFTJuceAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    const std::vector<uint8_t> core_bytes = core_.serialize_state();
    const std::vector<uint8_t> provenance_bytes = serialize_provenance(imported_midi_part_provenance_);

    std::vector<uint8_t> envelope;
    envelope.reserve(sizeof(kStateEnvelopeMagic) + 12 + core_bytes.size() + provenance_bytes.size());
    envelope.insert(envelope.end(), std::begin(kStateEnvelopeMagic), std::end(kStateEnvelopeMagic));
    write_u32_le(envelope, kStateEnvelopeVersion);
    write_u32_le(envelope, static_cast<uint32_t>(core_bytes.size()));
    envelope.insert(envelope.end(), core_bytes.begin(), core_bytes.end());
    write_u32_le(envelope, static_cast<uint32_t>(provenance_bytes.size()));
    envelope.insert(envelope.end(), provenance_bytes.begin(), provenance_bytes.end());

    destData.replaceAll(envelope.data(), envelope.size());
}

void FFTJuceAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0) {
        return;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    const size_t total_size = static_cast<size_t>(sizeInBytes);

    std::vector<uint8_t> core_bytes;
    std::vector<FFTImportedMidiPartProvenance> restored_provenance;
    bool envelope_present = false;

    if (total_size >= sizeof(kStateEnvelopeMagic) &&
        std::memcmp(bytes, kStateEnvelopeMagic, sizeof(kStateEnvelopeMagic)) == 0) {
        size_t cursor = sizeof(kStateEnvelopeMagic);
        uint32_t version = 0;
        uint32_t core_size = 0;
        uint32_t provenance_size = 0;
        if (!read_u32_le(bytes, total_size, cursor, version) ||
            version != kStateEnvelopeVersion ||
            !read_u32_le(bytes, total_size, cursor, core_size) ||
            cursor + core_size > total_size) {
            status_text_ = "State envelope is corrupt";
            return;
        }
        core_bytes.assign(bytes + cursor, bytes + cursor + core_size);
        cursor += core_size;
        if (!read_u32_le(bytes, total_size, cursor, provenance_size) ||
            cursor + provenance_size > total_size) {
            status_text_ = "State envelope is corrupt";
            return;
        }
        if (provenance_size > 0) {
            deserialize_provenance(bytes + cursor, provenance_size, restored_provenance);
        }
        envelope_present = true;
    } else {
        core_bytes.assign(bytes, bytes + total_size);
    }

    std::string error_message;
    if (!core_.restore_state(core_bytes, &error_message)) {
        status_text_ = juce::String(error_message);
        return;
    }
    imported_midi_part_provenance_ = envelope_present
        ? std::move(restored_provenance)
        : std::vector<FFTImportedMidiPartProvenance>{};
    // If the restored paths don't exist on this machine (e.g. state saved on
    // a different OS), replace them with the local config/default. With the
    // public-release defaults (empty fallbacks) this clears stale paths
    // rather than substituting a developer's local copy.
    const auto& state = core_.state();
    if (!state.waveset_path.empty() && !std::filesystem::exists(state.waveset_path)) {
        core_.set_waveset_path_in_state(default_waveset_path());
    }
    if (!state.smd_path.empty() && !std::filesystem::exists(state.smd_path)) {
        core_.set_smd_path_in_state(default_smd_path());
    }
    if (!state.authoring_path.empty() && !std::filesystem::exists(state.authoring_path)) {
        core_.clear_authoring_path_in_state();
    }
    const FFTStateReloadReport report = core_.reload_from_state_report();
    apply_reload_report(report, "State restored");
}

bool FFTJuceAudioProcessor::set_waveset_path(const juce::String& path) {
    const FFTFilePlaybackLoadResult result = core_.load_waveset_path(path.toStdString());
    waveset_status_text_ = juce::String(result.message);
    status_text_ = result.ok ? "WAVESET loaded" : juce::String(result.message);
    if (result.ok) {
        updateHostDisplay();
    }
    return result.ok;
}

bool FFTJuceAudioProcessor::set_music_document_path(const juce::String& path) {
    const FFTFilePlaybackLoadResult result = core_.load_music_document_path(path.toStdString());
    smd_status_text_ = juce::String(result.message);
    status_text_ = result.ok ? "Music document loaded" : juce::String(result.message);
    if (result.ok) {
        imported_midi_part_provenance_.clear();
        updateHostDisplay();
    }
    return result.ok;
}

bool FFTJuceAudioProcessor::create_new_music_document_path(const juce::String& path) {
    const FFTFilePlaybackLoadResult result = core_.create_new_music_document_path(path.toStdString());
    smd_status_text_ = juce::String(result.message);
    status_text_ = result.ok ? "New song created" : juce::String(result.message);
    if (result.ok) {
        imported_midi_part_provenance_.clear();
        updateHostDisplay();
    }
    return result.ok;
}

bool FFTJuceAudioProcessor::import_midi_path(const juce::String& midi_path, const juce::String& authoring_path) {
    std::string error_message;
    const auto imported = import_midi_file_to_authoring_result(
        juce::File(midi_path),
        &error_message);
    if (!imported.has_value()) {
        smd_status_text_ = juce::String(error_message);
        status_text_ = juce::String(error_message);
        return false;
    }

    const FFTFilePlaybackLoadResult result = core_.create_music_document_from_authoring_document(
        authoring_path.toStdString(),
        imported->document,
        "MIDI imported");
    smd_status_text_ = juce::String(result.message);
    status_text_ = result.ok ? "MIDI imported" : juce::String(result.message);
    if (result.ok) {
        imported_midi_part_provenance_ = imported->part_provenance;
        updateHostDisplay();
    } else {
        imported_midi_part_provenance_.clear();
    }
    return result.ok;
}

bool FFTJuceAudioProcessor::set_smd_path(const juce::String& path) {
    return set_music_document_path(path);
}

bool FFTJuceAudioProcessor::export_smd_path(const juce::String& path) {
    return export_smd_path(path, 0u);
}

bool FFTJuceAudioProcessor::export_smd_path(const juce::String& path, size_t target_bytes) {
    return export_smd_path_with_report(path, target_bytes).error.empty();
}

fftplugin::FFTSmdGameCompileReport
FFTJuceAudioProcessor::export_smd_path_with_report(const juce::String& path, size_t target_bytes) {
    fftplugin::FFTSmdGameCompileBudget budget;
    budget.target_bytes = target_bytes;
    fftplugin::FFTSmdGameCompileReport report =
        core_.export_smd_path_for_game(path.toStdString(), budget);
    const bool ok = report.error.empty();
    status_text_ = juce::String(ok ? "Exported. " + report.summary
                                   : report.summary);
    if (ok) {
        updateHostDisplay();
    }
    return report;
}

bool FFTJuceAudioProcessor::convert_tracks_to_poly_track(const std::vector<int>& track_indices) {
    std::vector<int32_t> authored_indices;
    authored_indices.reserve(track_indices.size());
    for (const int track_index : track_indices) {
        authored_indices.push_back(track_index);
    }
    std::string error_message;
    const bool ok = core_.convert_tracks_to_poly_track(authored_indices, &error_message);
    status_text_ = ok ? "PolyTrack created" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::ungroup_poly_track(int track_index) {
    std::string error_message;
    const bool ok = core_.ungroup_poly_track(track_index, &error_message);
    status_text_ = ok ? "PolyTrack ungrouped" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::authored_track_is_poly_track(int track_index) const {
    return core_.authored_track_is_poly_track(track_index);
}

juce::String FFTJuceAudioProcessor::waveset_path() const {
    return juce::String(core_.state().waveset_path);
}

juce::String FFTJuceAudioProcessor::music_document_path() const {
    if (!core_.state().authoring_path.empty()) {
        return juce::String(core_.state().authoring_path);
    }
    return juce::String(core_.state().smd_path);
}

juce::String FFTJuceAudioProcessor::smd_path() const {
    return juce::String(core_.state().smd_path);
}

juce::String FFTJuceAudioProcessor::waveset_status_text() const {
    return waveset_status_text_;
}

juce::String FFTJuceAudioProcessor::smd_status_text() const {
    return smd_status_text_;
}

juce::String FFTJuceAudioProcessor::status_text() const {
    return status_text_;
}

void FFTJuceAudioProcessor::set_status_text_for_debug(const juce::String& text) {
    status_text_ = text;
}

juce::String FFTJuceAudioProcessor::diagnostic_text() const {
    const FFTProcessSetup& setup = core_.process_setup();
    const auto file_exists = [](const std::string& p) {
        return !p.empty() && std::filesystem::exists(p);
    };

    juce::String s;
    s << "=== FFT Plugin Diagnostics ===\n\n";

    s << "[Engine]\n";
    s << "prepared:        " << (core_.prepared() ? "YES" : "NO") << "\n";
    s << "sample_rate:     " << juce::String(setup.sample_rate) << "\n";
    s << "output_channels: " << juce::String(setup.output_channels) << "\n";
    s << "last_error:      " << juce::String(core_.last_error()) << "\n";
    s << "status:          " << status_text_ << "\n\n";

    s << "[WAVESET]\n";
    s << "path:   " << waveset_path() << "\n";
    s << "exists: " << (file_exists(core_.state().waveset_path) ? "YES" : "NO") << "\n";
    s << "status: " << waveset_status_text() << "\n\n";

    s << "[SMD]\n";
    s << "path:   " << smd_path() << "\n";
    s << "exists: " << (file_exists(core_.state().smd_path) ? "YES" : "NO") << "\n";
    s << "status: " << smd_status_text() << "\n\n";

#if defined(_WIN32)
    const char* config_base = std::getenv("APPDATA");
    const std::filesystem::path config_path = config_base
        ? std::filesystem::path(config_base) / "fft-plugin" / "paths.conf"
        : std::filesystem::path();
#else
    const char* home = std::getenv("HOME");
    const std::filesystem::path config_path = home
        ? std::filesystem::path(home) / ".config" / "fft-plugin" / "paths.conf"
        : std::filesystem::path();
#endif
    s << "[Config file]\n";
    s << "path:   " << juce::String(config_path.string()) << "\n";
    s << "exists: " << (std::filesystem::exists(config_path) ? "YES" : "NO") << "\n";

    return s;
}

juce::String FFTJuceAudioProcessor::playback_summary() const {
    const auto& engine = core_.playback_engine();
    juce::StringArray parts;
    parts.add(engine.ready() ? "ready" : "not ready");
    parts.add(core_.transport_playing() ? "playing" : "stopped");
    if (engine.finished()) {
        parts.add("finished");
    }
    parts.add("voices=" + juce::String(engine.active_voice_count()));
    if (engine.has_smd()) {
        parts.add("tempo=" + juce::String(engine.tempo_bpm(), 2) + " bpm");
    }
    return parts.joinIntoString(" | ");
}

int FFTJuceAudioProcessor::inspector_track_count() const {
    const auto& smd = core_.playback_engine().smd_file();
    return smd.has_value() ? static_cast<int>(smd->track_events.size()) : 0;
}

juce::String FFTJuceAudioProcessor::inspector_metadata_text() const {
    const auto& smd = core_.playback_engine().smd_file();
    if (!smd.has_value()) {
        return "No loaded SMD available.\nSelected path: " + music_document_path();
    }
    return juce::String(build_smd_metadata_text(*smd));
}

juce::String FFTJuceAudioProcessor::inspector_track_summary_text() const {
    const auto& smd = core_.playback_engine().smd_file();
    if (!smd.has_value()) {
        return "No track summary available.\n";
    }
    return juce::String(build_smd_track_summary_text(*smd));
}

juce::String FFTJuceAudioProcessor::inspector_track_events_text(int track_index) const {
    const auto& smd = core_.playback_engine().smd_file();
    if (!smd.has_value()) {
        return "No event list available.\n";
    }
    return juce::String(build_smd_track_events_text(*smd, track_index));
}

FFTSmdSongPresentation FFTJuceAudioProcessor::inspector_song_presentation(FFTSmdPresentationMode mode) const {
    const auto& smd = core_.playback_engine().smd_file();
    if (!smd.has_value()) {
        return {};
    }
    FFTSmdSongPresentation presentation;
    if (mode == FFTSmdPresentationMode::source && core_.state().smd_authoring.has_value()) {
        presentation = build_authored_source_presentation(
            *core_.state().smd_authoring,
            [this](int32_t track_index, int32_t source_event_index) {
                return core_.track_source_opcode_disabled(track_index, source_event_index);
            });
    } else if (mode == FFTSmdPresentationMode::playback && core_.state().smd_authoring.has_value()) {
        presentation = build_authored_playback_presentation(
            *core_.state().smd_authoring,
            core_.playback_engine().build_playback_song_presentation());
    } else {
        presentation =
            mode == FFTSmdPresentationMode::playback
            ? core_.playback_engine().build_playback_song_presentation()
            : build_smd_song_presentation(
                *smd,
                FFTSmdPresentationMode::source,
                [this](int32_t track_index, int32_t source_event_index) {
                    return core_.track_source_opcode_disabled(track_index, source_event_index);
                });
    }

    for (auto& track : presentation.tracks) {
        track.muted = core_.track_muted(track.track_index);
        track.soloed = core_.track_soloed(track.track_index);
    }
    return presentation;
}

bool FFTJuceAudioProcessor::start_local_playback(int32_t start_tick, int32_t end_tick) {
    const bool ok = core_.start_local_playback(start_tick, end_tick);
    status_text_ = ok ? "Local playback started" : juce::String(core_.last_error());
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

void FFTJuceAudioProcessor::stop_local_playback() {
    core_.stop_local_playback(false);
    status_text_ = "Local playback stopped";
    updateHostDisplay();
}

bool FFTJuceAudioProcessor::local_transport_active() const {
    return core_.local_transport_active();
}

void FFTJuceAudioProcessor::set_track_muted(int track_index, bool muted) {
    core_.set_track_muted(track_index, muted);
    updateHostDisplay();
}

void FFTJuceAudioProcessor::set_track_soloed(int track_index, bool soloed) {
    core_.set_track_soloed(track_index, soloed);
    updateHostDisplay();
}

bool FFTJuceAudioProcessor::track_muted(int track_index) const {
    return core_.track_muted(track_index);
}

bool FFTJuceAudioProcessor::track_soloed(int track_index) const {
    return core_.track_soloed(track_index);
}

int FFTJuceAudioProcessor::selected_track_id() const {
    return core_.selected_track_id();
}

void FFTJuceAudioProcessor::set_selected_track_id(int track_index) {
    core_.set_selected_track_id(track_index);
}

bool FFTJuceAudioProcessor::set_track_opcode_code(
    int track_index,
    int source_event_index,
    int expected_opcode,
    int new_opcode
) {
    std::string error_message;
    const bool ok = core_.set_track_opcode_code(
        track_index,
        source_event_index,
        expected_opcode,
        new_opcode,
        &error_message);
    status_text_ = ok ? "Opcode updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::delete_track_opcode(
    int track_index,
    int source_event_index,
    int expected_opcode
) {
    std::string error_message;
    const bool ok = core_.delete_track_opcode(
        track_index,
        source_event_index,
        expected_opcode,
        &error_message);
    status_text_ = ok ? "Source event deleted" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::delete_authored_opcode(
    int track_index,
    int authored_opcode_index,
    int expected_opcode
) {
    std::string error_message;
    const bool ok = core_.delete_authored_opcode(
        track_index,
        authored_opcode_index,
        expected_opcode,
        &error_message);
    status_text_ = ok ? "Opcode deleted" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::move_track_opcode(
    int track_index,
    int source_event_index,
    int expected_opcode,
    int target_tick,
    int insertion_sequence_index,
    int* moved_source_event_index
) {
    std::string error_message;
    const bool ok = core_.move_track_opcode(
        track_index,
        source_event_index,
        expected_opcode,
        target_tick,
        insertion_sequence_index,
        moved_source_event_index,
        &error_message);
    status_text_ = ok ? "Opcode moved" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::move_authored_opcode(
    int track_index,
    int authored_opcode_index,
    int expected_opcode,
    int target_tick,
    int insertion_sequence_index,
    int* moved_authored_opcode_index
) {
    std::string error_message;
    const bool ok = core_.move_authored_opcode(
        track_index,
        authored_opcode_index,
        expected_opcode,
        target_tick,
        insertion_sequence_index,
        moved_authored_opcode_index,
        &error_message);
    status_text_ = ok ? "Opcode moved" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::move_track_opcodes(
    int track_index,
    const std::vector<int32_t>& source_event_indices,
    int dragged_source_event_index,
    int target_tick,
    int insertion_sequence_index,
    std::vector<int32_t>* moved_source_event_indices,
    int* moved_dragged_source_event_index
) {
    std::string error_message;
    const bool ok = core_.move_track_opcodes(
        track_index,
        source_event_indices,
        dragged_source_event_index,
        target_tick,
        insertion_sequence_index,
        moved_source_event_indices,
        moved_dragged_source_event_index,
        &error_message);
    status_text_ = ok ? "Opcodes moved" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::move_authored_opcodes(
    int track_index,
    const std::vector<int32_t>& authored_opcode_indices,
    int dragged_authored_opcode_index,
    int target_tick,
    int insertion_sequence_index,
    std::vector<int32_t>* moved_authored_opcode_indices,
    int* moved_dragged_authored_opcode_index
) {
    std::string error_message;
    const bool ok = core_.move_authored_opcodes(
        track_index,
        authored_opcode_indices,
        dragged_authored_opcode_index,
        target_tick,
        insertion_sequence_index,
        moved_authored_opcode_indices,
        moved_dragged_authored_opcode_index,
        &error_message);
    status_text_ = ok ? "Opcodes moved" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::insert_track_time(int track_index, int start_tick, int duration_ticks) {
    std::string error_message;
    const bool ok = core_.insert_track_time(track_index, start_tick, duration_ticks, &error_message);
    status_text_ = ok ? "Time inserted" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::delete_track_time(int track_index, int start_tick, int duration_ticks) {
    std::string error_message;
    const bool ok = core_.delete_track_time(track_index, start_tick, duration_ticks, &error_message);
    status_text_ = ok ? "Time deleted" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::replace_track_note_with_rest(int track_index, int source_event_index) {
    std::string error_message;
    const bool ok = core_.replace_track_note_with_rest(
        track_index,
        source_event_index,
        &error_message);
    status_text_ = ok ? "Note replaced with rest" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::replace_authored_note_with_rest(int track_index, int authored_span_index) {
    std::string error_message;
    const bool ok = core_.replace_authored_note_with_rest(
        track_index,
        authored_span_index,
        &error_message);
    status_text_ = ok ? "Note replaced with rest" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::replace_track_rest_with_note(
    int track_index,
    int source_event_index,
    int note_relative_key,
    int start_offset_ticks,
    int duration_ticks
) {
    std::string error_message;
    const bool ok = core_.replace_track_rest_with_note(
        track_index,
        source_event_index,
        note_relative_key,
        start_offset_ticks,
        duration_ticks,
        &error_message);
    status_text_ = ok
        ? (note_relative_key == 13 ? "Span replaced with rest" : "Span replaced with note")
        : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::replace_authored_rest_with_note(
    int track_index,
    int authored_span_index,
    int note_relative_key,
    int start_offset_ticks,
    int duration_ticks
) {
    std::string error_message;
    const bool ok = core_.replace_authored_rest_with_note(
        track_index,
        authored_span_index,
        note_relative_key,
        start_offset_ticks,
        duration_ticks,
        &error_message);
    status_text_ = ok
        ? (note_relative_key == 13 ? "Span replaced with rest" : "Span replaced with note")
        : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::insert_authored_poly_note(
    int track_index,
    int note_relative_key,
    int start_tick,
    int duration_ticks
) {
    std::string error_message;
    const bool ok = core_.insert_authored_poly_note(
        track_index,
        note_relative_key,
        start_tick,
        duration_ticks,
        &error_message);
    status_text_ = ok ? "Poly note inserted" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::insert_authored_poly_rest(
    int track_index,
    int start_tick,
    int duration_ticks
) {
    std::string error_message;
    const bool ok = core_.insert_authored_poly_rest(
        track_index,
        start_tick,
        duration_ticks,
        &error_message);
    status_text_ = ok ? "Poly rest inserted" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_note_fermata_extension(
    int track_index,
    int source_event_index,
    int extension_ticks
) {
    std::string error_message;
    const bool ok = core_.set_track_note_fermata_extension(
        track_index,
        source_event_index,
        extension_ticks,
        &error_message);
    status_text_ = ok ? "Note fermata updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_authored_note_fermata_extension(
    int track_index,
    int authored_span_index,
    int extension_ticks
) {
    std::string error_message;
    const bool ok = core_.set_authored_note_fermata_extension(
        track_index,
        authored_span_index,
        extension_ticks,
        &error_message);
    status_text_ = ok ? "Note fermata updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_note_geometry(
    int track_index,
    int source_event_index,
    int start_tick,
    int base_duration_ticks,
    int extension_ticks
) {
    std::string error_message;
    const bool ok = core_.set_track_note_geometry(
        track_index,
        source_event_index,
        start_tick,
        base_duration_ticks,
        extension_ticks,
        &error_message);
    status_text_ = ok ? "Note resized" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_authored_note_geometry(
    int track_index,
    int authored_span_index,
    int start_tick,
    int base_duration_ticks,
    int extension_ticks
) {
    std::string error_message;
    const bool ok = core_.set_authored_note_geometry(
        track_index,
        authored_span_index,
        start_tick,
        base_duration_ticks,
        extension_ticks,
        &error_message);
    status_text_ = ok ? "Note resized" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::resize_track_rest_duration(
    int track_index,
    int source_event_index,
    int delta_ticks
) {
    std::string error_message;
    const bool ok = core_.resize_track_rest_duration(
        track_index,
        source_event_index,
        delta_ticks,
        &error_message);
    status_text_ = ok ? "Rest resized" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::resize_authored_rest_duration(
    int track_index,
    int authored_span_index,
    int delta_ticks
) {
    std::string error_message;
    const bool ok = core_.resize_authored_rest_duration(
        track_index,
        authored_span_index,
        delta_ticks,
        &error_message);
    status_text_ = ok ? "Rest resized" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::insert_track_opcode(
    int track_index,
    int target_tick,
    int insertion_sequence_index,
    int opcode,
    const std::vector<int32_t>& params,
    int* inserted_source_event_index
) {
    std::string error_message;
    int32_t inserted_index = -1;
    const bool ok = core_.insert_track_opcode(
        track_index,
        target_tick,
        insertion_sequence_index,
        opcode,
        params,
        &inserted_index,
        &error_message);
    status_text_ = ok ? "Opcode inserted" : juce::String(error_message);
    if (ok) {
        if (inserted_source_event_index != nullptr) {
            *inserted_source_event_index = inserted_index;
        }
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_source_opcode_disabled(
    int track_index,
    int source_event_index,
    bool disabled
) {
    std::string error_message;
    const bool ok = core_.set_track_source_opcode_disabled(
        track_index,
        source_event_index,
        disabled,
        &error_message);
    status_text_ = ok
        ? (disabled ? "Opcode disabled" : "Opcode enabled")
        : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::track_source_opcode_disabled(int track_index, int source_event_index) const {
    return core_.track_source_opcode_disabled(track_index, source_event_index);
}

bool FFTJuceAudioProcessor::set_track_generic_opcode_param_value(
    int track_index,
    int source_event_index,
    int expected_opcode,
    int value,
    int min_value,
    int max_value
) {
    std::string error_message;
    const bool ok = core_.set_track_generic_opcode_param_value(
        track_index,
        source_event_index,
        expected_opcode,
        value,
        min_value,
        max_value,
        &error_message);
    status_text_ = ok ? "Opcode updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_generic_opcode_param_values(
    int track_index,
    int source_event_index,
    int expected_opcode,
    const std::vector<int32_t>& values
) {
    std::string error_message;
    const bool ok = core_.set_track_generic_opcode_param_values(
        track_index,
        source_event_index,
        expected_opcode,
        values,
        &error_message);
    status_text_ = ok ? "Opcode updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_dynamics_opcode_value(
    int track_index,
    int source_event_index,
    int dynamics_value
) {
    std::string error_message;
    const bool ok = core_.set_track_dynamics_opcode_value(
        track_index,
        source_event_index,
        dynamics_value,
        &error_message);
    status_text_ = ok ? "Dynamics updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_pan_opcode_value(
    int track_index,
    int source_event_index,
    int pan_value
) {
    std::string error_message;
    const bool ok = core_.set_track_pan_opcode_value(
        track_index,
        source_event_index,
        pan_value,
        &error_message);
    status_text_ = ok ? "Pan updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_attack_opcode_value(
    int track_index,
    int source_event_index,
    int attack_value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_attack_opcode_value(
        track_index, source_event_index, attack_value, &error_message);
    status_text_ = ok ? "ADSR attack updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_sustain_rate_opcode_value(
    int track_index,
    int source_event_index,
    int sustain_rate_value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_sustain_rate_opcode_value(
        track_index, source_event_index, sustain_rate_value, &error_message);
    status_text_ = ok ? "ADSR sustain rate updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_release_opcode_value(
    int track_index,
    int source_event_index,
    int release_value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_release_opcode_value(
        track_index, source_event_index, release_value, &error_message);
    status_text_ = ok ? "ADSR release updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_decay_opcode_value(
    int track_index,
    int source_event_index,
    int decay_value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_decay_opcode_value(
        track_index, source_event_index, decay_value, &error_message);
    status_text_ = ok ? "ADSR decay updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_sustain_level_opcode_value(
    int track_index,
    int source_event_index,
    int sustain_level_value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_sustain_level_opcode_value(
        track_index, source_event_index, sustain_level_value, &error_message);
    status_text_ = ok ? "ADSR sustain level updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_decay_sustain_opcode_values(
    int track_index,
    int source_event_index,
    int decay_value,
    int sustain_level_value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_decay_sustain_opcode_values(
        track_index,
        source_event_index,
        decay_value,
        sustain_level_value,
        &error_message);
    status_text_ = ok ? "ADSR C7 updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_pitch_lfo_depth_opcode_value(
    int track_index,
    int source_event_index,
    int depth_value
) {
    std::string error_message;
    const bool ok = core_.set_track_pitch_lfo_depth_opcode_value(
        track_index,
        source_event_index,
        depth_value,
        &error_message);
    status_text_ = ok ? "Pitch LFO depth updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_pitch_lfo_opcode_values(
    int track_index,
    int source_event_index,
    int length_value,
    int signed_shape_value,
    int depth_value
) {
    std::string error_message;
    const bool ok = core_.set_track_pitch_lfo_opcode_values(
        track_index,
        source_event_index,
        length_value,
        signed_shape_value,
        depth_value,
        &error_message);
    status_text_ = ok ? "Pitch LFO updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_pitch_bend_opcode_value(
    int track_index,
    int source_event_index,
    int bend_value
) {
    std::string error_message;
    const bool ok = core_.set_track_pitch_bend_opcode_value(
        track_index,
        source_event_index,
        bend_value,
        &error_message);
    status_text_ = ok ? "Pitch bend updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_conditional_seq_flag_opcode_value(
    int track_index,
    int source_event_index,
    int flag_value
) {
    std::string error_message;
    const bool ok = core_.set_track_conditional_seq_flag_opcode_value(
        track_index,
        source_event_index,
        flag_value,
        &error_message);
    status_text_ = ok ? "Conditional flag updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_detune_opcode_value(
    int track_index,
    int source_event_index,
    int detune_value
) {
    std::string error_message;
    const bool ok = core_.set_track_detune_opcode_value(
        track_index,
        source_event_index,
        detune_value,
        &error_message);
    status_text_ = ok ? "Detune updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_unknown_ad_opcode_value(
    int track_index,
    int source_event_index,
    int value
) {
    std::string error_message;
    const bool ok = core_.set_track_unknown_ad_opcode_value(
        track_index,
        source_event_index,
        value,
        &error_message);
    status_text_ = ok ? "AD opcode updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_adsr_slide_opcode_value(
    int track_index,
    int source_event_index,
    int value
) {
    std::string error_message;
    const bool ok = core_.set_track_adsr_slide_opcode_value(
        track_index,
        source_event_index,
        value,
        &error_message);
    status_text_ = ok ? "ADSR slide updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_volume_lfo_depth_opcode_value(
    int track_index,
    int source_event_index,
    int depth_value
) {
    std::string error_message;
    const bool ok = core_.set_track_volume_lfo_depth_opcode_value(
        track_index,
        source_event_index,
        depth_value,
        &error_message);
    status_text_ = ok ? "Volume LFO depth updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_tempo_slide_opcode_values(
    int track_index,
    int source_event_index,
    int first_value,
    int second_value
) {
    std::string error_message;
    const bool ok = core_.set_track_tempo_slide_opcode_values(
        track_index,
        source_event_index,
        first_value,
        second_value,
        &error_message);
    status_text_ = ok ? "Tempo slide updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_volume_lfo_opcode_values(
    int track_index,
    int source_event_index,
    int length_value,
    int signed_shape_value,
    int depth_value
) {
    std::string error_message;
    const bool ok = core_.set_track_volume_lfo_opcode_values(
        track_index,
        source_event_index,
        length_value,
        signed_shape_value,
        depth_value,
        &error_message);
    status_text_ = ok ? "Volume LFO updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_octave_opcode_value(
    int track_index,
    int source_event_index,
    int octave_value
) {
    std::string error_message;
    const bool ok = core_.set_track_octave_opcode_value(
        track_index,
        source_event_index,
        octave_value,
        &error_message);
    status_text_ = ok ? "Octave updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_bank_select_opcode_value(
    int track_index,
    int source_event_index,
    int bank_value
) {
    std::string error_message;
    const bool ok = core_.set_track_bank_select_opcode_value(
        track_index,
        source_event_index,
        bank_value,
        &error_message);
    status_text_ = ok ? "Bank updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_tempo_opcode_value(
    int track_index,
    int source_event_index,
    int tempo_value
) {
    std::string error_message;
    const bool ok = core_.set_track_tempo_opcode_value(
        track_index,
        source_event_index,
        tempo_value,
        &error_message);
    status_text_ = ok ? "Tempo updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_time_signature_opcode_values(
    int track_index,
    int source_event_index,
    int numerator,
    int denominator
) {
    std::string error_message;
    const bool ok = core_.set_track_time_signature_opcode_values(
        track_index,
        source_event_index,
        numerator,
        denominator,
        &error_message);
    status_text_ = ok ? "Time signature updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::set_track_instrument_opcode_value(
    int track_index,
    int source_event_index,
    int instrument_id
) {
    std::string error_message;
    const bool ok = core_.set_track_instrument_opcode_value(
        track_index,
        source_event_index,
        instrument_id,
        &error_message);
    status_text_ = ok ? "Instrument updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

int FFTJuceAudioProcessor::track_transposition(int track_index) const {
    return static_cast<int>(core_.track_transposition(static_cast<int32_t>(track_index)));
}

bool FFTJuceAudioProcessor::set_track_transposition(int track_index, int semitones) {
    std::string error_message;
    const bool ok = core_.set_track_transposition(
        static_cast<int32_t>(track_index),
        static_cast<int32_t>(semitones),
        &error_message);
    status_text_ = ok ? "Track transposition updated" : juce::String(error_message);
    if (ok) {
        updateHostDisplay();
    }
    return ok;
}

bool FFTJuceAudioProcessor::start_preview_note(const FFTPreviewNoteRequest& request) {
    return play_match_lab_fft_note(request, 700, 220);
}

void FFTJuceAudioProcessor::stop_preview_note(int16_t midi_note) {
    juce::ignoreUnused(midi_note);
    stop_match_lab_reference();
}

bool FFTJuceAudioProcessor::play_match_lab_fft_note(
    const FFTPreviewNoteRequest& request,
    int duration_ms,
    int release_ms
) {
    std::vector<float> interleaved = render_fft_preview_note_buffer(
        core_.playback_engine().waveset_service(),
        request,
        FFTFilePlaybackEngine::kSampleRate,
        duration_ms,
        release_ms);
    if (interleaved.empty()) {
        status_text_ = "Failed to render FFT match preview";
        return false;
    }

    {
        const juce::SpinLock::ScopedLockType reference_lock(match_lab_reference_.lock);
        match_lab_reference_.active = false;
        match_lab_reference_.frame_position = 0;
    }
    const juce::SpinLock::ScopedLockType lock(match_lab_fft_preview_.lock);
    match_lab_fft_preview_.interleaved = std::move(interleaved);
    match_lab_fft_preview_.frame_position = 0;
    match_lab_fft_preview_.active = true;
    return true;
}

bool FFTJuceAudioProcessor::load_match_lab_reference_wav(const juce::String& path) {
    const juce::File file(path);
    if (!file.existsAsFile()) {
        status_text_ = "Reference WAV not found";
        return false;
    }

    std::vector<float> interleaved = load_wav_interleaved_f32(file);
    if (interleaved.empty()) {
        status_text_ = "Failed to load reference WAV";
        return false;
    }

    {
        const juce::SpinLock::ScopedLockType fft_lock(match_lab_fft_preview_.lock);
        match_lab_fft_preview_.active = false;
        match_lab_fft_preview_.frame_position = 0;
    }
    const juce::SpinLock::ScopedLockType lock(match_lab_reference_.lock);
    match_lab_reference_.interleaved = std::move(interleaved);
    match_lab_reference_.frame_position = 0;
    match_lab_reference_.active = false;
    return true;
}

void FFTJuceAudioProcessor::play_match_lab_reference() {
    {
        const juce::SpinLock::ScopedLockType fft_lock(match_lab_fft_preview_.lock);
        match_lab_fft_preview_.active = false;
        match_lab_fft_preview_.frame_position = 0;
    }
    const juce::SpinLock::ScopedLockType reference_lock(match_lab_reference_.lock);
    match_lab_reference_.frame_position = 0;
    match_lab_reference_.active = !match_lab_reference_.interleaved.empty();
}

void FFTJuceAudioProcessor::stop_match_lab_reference() {
    {
        const juce::SpinLock::ScopedLockType reference_lock(match_lab_reference_.lock);
        match_lab_reference_.active = false;
        match_lab_reference_.frame_position = 0;
    }
    const juce::SpinLock::ScopedLockType fft_lock(match_lab_fft_preview_.lock);
    match_lab_fft_preview_.active = false;
    match_lab_fft_preview_.frame_position = 0;
}

std::optional<FFTInstrumentInfo> FFTJuceAudioProcessor::loaded_instrument_info(int32_t played_sample_id) const {
    return core_.playback_engine().waveset_service().instrument_info(played_sample_id);
}

std::optional<FFTMatchLabSeed> FFTJuceAudioProcessor::match_lab_seed_for_note(int track_index, int source_event_index) const {
    if (track_index < 0 || source_event_index < 0 ||
        static_cast<size_t>(track_index) >= imported_midi_part_provenance_.size()) {
        return std::nullopt;
    }

    const auto& provenance = imported_midi_part_provenance_[static_cast<size_t>(track_index)];
    if (static_cast<size_t>(source_event_index) >= provenance.notes_by_authored_index.size()) {
        return std::nullopt;
    }

    const auto& state = core_.state();
    if (!state.smd_authoring.has_value() ||
        static_cast<size_t>(track_index) >= state.smd_authoring->parts.size()) {
        return std::nullopt;
    }

    const auto& part = state.smd_authoring->parts[static_cast<size_t>(track_index)];
    if (part.kind != FFTSmdAuthoringPartKind::poly_track ||
        static_cast<size_t>(source_event_index) >= part.poly_track.notes.size()) {
        return std::nullopt;
    }

    const auto& imported_note = provenance.notes_by_authored_index[static_cast<size_t>(source_event_index)];
    const auto& authored_note = part.poly_track.notes[static_cast<size_t>(source_event_index)];

    int32_t played_sample_id = 0;
    int32_t octave = imported_note.midi_note / 12;
    int32_t dynamics = 63;
    int32_t pan = 64;

    for (const auto& opcode : part.poly_track.opcodes) {
        if (opcode.tick > authored_note.start_tick) {
            break;
        }
        switch (opcode.opcode.opcode) {
        case 0xAC:
            if (!opcode.opcode.params.empty()) {
                played_sample_id = smd_instrument_opcode_param_to_played_sample_id(opcode.opcode.params[0]);
            }
            break;
        case 0x94:
            if (!opcode.opcode.params.empty()) {
                octave = opcode.opcode.params[0];
            }
            break;
        case 0x95:
            ++octave;
            break;
        case 0x96:
            --octave;
            break;
        case 0xE0:
            if (!opcode.opcode.params.empty()) {
                dynamics = opcode.opcode.params[0];
            }
            break;
        case 0xE8:
            if (!opcode.opcode.params.empty()) {
                pan = opcode.opcode.params[0];
            }
            break;
        default:
            break;
        }
    }

    int32_t tempo_value = state.smd_authoring->initial_tempo > 0 ? state.smd_authoring->initial_tempo : 102;
    if (!state.smd_authoring->parts.empty()) {
        const auto& conductor = state.smd_authoring->parts.front();
        if (conductor.kind == FFTSmdAuthoringPartKind::raw_track) {
            for (const auto& opcode : conductor.raw_track.opcodes) {
                if (opcode.tick > authored_note.start_tick) {
                    break;
                }
                if (opcode.opcode.opcode == 0xA0 && !opcode.opcode.params.empty()) {
                    tempo_value = opcode.opcode.params[0];
                }
            }
        }
    }

    const double bpm = std::max(1.0, fft_tempo_to_bpm(tempo_value));
    const double quarter_note_ms = 60000.0 / bpm;
    const int32_t duration_ms = std::clamp(
        static_cast<int32_t>(std::lround((static_cast<double>(imported_note.duration_ticks) / 48.0) * quarter_note_ms)),
        120,
        4000);

    FFTMatchLabSeed seed;
    seed.has_midi_reference = true;
    seed.gm_program = std::clamp(imported_note.gm_program, 0, 127);
    seed.gm_midi_note = imported_note.midi_note;
    seed.velocity = std::clamp(imported_note.velocity, 1, 127);
    seed.gm_volume = std::clamp(imported_note.gm_volume, 0, 127);
    seed.gm_pan = std::clamp(imported_note.gm_pan, 0, 127);
    seed.gm_expression = std::clamp(imported_note.gm_expression, 0, 127);
    seed.duration_ms = duration_ms;
    seed.fft_played_sample_id = played_sample_id;
    seed.fft_midi_note = std::clamp(octave * 12 + authored_note.relative_key, 0, 127);
    seed.dynamics = std::clamp(dynamics, 0, 127);
    seed.pan = std::clamp(pan, 0, 127);
    seed.source_name = imported_note.source_name;
    seed.editor_track_name = part.name;
    return seed;
}

int FFTJuceAudioProcessor::current_playback_tick() const {
    return core_.current_playback_tick();
}

std::vector<int32_t> FFTJuceAudioProcessor::current_source_track_ticks() const {
    return core_.current_source_track_ticks();
}

bool FFTJuceAudioProcessor::reload_from_state() {
    const FFTStateReloadReport report = core_.reload_from_state_report();
    apply_reload_report(report, "State restored");
    return report.ready;
}

void FFTJuceAudioProcessor::apply_reload_report(
    const FFTStateReloadReport& report,
    const juce::String& success_text
) {
    waveset_status_text_ = juce::String(report.waveset_message);
    smd_status_text_ = juce::String(report.smd_message);
    status_text_ = report.ready ? success_text : juce::String(report.summary);
}

void FFTJuceAudioProcessor::ensure_default_paths() {
    if (core_.state().waveset_path.empty()) {
        core_.set_waveset_path_in_state(default_waveset_path());
    }
    if (core_.state().smd_path.empty()) {
        core_.set_smd_path_in_state(default_smd_path());
    }
}

}  // namespace jucewrap
}  // namespace fftplugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new fftplugin::jucewrap::FFTJuceAudioProcessor();
}
