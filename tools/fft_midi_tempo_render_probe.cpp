// End-to-end check: import aerith.mid the way the plugin does,
// save the authoring doc, render audio, edit tick-0 tempo opcode,
// render again, hash both renders and report whether they differ.
//
// usage:
//   fft_midi_tempo_render_probe <waveset.wd> <input.mid> <out_dir> [seconds=12] [new_tempo=200]
//
// Outputs in <out_dir>:
//   imported.fftauth
//   render_before.wav
//   render_after.wav

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "fft_plugin/fft_plugin_processor_core.h"
#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_opcodes.h"
#include "FFTMidiImport.h"

namespace {

constexpr int32_t kTempoOpcode = 0xA0;

void write_wav_stereo(const std::string& path, const std::vector<int16_t>& pcm) {
    std::ofstream out(path, std::ios::binary);
    const uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    const uint32_t riff_size = 36U + data_size;
    const uint16_t audio_format = 1;
    const uint16_t channels = 2;
    const uint32_t sample_rate = fftplugin::FFTFilePlaybackEngine::kSampleRate;
    const uint16_t bits_per_sample = 16;
    const uint16_t block_align = static_cast<uint16_t>(channels * (bits_per_sample / 8));
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t fmt_size = 16;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riff_size), sizeof(riff_size));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmt_size), sizeof(fmt_size));
    out.write(reinterpret_cast<const char*>(&audio_format), sizeof(audio_format));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sample_rate), sizeof(sample_rate));
    out.write(reinterpret_cast<const char*>(&byte_rate), sizeof(byte_rate));
    out.write(reinterpret_cast<const char*>(&block_align), sizeof(block_align));
    out.write(reinterpret_cast<const char*>(&bits_per_sample), sizeof(bits_per_sample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(data_size));
}

std::string fnv1a64_hex(const std::vector<int16_t>& pcm) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    const size_t n = pcm.size() * sizeof(int16_t);
    for (size_t i = 0; i < n; ++i) {
        h ^= bytes[i];
        h *= 0x100000001b3ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << h;
    return out.str();
}

bool render_audio(
    fftplugin::FFTPluginProcessorCore& processor,
    double seconds,
    int32_t block_size,
    std::vector<int16_t>& pcm
) {
    std::cerr << "[render_audio.before_play] bpm=" << processor.playback_engine().tempo_bpm() << "\n";
    processor.set_transport_playing(true);
    std::cerr << "[render_audio.after_play] bpm=" << processor.playback_engine().tempo_bpm() << "\n";
    const int32_t total_frames = static_cast<int32_t>(seconds * 44100.0);
    pcm.clear();
    pcm.reserve(static_cast<size_t>(total_frames) * 2U);
    std::vector<float> left(static_cast<size_t>(block_size), 0.0F);
    std::vector<float> right(static_cast<size_t>(block_size), 0.0F);
    while (static_cast<int32_t>(pcm.size() / 2U) < total_frames) {
        const int32_t frames_remaining = total_frames - static_cast<int32_t>(pcm.size() / 2U);
        const int32_t chunk_frames = std::min(block_size, frames_remaining);
        processor.process(left.data(), right.data(), chunk_frames);
        if (pcm.empty()) {
            std::cerr << "[render_audio.after_first_chunk] bpm="
                      << processor.playback_engine().tempo_bpm()
                      << " frames=" << chunk_frames << "\n";
        }
        for (int32_t frame = 0; frame < chunk_frames; ++frame) {
            const float l = std::clamp(left[frame], -1.0F, 1.0F);
            const float r = std::clamp(right[frame], -1.0F, 1.0F);
            const int32_t l_pcm = std::clamp(static_cast<int32_t>(l * 32768.0F), -32768, 32767);
            const int32_t r_pcm = std::clamp(static_cast<int32_t>(r * 32768.0F), -32768, 32767);
            pcm.push_back(static_cast<int16_t>(l_pcm));
            pcm.push_back(static_cast<int16_t>(r_pcm));
        }
        if (!processor.transport_playing() && processor.playback_engine().finished()) {
            break;
        }
    }
    processor.set_transport_playing(false);
    return true;
}

struct TempoOp {
    int32_t part_index;
    int32_t source_event_index;
    int32_t tick;
    int32_t value;
};

