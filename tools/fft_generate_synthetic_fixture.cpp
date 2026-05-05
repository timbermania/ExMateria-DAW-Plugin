// Generates tests/fixtures/synthetic/tiny.fftauth — a small, ROM-free
// authoring document used by the byte-identity gate. Run once (or whenever
// the fixture intentionally changes) and commit the output. Re-running with
// the same source produces byte-identical bytes (the codec is deterministic).
//
// Usage: fft_generate_synthetic_fixture <output_path>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_file.h"

namespace {

using fftplugin::FFTSmdAuthoredOpcode;
using fftplugin::FFTSmdAuthoredPolyNote;
using fftplugin::FFTSmdAuthoredPolyTrack;
using fftplugin::FFTSmdAuthoredSpan;
using fftplugin::FFTSmdAuthoredTrack;
using fftplugin::FFTSmdAuthoringDocument;
using fftplugin::FFTSmdAuthoringPart;
using fftplugin::FFTSmdAuthoringPartKind;
using fftplugin::FFTSmdOpcodeEvent;

FFTSmdAuthoredOpcode make_opcode(int32_t tick, int32_t order, int32_t op, std::vector<int32_t> params) {
    FFTSmdAuthoredOpcode out;
    out.tick = tick;
    out.stack_order = order;
    out.enabled = true;
    out.exact_timing = false;
    out.opcode.opcode = op;
    out.opcode.params = std::move(params);
    return out;
}

FFTSmdAuthoredSpan make_span(int32_t start, int32_t total, int32_t key, int32_t velocity = 100) {
    FFTSmdAuthoredSpan span;
    span.start_tick = start;
    span.total_ticks = total;
    span.base_ticks = total;
    span.velocity_hint = velocity;
    span.relative_key = key;  // 0-11 note, 12 tie, 13 rest
    return span;
}

FFTSmdAuthoredPolyNote make_poly_note(int32_t start, int32_t total, int32_t key, int32_t velocity = 100) {
    FFTSmdAuthoredPolyNote note;
    note.start_tick = start;
    note.total_ticks = total;
    note.base_ticks = total;
    note.velocity_hint = velocity;
    note.relative_key = key;
    return note;
}

// Build a deterministic 2-part document exercising a representative slice
// of the opcode + span vocabulary the SMD codec needs to round-trip.
FFTSmdAuthoringDocument build_synthetic_document() {
    FFTSmdAuthoringDocument doc;
    doc.format_version = 4;
    doc.initial_tempo = 50;       // ~58 BPM via fft_tempo_to_bpm
    doc.initial_volume = 100;
    doc.assoc_wds_id = 0;
    doc.song_title = "synthetic";

    // Part 0: raw track with a rich opcode set + 4 quarter-note spans (PPQ=48).
    {
        FFTSmdAuthoringPart part;
        part.kind = FFTSmdAuthoringPartKind::raw_track;
        part.name = "RawSweep";

        FFTSmdAuthoredTrack track;
        track.total_ticks = 192;
        track.track_transposition = 0;

        // Spans: note key 0, rest, note key 4, note key 7 — each 48 ticks.
        track.spans.push_back(make_span(0,   48, 0));
        track.spans.push_back(make_span(48,  48, 13));  // rest
        track.spans.push_back(make_span(96,  48, 4));
        track.spans.push_back(make_span(144, 48, 7));

        // Opcode coverage: instrument, octave, dynamics, pan, reverb on/off.
        track.opcodes.push_back(make_opcode(0,   0, 0xAC, {1}));   // Instrument
        track.opcodes.push_back(make_opcode(0,   1, 0x94, {2}));   // Octave
        track.opcodes.push_back(make_opcode(0,   2, 0xE0, {110})); // Dynamics
        track.opcodes.push_back(make_opcode(0,   3, 0xE8, {64}));  // Pan (center)
        track.opcodes.push_back(make_opcode(0,   4, 0xBA, {}));    // ReverbOn
        track.opcodes.push_back(make_opcode(96,  0, 0xBB, {}));    // ReverbOff
        track.opcodes.push_back(make_opcode(96,  1, 0x95, {}));    // RaiseOctave

        part.raw_track = std::move(track);
        doc.parts.push_back(std::move(part));
    }

    // Part 1: poly track with two overlapping notes, instrument change.
    {
        FFTSmdAuthoringPart part;
        part.kind = FFTSmdAuthoringPartKind::poly_track;
        part.name = "PolyPair";

        FFTSmdAuthoredPolyTrack track;
        track.total_ticks = 192;
        track.track_transposition = 0;

        track.notes.push_back(make_poly_note(0, 96, 0));   // C
        track.notes.push_back(make_poly_note(0, 96, 7));   // G  (overlap)
        track.notes.push_back(make_poly_note(96, 96, 4));  // E

        track.opcodes.push_back(make_opcode(0, 0, 0xAC, {2}));   // Instrument
        track.opcodes.push_back(make_opcode(0, 1, 0xE0, {100})); // Dynamics

        part.poly_track = std::move(track);
        doc.parts.push_back(std::move(part));
    }

    // Legacy mirror — deserialize_authored_document_impl rebuilds `tracks`
    // from the raw-track subset of `parts` on load; mirror that here.
    for (const auto& part : doc.parts) {
        if (part.kind == FFTSmdAuthoringPartKind::raw_track) {
            doc.tracks.push_back(part.raw_track);
        }
    }
    // Deserializer also overwrites track_count with the compiled value, so
    // pre-compute it here or the first/second serialize differ on track_count.
    doc.track_count = fftplugin::compile_smd_authoring_document(doc).smd.track_count;

    return doc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <output_path>\n";
        return 1;
    }
    const std::string output_path = argv[1];

    const auto doc = build_synthetic_document();
    std::string error;
    if (!fftplugin::save_smd_authoring_document(output_path, doc, &error)) {
        std::cerr << "save failed: " << error << "\n";
        return 1;
    }

    // Sanity-check that the produced bytes round-trip cleanly through the
    // codec — fail loud at generation time, not later in the gate.
    const auto reloaded = fftplugin::load_smd_authoring_document(output_path, &error);
    if (!reloaded.has_value()) {
        std::cerr << "reload failed: " << error << "\n";
        return 1;
    }
    const auto bytes_a = fftplugin::serialize_smd_authoring_document(doc);
    const auto bytes_b = fftplugin::serialize_smd_authoring_document(*reloaded);
    if (bytes_a != bytes_b) {
        std::cerr << "synthetic fixture is not codec-stable\n";
        return 1;
    }

    std::cout << "wrote=" << output_path << " bytes=" << bytes_a.size() << "\n";
    return 0;
}
