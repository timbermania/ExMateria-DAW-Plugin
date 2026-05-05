#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "asset_paths.h"
#include "fft_plugin/fft_spu_preview_adapter.h"
#include "fft_plugin/fft_spu_preview_core.h"
#include "fft_plugin/fft_waveset_file_service.h"

namespace {

struct Config {
    std::string waveset_path = fftplugin::asset_paths::default_waveset();
    std::string out_wav_path;
    std::string out_json_path;
    int32_t instrument_id = 86;
    int16_t midi_note = 60;
    int16_t velocity = 127;
    int32_t frame_count = 22050;
    bool reverb = false;
};

struct Stats {
    int64_t abs_sum = 0;
    int32_t peak = 0;
    int32_t frame_count = 0;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--waveset=", 0) == 0) {
            cfg.waveset_path = arg.substr(std::string("--waveset=").size());
        } else if (arg.rfind("--out-wav=", 0) == 0) {
            cfg.out_wav_path = arg.substr(std::string("--out-wav=").size());
        } else if (arg.rfind("--out-json=", 0) == 0) {
            cfg.out_json_path = arg.substr(std::string("--out-json=").size());
        } else if (arg.rfind("--instrument=", 0) == 0) {
            cfg.instrument_id = std::stoi(arg.substr(std::string("--instrument=").size()));
        } else if (arg.rfind("--note=", 0) == 0) {
            cfg.midi_note = static_cast<int16_t>(std::stoi(arg.substr(std::string("--note=").size())));
        } else if (arg.rfind("--velocity=", 0) == 0) {
            cfg.velocity = static_cast<int16_t>(std::stoi(arg.substr(std::string("--velocity=").size())));
        } else if (arg.rfind("--frames=", 0) == 0) {
            cfg.frame_count = std::stoi(arg.substr(std::string("--frames=").size()));
        } else if (arg == "--reverb") {
            cfg.reverb = true;
        }
    }
    return cfg;
}

void write_u16_le(std::ofstream& out, uint16_t value) {
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
}

void write_u32_le(std::ofstream& out, uint32_t value) {
    out.put(static_cast<char>(value & 0xFF));
    out.put(static_cast<char>((value >> 8) & 0xFF));
    out.put(static_cast<char>((value >> 16) & 0xFF));
    out.put(static_cast<char>((value >> 24) & 0xFF));
}

void write_wav_stereo(const std::string& path, const std::vector<int16_t>& pcm) {
    if (path.empty()) {
        return;
    }

    std::ofstream out(path, std::ios::binary);
    const uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    out.write("RIFF", 4);
    write_u32_le(out, 36 + data_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32_le(out, 16);
    write_u16_le(out, 1);
    write_u16_le(out, 2);
    write_u32_le(out, 44100);
    write_u32_le(out, 44100 * 4);
    write_u16_le(out, 4);
    write_u16_le(out, 16);
    out.write("data", 4);
    write_u32_le(out, data_size);
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(data_size));
}

Stats compute_stats(const std::vector<int16_t>& pcm) {
    Stats stats;
    stats.frame_count = static_cast<int32_t>(pcm.size() / 2);
    stats.abs_sum = std::accumulate(
        pcm.begin(),
        pcm.end(),
        int64_t {0},
        [](int64_t acc, int16_t sample) { return acc + std::abs(static_cast<int>(sample)); }
    );
    const auto peak_it = std::max_element(
        pcm.begin(),
        pcm.end(),
        [](int16_t lhs, int16_t rhs) {
            return std::abs(static_cast<int>(lhs)) < std::abs(static_cast<int>(rhs));
        }
    );
    stats.peak = peak_it == pcm.end() ? 0 : static_cast<int32_t>(*peak_it);
    return stats;
}

void write_stats_json(const std::string& path, const Config& cfg, const Stats& stats) {
    if (path.empty()) {
        return;
    }

    std::ofstream out(path, std::ios::binary);
    out << "{\n";
    out << "  \"waveset_path\": \"" << cfg.waveset_path << "\",\n";
    out << "  \"instrument_id\": " << cfg.instrument_id << ",\n";
    out << "  \"midi_note\": " << cfg.midi_note << ",\n";
    out << "  \"velocity\": " << cfg.velocity << ",\n";
    out << "  \"frame_count\": " << stats.frame_count << ",\n";
    out << "  \"reverb\": " << (cfg.reverb ? "true" : "false") << ",\n";
    out << "  \"pcm_abs_sum\": " << stats.abs_sum << ",\n";
    out << "  \"pcm_peak\": " << stats.peak << "\n";
    out << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);

    fftplugin::FFTWavesetFileService waveset_service;
    const auto load_result = waveset_service.load_from_path(cfg.waveset_path);
    if (!load_result.ok) {
        std::cerr << "Failed to load waveset: " << load_result.message << "\n";
        return 1;
    }

    fftplugin::FFTSpuPreviewCore spu_core;
    fftplugin::FFTSpuPreviewAdapter preview(&spu_core);
    preview.set_instrument_bank(&waveset_service);
    preview.set_waveset_service(&waveset_service);
    preview.configure_preview(fftplugin::FFTPreviewBackendConfig {
        .sample_rate = 44100,
        .max_voices = 24,
        .reverb_enabled = cfg.reverb,
        .lfo_tick_samples = 611,
    });

    preview.note_on(fftplugin::FFTPreviewNoteRequest {
        .midi_note = cfg.midi_note,
        .velocity = cfg.velocity,
        .instrument_id = cfg.instrument_id,
    });

    const auto pcm = spu_core.render_interleaved_pcm16(cfg.frame_count);
    const Stats stats = compute_stats(pcm);

    write_wav_stereo(cfg.out_wav_path, pcm);
    write_stats_json(cfg.out_json_path, cfg, stats);

    std::cout << "waveset_loaded=yes\n";
    std::cout << "instrument_count=" << waveset_service.instrument_count() << "\n";
    std::cout << "frame_count=" << stats.frame_count << "\n";
    std::cout << "pcm_abs_sum=" << stats.abs_sum << "\n";
    std::cout << "pcm_peak=" << stats.peak << "\n";
    if (!cfg.out_wav_path.empty()) {
        std::cout << "saved_wav=" << cfg.out_wav_path << "\n";
    }
    if (!cfg.out_json_path.empty()) {
        std::cout << "saved_json=" << cfg.out_json_path << "\n";
    }
    return 0;
}
