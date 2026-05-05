#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_smd_sequencer.h"
#include "fft_plugin/fft_spu_preview_core.h"
#include "fft_plugin/fft_waveset_file_service.h"

namespace {

void write_wav_stereo(const std::string& path, const std::vector<int16_t>& pcm) {
    std::ofstream out(path, std::ios::binary);
    const uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    const uint32_t riff_size = 36U + data_size;
    const uint16_t audio_format = 1;
    const uint16_t channels = 2;
    const uint32_t sample_rate = 44100;
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

}  // namespace

int main(int argc, char** argv) {
    const std::string waveset_path = argc > 1 ? argv[1] : fftplugin::asset_paths::default_waveset();
    const std::string smd_path = argc > 2 ? argv[2] : fftplugin::asset_paths::default_smd();
    const std::string out_path = argc > 3
        ? argv[3]
        : "/tmp/fft_music31_plugin_mix.wav";
    const double seconds = argc > 4 ? std::stod(argv[4]) : 43.0;

    fftplugin::FFTWavesetFileService waveset_service;
    const auto waveset_result = waveset_service.load_from_path(waveset_path);
    if (!waveset_result.ok) {
        std::cerr << "waveset_error=" << waveset_result.message << "\n";
        return 1;
    }

    std::string smd_error;
    const auto smd = fftplugin::load_smd_file(smd_path, &smd_error);
    if (!smd.has_value()) {
        std::cerr << "smd_error=" << smd_error << "\n";
        return 1;
    }

    fftplugin::FFTSpuPreviewCore spu_core;
    fftplugin::FFTSmdSequencer sequencer(&spu_core, &waveset_service);
    std::string seq_error;
    if (!sequencer.load_smd(*smd, &seq_error)) {
        std::cerr << "sequencer_error=" << seq_error << "\n";
        return 1;
    }

    const int32_t total_frames = static_cast<int32_t>(seconds * 44100.0);
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(total_frames) * 2U);

    while (static_cast<int32_t>(pcm.size() / 2) < total_frames) {
        if (sequencer.tick()) {
            auto tick_pcm = sequencer.render_tick_pcm16();
            pcm.insert(pcm.end(), tick_pcm.begin(), tick_pcm.end());
            continue;
        }
        if (sequencer.has_active_audio()) {
            const int32_t remaining_frames = total_frames - static_cast<int32_t>(pcm.size() / 2);
            auto tail_pcm = sequencer.render_frames_only_pcm16(remaining_frames);
            pcm.insert(pcm.end(), tail_pcm.begin(), tail_pcm.end());
        }
        break;
    }

    write_wav_stereo(out_path, pcm);
    std::cout << "track_count=" << smd->track_count << "\n";
    std::cout << "tempo_bpm=" << sequencer.tempo_bpm() << "\n";
    std::cout << "frames=" << (pcm.size() / 2) << "\n";
    std::cout << "saved=" << out_path << "\n";
    return 0;
}
