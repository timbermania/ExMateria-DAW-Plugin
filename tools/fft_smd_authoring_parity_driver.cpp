#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "asset_paths.h"
#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_file_playback_engine.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_file.h"
#include "test_driver_event_utils.h"

namespace {

using fftplugin::test_driver::describe_event;
using fftplugin::test_driver::same_event;

std::vector<int16_t> render_pcm(
    fftplugin::FFTFilePlaybackEngine& engine,
    double seconds,
    int32_t block_size
) {
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
    return pcm;
}

std::string temp_authoring_path() {
    if (const char* temp = std::getenv("TEMP"); temp != nullptr && *temp != '\0') {
        return std::string(temp) + "\\fft_smd_authoring_parity.fftauth";
    }
    return "C:\\Windows\\Temp\\fft_smd_authoring_parity.fftauth";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string waveset_path = argc > 1 ? argv[1] : fftplugin::asset_paths::default_waveset();
    const std::string smd_path = argc > 2 ? argv[2] : fftplugin::asset_paths::default_smd();
    const double seconds = argc > 3 ? std::stod(argv[3]) : 10.0;
    const int32_t block_size = argc > 4 ? std::stoi(argv[4]) : 512;

    std::string error_message;
    const auto raw_smd = fftplugin::load_smd_file(smd_path, &error_message);
    if (!raw_smd.has_value()) {
        std::cerr << "load_smd_error=" << error_message << "\n";
        return 1;
    }

    const auto authored = fftplugin::import_smd_authoring_document(*raw_smd);
    const auto compiled = fftplugin::compile_smd_authoring_document(authored);
    const std::string authored_path = temp_authoring_path();
    if (!fftplugin::save_smd_authoring_document(authored_path, authored, &error_message)) {
        std::cerr << "save_authored_error=" << error_message << "\n";
        return 1;
    }
    const auto reloaded_authored = fftplugin::load_smd_authoring_document(authored_path, &error_message);
    if (!reloaded_authored.has_value()) {
        std::cerr << "reload_authored_error=" << error_message << "\n";
        return 1;
    }

    // .fftauth byte-identity gate: serialize → load → re-serialize must
    // produce identical bytes. Catches any drift in the authoring codec.
    const auto authored_bytes_a = fftplugin::serialize_smd_authoring_document(authored);
    const auto authored_bytes_b = fftplugin::serialize_smd_authoring_document(*reloaded_authored);
    if (authored_bytes_a != authored_bytes_b) {
        std::cerr << "fftauth_byte_identity_failed bytes_a=" << authored_bytes_a.size()
                  << " bytes_b=" << authored_bytes_b.size() << "\n";
        return 1;
    }
    std::cout << "fftauth_byte_identity=1\n";

    // Compiler determinism gate: compiling the same authored document twice
    // must produce identical SMD bytes. Pins the compilation pipeline.
    const auto compiled_bytes_a = fftplugin::serialize_smd_file(compiled.smd, &error_message);
    if (compiled_bytes_a.empty()) {
        std::cerr << "compiled_serialize_a_error=" << error_message << "\n";
        return 1;
    }
    const auto reloaded_compiled = fftplugin::compile_smd_authoring_document(*reloaded_authored);
    const auto compiled_bytes_b = fftplugin::serialize_smd_file(reloaded_compiled.smd, &error_message);
    if (compiled_bytes_b.empty()) {
        std::cerr << "compiled_serialize_b_error=" << error_message << "\n";
        return 1;
    }
    if (compiled_bytes_a != compiled_bytes_b) {
        std::cerr << "compiler_determinism_failed bytes_a=" << compiled_bytes_a.size()
                  << " bytes_b=" << compiled_bytes_b.size() << "\n";
        return 1;
    }
    std::cout << "compiler_determinism=1\n";

    bool exact_match = true;
    if (raw_smd->track_events.size() != compiled.smd.track_events.size()) {
        exact_match = false;
        std::cout << "track_count_mismatch raw=" << raw_smd->track_events.size()
                  << " compiled=" << compiled.smd.track_events.size() << "\n";
    }
    const size_t track_count = std::min(raw_smd->track_events.size(), compiled.smd.track_events.size());
    for (size_t track_index = 0; track_index < track_count; ++track_index) {
        const auto& raw_track = raw_smd->track_events[track_index];
        const auto& compiled_track = compiled.smd.track_events[track_index];
        if (raw_track.size() != compiled_track.size()) {
            exact_match = false;
            std::cout << "track " << track_index
                      << " event_count_mismatch raw=" << raw_track.size()
                      << " compiled=" << compiled_track.size() << "\n";
        }
        const size_t event_count = std::min(raw_track.size(), compiled_track.size());
        for (size_t event_index = 0; event_index < event_count; ++event_index) {
            if (same_event(raw_track[event_index], compiled_track[event_index])) {
                continue;
            }
            exact_match = false;
            std::cout << "track " << track_index
                      << " first_event_diff index=" << event_index << "\n";
            std::cout << "  raw:      " << describe_event(raw_track[event_index]) << "\n";
            std::cout << "  compiled: " << describe_event(compiled_track[event_index]) << "\n";
            break;
        }
    }
    std::cout << "event_stream_exact=" << (exact_match ? 1 : 0) << "\n";

    fftplugin::FFTFilePlaybackEngine raw_engine;
    auto waveset_result = raw_engine.load_waveset_path(waveset_path);
    if (!waveset_result.ok) {
        std::cerr << "raw_waveset_error=" << waveset_result.message << "\n";
        return 1;
    }
    auto raw_result = raw_engine.load_smd_path(smd_path);
    if (!raw_result.ok) {
        std::cerr << "raw_smd_error=" << raw_result.message << "\n";
        return 1;
    }

    fftplugin::FFTFilePlaybackEngine authored_engine;
    waveset_result = authored_engine.load_waveset_path(waveset_path);
    if (!waveset_result.ok) {
        std::cerr << "authored_waveset_error=" << waveset_result.message << "\n";
        return 1;
    }
    if (!authored_engine.load_compiled_smd_document(compiled, smd_path, &error_message)) {
        std::cerr << "compiled_load_error=" << error_message << "\n";
        return 1;
    }

    fftplugin::FFTFilePlaybackEngine reloaded_engine;
    waveset_result = reloaded_engine.load_waveset_path(waveset_path);
    if (!waveset_result.ok) {
        std::cerr << "reloaded_waveset_error=" << waveset_result.message << "\n";
        return 1;
    }
    if (!reloaded_engine.load_compiled_smd_document(reloaded_compiled, smd_path, &error_message)) {
        std::cerr << "reloaded_compiled_load_error=" << error_message << "\n";
        return 1;
    }

    const auto raw_pcm = render_pcm(raw_engine, seconds, block_size);
    const auto authored_pcm = render_pcm(authored_engine, seconds, block_size);
    const auto reloaded_pcm = render_pcm(reloaded_engine, seconds, block_size);

    const size_t compared_samples = std::min(raw_pcm.size(), authored_pcm.size());
    size_t first_diff = compared_samples;
    for (size_t sample_index = 0; sample_index < compared_samples; ++sample_index) {
        if (raw_pcm[sample_index] != authored_pcm[sample_index]) {
            first_diff = sample_index;
            break;
        }
    }

    const bool pcm_match = raw_pcm.size() == authored_pcm.size() && first_diff == compared_samples;
    std::cout << "pcm_match=" << (pcm_match ? 1 : 0) << "\n";
    if (!pcm_match) {
        std::cout << "raw_samples=" << raw_pcm.size()
                  << " compiled_samples=" << authored_pcm.size() << "\n";
        if (first_diff != compared_samples) {
            std::cout << "first_pcm_diff_sample=" << first_diff
                      << " raw=" << raw_pcm[first_diff]
                      << " compiled=" << authored_pcm[first_diff] << "\n";
        }
    }

    const size_t reloaded_compared_samples = std::min(raw_pcm.size(), reloaded_pcm.size());
    size_t reloaded_first_diff = reloaded_compared_samples;
    for (size_t sample_index = 0; sample_index < reloaded_compared_samples; ++sample_index) {
        if (raw_pcm[sample_index] != reloaded_pcm[sample_index]) {
            reloaded_first_diff = sample_index;
            break;
        }
    }
    const bool reloaded_pcm_match =
        raw_pcm.size() == reloaded_pcm.size() && reloaded_first_diff == reloaded_compared_samples;
    std::cout << "reloaded_pcm_match=" << (reloaded_pcm_match ? 1 : 0) << "\n";
    if (!reloaded_pcm_match) {
        std::cout << "reloaded_samples=" << reloaded_pcm.size() << "\n";
        if (reloaded_first_diff != reloaded_compared_samples) {
            std::cout << "first_reloaded_pcm_diff_sample=" << reloaded_first_diff
                      << " raw=" << raw_pcm[reloaded_first_diff]
                      << " reloaded=" << reloaded_pcm[reloaded_first_diff] << "\n";
        }
    }

    return (pcm_match && reloaded_pcm_match) ? 0 : 2;
}
