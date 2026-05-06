#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "fft_plugin/fft_plugin_processor_core.h"
#include "fft_plugin/fft_plugin_state.h"
#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_opcodes.h"

namespace {

constexpr int32_t kTempoOpcode = 0xA0;

struct TempoOpcodeRef {
    int32_t part_index;
    std::string part_name;
    int32_t authored_index;
    int32_t source_event_index;
    int32_t tick;
    int32_t value;
};

std::vector<TempoOpcodeRef> collect_tempo_opcodes(
    const fftplugin::FFTSmdAuthoringDocument& doc,
    const fftplugin::FFTSmdCompiledDocument& compiled
) {
    std::vector<TempoOpcodeRef> out;
    for (size_t pi = 0; pi < doc.parts.size(); ++pi) {
        const auto& part = doc.parts[pi];
        const auto& opcodes = part.kind == fftplugin::FFTSmdAuthoringPartKind::poly_track
            ? part.poly_track.opcodes
            : part.raw_track.opcodes;
        const auto& sei_map = pi < compiled.authored_opcode_source_indices.size()
            ? compiled.authored_opcode_source_indices[pi]
            : std::vector<int32_t>{};
        for (size_t oi = 0; oi < opcodes.size(); ++oi) {
            if (opcodes[oi].opcode.opcode == kTempoOpcode) {
                out.push_back(TempoOpcodeRef {
                    .part_index = static_cast<int32_t>(pi),
                    .part_name = part.name,
                    .authored_index = static_cast<int32_t>(oi),
                    .source_event_index = oi < sei_map.size() ? sei_map[oi] : -1,
                    .tick = opcodes[oi].tick,
                    .value = opcodes[oi].opcode.params.empty() ? -1 : opcodes[oi].opcode.params[0],
                });
            }
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: fft_tempo_edit_probe <waveset.wd> <music.fftauth-or-smd> [new_tempo_value]\n";
        return 1;
    }
    const std::string waveset_path = argv[1];
    const std::string music_path = argv[2];
    const int32_t new_value = argc > 3 ? std::stoi(argv[3]) : 200;

    fftplugin::FFTPluginProcessorCore processor;
    std::string err;
    if (!processor.prepare_to_play(fftplugin::FFTProcessSetup {
            .sample_rate = 44100.0,
            .max_block_size = 512,
            .output_channels = 2,
        }, &err)) {
        std::cerr << "prepare_error=" << err << "\n";
        return 1;
    }
    const auto wave_res = processor.load_waveset_path(waveset_path);
    if (!wave_res.ok) {
        std::cerr << "waveset_error=" << wave_res.message << "\n";
        return 1;
    }
    const auto music_res = processor.load_music_document_path(music_path);
    if (!music_res.ok) {
        std::cerr << "music_error=" << music_res.message << "\n";
        return 1;
    }

    const auto& state = processor.state();
    if (!state.smd_authoring.has_value()) {
        std::cerr << "no_authoring_document\n";
        return 1;
    }
    const auto& doc = *state.smd_authoring;

    std::cout << "music_path=" << music_path << "\n";
    std::cout << "parts=" << doc.parts.size() << "\n";
    std::cout << "initial_tempo_byte=" << doc.initial_tempo << "\n";
    std::cout << "engine_tempo_bpm_initial=" << processor.playback_engine().tempo_bpm() << "\n\n";

    const auto compiled = fftplugin::compile_smd_authoring_document(doc);
    auto tempos = collect_tempo_opcodes(doc, compiled);
    std::cout << "tempo_opcodes_found=" << tempos.size() << "\n";
    for (const auto& t : tempos) {
        std::cout << "  part=" << t.part_index
                  << " name=" << t.part_name
                  << " authored_index=" << t.authored_index
                  << " sei=" << t.source_event_index
                  << " tick=" << t.tick
                  << " value=" << t.value << "\n";
    }
    std::cout << "\n";

    int ok_count = 0;
    int fail_count = 0;
    for (const auto& t : tempos) {
        const double bpm_before = processor.playback_engine().tempo_bpm();
        std::string call_err;
        const bool ok = processor.set_track_tempo_opcode_value(
            t.part_index, t.source_event_index, new_value, &call_err);
        const double bpm_after = processor.playback_engine().tempo_bpm();
        std::cout << "edit"
                  << " part=" << t.part_index
                  << " sei=" << t.source_event_index
                  << " tick=" << t.tick
                  << " new_value=" << new_value
                  << " ok=" << (ok ? "true" : "false")
                  << " bpm_before=" << bpm_before
                  << " bpm_after=" << bpm_after
                  << " engine_changed=" << (bpm_before != bpm_after ? "yes" : "no");
        if (!ok) {
            std::cout << " err=\"" << call_err << "\"";
        }
        std::cout << "\n";
        if (ok) {
            ++ok_count;
        } else {
            ++fail_count;
        }
    }
    std::cout << "\nsummary ok=" << ok_count << " fail=" << fail_count << "\n";

    // Now: also write through to document.initial_tempo and re-load, to
    // confirm the symptom is that the SMD header byte (sourced from
    // document.initial_tempo) is what the engine reads at tick 0.
    if (state.smd_authoring.has_value()) {
        const double bpm_before = processor.playback_engine().tempo_bpm();
        auto mutated = *state.smd_authoring;
        mutated.initial_tempo = new_value;
        const auto reload_res = processor.create_music_document_from_authoring_document(
            music_path, std::move(mutated), "tempo probe rewrite");
        std::cout << "\nrewrite_initial_tempo: ok=" << (reload_res.ok ? "true" : "false")
                  << " bpm_before=" << bpm_before
                  << " bpm_after=" << processor.playback_engine().tempo_bpm()
                  << " engine_changed="
                  << (bpm_before != processor.playback_engine().tempo_bpm() ? "yes" : "no")
                  << "\n";
    }
    return 0;
}