std::vector<TempoOp> collect_part0_tempos(const fftplugin::FFTSmdAuthoringDocument& doc) {
    std::vector<TempoOp> out;
    if (doc.parts.empty()) return out;
    const auto& part = doc.parts[0];
    const auto& opcodes = part.kind == fftplugin::FFTSmdAuthoringPartKind::poly_track
        ? part.poly_track.opcodes
        : part.raw_track.opcodes;
    const auto compiled = fftplugin::compile_smd_authoring_document(doc);
    const auto& sei_map = compiled.authored_opcode_source_indices.empty()
        ? std::vector<int32_t>{}
        : compiled.authored_opcode_source_indices[0];
    for (size_t oi = 0; oi < opcodes.size(); ++oi) {
        if (opcodes[oi].opcode.opcode == kTempoOpcode) {
            out.push_back(TempoOp{
                .part_index = 0,
                .source_event_index = oi < sei_map.size() ? sei_map[oi] : -1,
                .tick = opcodes[oi].tick,
                .value = opcodes[oi].opcode.params.empty() ? -1 : opcodes[oi].opcode.params[0],
            });
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: fft_midi_tempo_render_probe <waveset.wd> <input.mid> <out_dir>"
                  << " [seconds=12] [new_tempo=200]\n";
        return 1;
    }
    const std::string waveset_path = argv[1];
    const std::string midi_path = argv[2];
    const std::string out_dir = argv[3];
    const double seconds = argc > 4 ? std::stod(argv[4]) : 12.0;
    const int32_t new_tempo = argc > 5 ? std::stoi(argv[5]) : 200;

    std::filesystem::create_directories(out_dir);
    const std::string fftauth_path = (std::filesystem::path(out_dir) / "imported.fftauth").string();
    const std::string before_wav = (std::filesystem::path(out_dir) / "render_before.wav").string();
    const std::string after_wav = (std::filesystem::path(out_dir) / "render_after.wav").string();

    // 1) Import MIDI exactly as the plugin does.
    std::string err;
    const auto imported = fftplugin::jucewrap::import_midi_file_to_authoring_document(
        juce::File(midi_path), &err);
    if (!imported.has_value()) {
        std::cerr << "midi_import_failed: " << err << "\n";
        return 1;
    }
    std::cout << "midi_import_ok parts=" << imported->parts.size()
              << " initial_tempo=" << imported->initial_tempo << "\n";

    // 2) Save authoring doc to disk so the .fftauth itself can be inspected.
    if (!fftplugin::save_smd_authoring_document(fftauth_path, *imported, &err)) {
        std::cerr << "save_authoring_failed: " << err << "\n";
        return 1;
    }
    std::cout << "wrote " << fftauth_path << "\n";

    // 3) First render: load via the plugin's processor.
    fftplugin::FFTPluginProcessorCore processor;
    if (!processor.prepare_to_play(fftplugin::FFTProcessSetup{
            .sample_rate = 44100.0,
            .max_block_size = 512,
            .output_channels = 2,
        }, &err)) {
        std::cerr << "prepare_failed: " << err << "\n";
        return 1;
    }
    const auto wave_res = processor.load_waveset_path(waveset_path);
    if (!wave_res.ok) {
        std::cerr << "waveset_load_failed: " << wave_res.message << "\n";
        return 1;
    }
    const auto load_res = processor.load_music_document_path(fftauth_path);
    if (!load_res.ok) {
        std::cerr << "fftauth_load_failed: " << load_res.message << "\n";
        return 1;
    }
    std::cout << "load_ok engine_bpm_initial=" << processor.playback_engine().tempo_bpm()
              << " doc_initial_tempo=" << processor.state().smd_authoring->initial_tempo << "\n";

    auto tempos = collect_part0_tempos(*processor.state().smd_authoring);
    std::cout << "conductor_tempo_count=" << tempos.size() << "\n";
    for (size_t i = 0; i < tempos.size() && i < 5; ++i) {
        std::cout << "  tempo[" << i << "] tick=" << tempos[i].tick
                  << " sei=" << tempos[i].source_event_index
                  << " value=" << tempos[i].value << "\n";
    }

    std::vector<int16_t> pcm_before;
    render_audio(processor, seconds, 512, pcm_before);
    write_wav_stereo(before_wav, pcm_before);
    const std::string hash_before = fnv1a64_hex(pcm_before);
    std::cout << "render_before bytes=" << pcm_before.size() * sizeof(int16_t)
              << " hash=" << hash_before
              << " engine_bpm_after_render=" << processor.playback_engine().tempo_bpm() << "\n";

    // 4) Edit the tick-0 tempo opcode (the earliest one).
    if (tempos.empty()) {
        std::cerr << "no_tempo_opcodes_to_edit\n";
        return 2;
    }
    auto earliest_it = std::min_element(tempos.begin(), tempos.end(),
        [](const TempoOp& a, const TempoOp& b) { return a.tick < b.tick; });
    std::cout << "editing tempo at tick=" << earliest_it->tick
              << " sei=" << earliest_it->source_event_index
              << " from=" << earliest_it->value << " to=" << new_tempo << "\n";

    std::string edit_err;
    const bool edit_ok = processor.set_track_tempo_opcode_value(
        earliest_it->part_index, earliest_it->source_event_index, new_tempo, &edit_err);
    std::cout << "edit_ok=" << (edit_ok ? "true" : "false")
              << " err=\"" << edit_err << "\""
              << " engine_bpm_after_edit=" << processor.playback_engine().tempo_bpm()
              << " doc_initial_tempo_after_edit=" << processor.state().smd_authoring->initial_tempo << "\n";

    // 5) Second render. Rewind to song start so we render the same window.
    processor.rewind();
    std::vector<int16_t> pcm_after;
    render_audio(processor, seconds, 512, pcm_after);
    write_wav_stereo(after_wav, pcm_after);
    const std::string hash_after = fnv1a64_hex(pcm_after);
    std::cout << "render_after  bytes=" << pcm_after.size() * sizeof(int16_t)
              << " hash=" << hash_after
              << " engine_bpm_after_render=" << processor.playback_engine().tempo_bpm() << "\n";

    std::cout << "\nresult audio_changed=" << (hash_before != hash_after ? "yes" : "no") << "\n";
    return hash_before != hash_after ? 0 : 3;
}
