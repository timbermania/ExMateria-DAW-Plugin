#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

#include "asset_paths.h"
#include "fft_plugin/fft_file_playback_engine.h"
#include "fft_plugin/fft_smd_file.h"
#include "fft_plugin/fft_smd_inspector.h"

int main(int argc, char** argv) {
    const std::string smd_path = argc > 1 ? argv[1] : fftplugin::asset_paths::default_smd();
    const std::string waveset_path = argc > 2 ? argv[2] : fftplugin::asset_paths::default_waveset();

    std::string error_message;
    const auto smd = fftplugin::load_smd_file(smd_path, &error_message);
    if (!smd.has_value()) {
        std::cerr << "error=" << error_message << "\n";
        return 1;
    }

    int32_t total_events = 0;
    int32_t total_notes = 0;
    int32_t total_opcodes = 0;
    for (const auto& track : smd->track_events) {
        total_events += static_cast<int32_t>(track.size());
        for (const auto& event : track) {
            if (std::holds_alternative<fftplugin::FFTSmdNoteEvent>(event)) {
                total_notes += 1;
            } else {
                total_opcodes += 1;
            }
        }
    }

    std::cout << "track_count=" << smd->track_count << "\n";
    std::cout << "title=" << smd->song_title << "\n";
    std::cout << "initial_tempo=" << smd->initial_tempo << "\n";
    std::cout << "initial_bpm=" << fftplugin::fft_tempo_to_bpm(smd->initial_tempo) << "\n";
    std::cout << "assoc_wds_id=" << smd->assoc_wds_id << "\n";
    std::cout << "total_events=" << total_events << "\n";
    std::cout << "total_notes=" << total_notes << "\n";
    std::cout << "total_opcodes=" << total_opcodes << "\n";
    std::cout << "\n[metadata]\n" << fftplugin::build_smd_metadata_text(*smd);
    std::cout << "\n[track_summary]\n" << fftplugin::build_smd_track_summary_text(*smd);
    std::cout << "\n[track0_events]\n" << fftplugin::build_smd_track_events_text(*smd, 0);

    const auto source_presentation = fftplugin::build_smd_song_presentation(
        *smd,
        fftplugin::FFTSmdPresentationMode::source);
    std::cout << "\n[source_presentation]\n";
    std::cout << "total_ticks=" << source_presentation.total_ticks << "\n";
    for (const auto& track : source_presentation.tracks) {
        std::cout << "track=" << track.track_index
                  << " notes=" << track.notes.size()
                  << " markers=" << track.markers.size()
                  << " total_ticks=" << track.total_ticks << "\n";
    }

    fftplugin::FFTFilePlaybackEngine engine;
    const auto waveset_result = engine.load_waveset_path(waveset_path);
    const auto smd_result = engine.load_smd_path(smd_path);
    std::cout << "\n[playback_load]\n";
    std::cout << "waveset_ok=" << waveset_result.ok << " msg=" << waveset_result.message << "\n";
    std::cout << "smd_ok=" << smd_result.ok << " msg=" << smd_result.message << "\n";
    if (waveset_result.ok && smd_result.ok) {
        const auto playback_presentation = engine.build_playback_song_presentation();
        std::cout << "\n[playback_presentation]\n";
        std::cout << "total_ticks=" << playback_presentation.total_ticks << "\n";
        std::cout << "conductor_markers=" << playback_presentation.conductor_markers.size() << "\n";
        for (const auto& track : playback_presentation.tracks) {
            std::cout << "track=" << track.track_index
                      << " notes=" << track.notes.size()
                      << " markers=" << track.markers.size()
                      << " total_ticks=" << track.total_ticks << "\n";
        }
    }

    if (!smd->track_events.empty() && !smd->track_events.front().empty()) {
        const auto& first = smd->track_events.front().front();
        if (std::holds_alternative<fftplugin::FFTSmdOpcodeEvent>(first)) {
            const auto& opcode = std::get<fftplugin::FFTSmdOpcodeEvent>(first);
            std::cout << "first_event_opcode=0x" << std::hex << opcode.opcode << std::dec << "\n";
            std::cout << "first_event_name=" << fftplugin::smd_opcode_name(static_cast<uint8_t>(opcode.opcode)) << "\n";
        }
    }
    return 0;
}
