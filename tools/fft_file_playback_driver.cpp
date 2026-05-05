#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "fft_plugin/fft_file_playback_engine.h"

namespace {

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

}  // namespace

int main(int argc, char** argv) {
    const std::string waveset_path = argc > 1 ? argv[1] : fftplugin::asset_paths::default_waveset();
    const std::string smd_path = argc > 2 ? argv[2] : fftplugin::asset_paths::default_smd();
    const std::string out_path = argc > 3
        ? argv[3]
        : "/tmp/fft_music31_file_player_mix.wav";
    const double seconds = argc > 4 ? std::stod(argv[4]) : 43.0;
    const int32_t block_size = argc > 5 ? std::stoi(argv[5]) : 512;

    fftplugin::FFTFilePlaybackEngine engine;
    const auto waveset_result = engine.load_waveset_path(waveset_path);
    if (!waveset_result.ok) {
        std::cerr << "waveset_error=" << waveset_result.message << "\n";
        return 1;
    }

    const auto smd_result = engine.load_smd_path(smd_path);
    if (!smd_result.ok) {
        std::cerr << "smd_error=" << smd_result.message << "\n";
        return 1;
    }

    engine.play();
    const int32_t total_frames = static_cast<int32_t>(seconds * fftplugin::FFTFilePlaybackEngine::kSampleRate);
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(total_frames) * 2U);

    while (static_cast<int32_t>(pcm.size() / 2U) < total_frames) {
        const int32_t frames_remaining = total_frames - static_cast<int32_t>(pcm.size() / 2U);
        const int32_t chunk_frames = std::min(block_size, frames_remaining);
        auto chunk = engine.render_interleaved_pcm16(chunk_frames);
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        if (!engine.playing() && engine.finished()) {
            break;
        }
    }

    write_wav_stereo(out_path, pcm);
    std::cout << "track_count=" << smd_result.track_count << "\n";
    std::cout << "instrument_count=" << waveset_result.instrument_count << "\n";
    std::cout << "tempo_bpm=" << engine.tempo_bpm() << "\n";
    std::cout << "frames=" << (pcm.size() / 2U) << "\n";
    std::cout << "block_size=" << block_size << "\n";
    std::cout << "saved=" << out_path << "\n";
    return 0;
}
