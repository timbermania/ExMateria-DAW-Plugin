#include <algorithm>
#include <map>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "fft_plugin/fft_smd_authoring_codec.h"
#include "fft_plugin/fft_smd_authoring_model.h"
#include "fft_plugin/fft_smd_file.h"

namespace {

using namespace fftplugin;

bool fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
}

void require_or_die(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
        std::exit(1);
    }
}

struct NoteSig {
    int32_t start = 0;
    int32_t duration = 0;
    int32_t key = 0;

    friend bool operator<(const NoteSig& lhs, const NoteSig& rhs) {
        return std::tie(lhs.start, lhs.duration, lhs.key) <
            std::tie(rhs.start, rhs.duration, rhs.key);
    }

    friend bool operator==(const NoteSig& lhs, const NoteSig& rhs) {
        return lhs.start == rhs.start && lhs.duration == rhs.duration && lhs.key == rhs.key;
    }
};

std::vector<NoteSig> note_sigs_from_authored(const FFTSmdAuthoringDocument& doc, int32_t cap) {
    std::vector<NoteSig> notes;
    for (const auto& part : doc.parts) {
        if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
            for (const auto& note : part.poly_track.notes) {
                if (note.relative_key < 0 || note.relative_key >= 12 || note.start_tick >= cap) {
                    continue;
                }
                notes.push_back(NoteSig {
                    .start = note.start_tick,
                    .duration = std::min(note.total_ticks, cap - note.start_tick),
                    .key = note.relative_key,
                });
            }
        } else {
            for (const auto& span : part.raw_track.spans) {
                if (span.relative_key < 0 || span.relative_key >= 12 || span.start_tick >= cap) {
                    continue;
                }
                notes.push_back(NoteSig {
                    .start = span.start_tick,
                    .duration = std::min(span.total_ticks, cap - span.start_tick),
                    .key = span.relative_key,
                });
            }
        }
    }
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    return notes;
}

std::vector<NoteSig> note_sigs_from_imported_smd_without_control(const FFTSmdFile& smd, int32_t cap) {
    FFTSmdFile without_control = smd;
    if (!without_control.track_events.empty()) {
        without_control.track_events.erase(without_control.track_events.begin());
        without_control.track_count = static_cast<int32_t>(without_control.track_events.size());
    }
    return note_sigs_from_authored(import_smd_authoring_document(without_control), cap);
}

// Only flags the legacy 0x91 (Loop) opcode that the unbounded-target
// compile path used to emit and which the engine plays as silence.
// 0x98 (Repeat) and 0x99 (Coda) are now intentional — emitted by the
// loop roller — and verified to play correctly via the smoke gate.
bool has_loop_opcode(const FFTSmdFile& smd) {
    for (const auto& track : smd.track_events) {
        for (const auto& event : track) {
            const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&event);
            if (opcode == nullptr) continue;
            if (opcode->opcode == 0x91) return true;
        }
    }
    return false;
}

bool run_duration_packing_test() {
    FFTSmdAuthoringDocument doc;
    doc.initial_tempo = 120;
    doc.initial_volume = 127;
    doc.song_title = "duration-test";

    FFTSmdAuthoringPart note_part;
    note_part.kind = FFTSmdAuthoringPartKind::raw_track;
    note_part.name = "note";
    note_part.raw_track.total_ticks = 190;
    note_part.raw_track.spans.push_back(FFTSmdAuthoredSpan {
        .start_tick = 0,
        .total_ticks = 190,
        .base_ticks = 190,
        .velocity_hint = 90,
        .relative_key = 0,
    });
    doc.parts.push_back(note_part);

    FFTSmdAuthoringPart rest_part;
    rest_part.kind = FFTSmdAuthoringPartKind::raw_track;
    rest_part.name = "rest";
    rest_part.raw_track.total_ticks = 255;
    rest_part.raw_track.spans.push_back(FFTSmdAuthoredSpan {
        .start_tick = 0,
        .total_ticks = 255,
        .base_ticks = 255,
        .velocity_hint = 90,
        .relative_key = 13,
    });
    doc.parts.push_back(rest_part);

    auto compiled = compile_smd_authoring_document(doc);
    if (compiled.smd.track_events.size() < 2) {
        return fail("duration packing produced too few tracks");
    }
    bool saw_note_fermata_shape = false;
    for (size_t i = 0; i + 1 < compiled.smd.track_events[0].size(); ++i) {
        const auto* note = std::get_if<FFTSmdNoteEvent>(&compiled.smd.track_events[0][i]);
        const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&compiled.smd.track_events[0][i + 1]);
        if (note != nullptr && opcode != nullptr &&
            note->relative_key == 0 && note->delta_time == 144 &&
            opcode->opcode == 0x81 && opcode->params == std::vector<int32_t> {46}) {
            saw_note_fermata_shape = true;
            break;
        }
    }
    if (!saw_note_fermata_shape) {
        return fail("190-tick note was not packed as 144-tick note plus 46-tick fermata");
    }
    bool saw_rest_shape = false;
    for (const auto& event : compiled.smd.track_events[1]) {
        const auto* opcode = std::get_if<FFTSmdOpcodeEvent>(&event);
        if (opcode != nullptr && opcode->opcode == 0x80 && opcode->params == std::vector<int32_t> {255}) {
            saw_rest_shape = true;
            break;
        }
    }
    if (!saw_rest_shape) {
        return fail("255-tick rest was not packed as 0x80 FF");
    }

    std::string error;
    const auto bytes = serialize_smd_file(compiled.smd, &error);
    if (bytes.empty()) {
        return fail("duration packing serialization failed: " + error);
    }

    const auto parsed = parse_smd_bytes(bytes, &error);
    if (!parsed.has_value()) {
        return fail("duration packing reparse failed: " + error);
    }
    const auto imported = import_smd_authoring_document(*parsed);
    if (imported.parts.size() < 2) {
        return fail("duration packing imported too few parts");
    }
    if (imported.parts[0].raw_track.spans.empty() ||
        imported.parts[0].raw_track.spans[0].total_ticks != 190) {
        return fail("190-tick note did not round-trip exactly");
    }
    if (imported.parts[1].raw_track.spans.empty() ||
        imported.parts[1].raw_track.spans[0].total_ticks != 255 ||
        imported.parts[1].raw_track.spans[0].relative_key != 13) {
        return fail("255-tick rest did not round-trip exactly");
    }

    // Direct (non-table) note delta-times are now legal — the codec
    // round-trips them via the literal-byte form (`key*19 + 0` followed
    // by an extra delta byte). Previously the serializer refused; now
    // we assert it accepts and the resulting bytes parse back cleanly.
    // (The compiler still prefers packing into table-aligned chunks per
    // the saw_note_fermata_shape check above; this just covers the case
    // where a hand-built or imported SMD lands a non-table delta.)
    FFTSmdFile literal_delta;
    literal_delta.song_title = "literal-delta";
    literal_delta.initial_tempo = 120;
    literal_delta.track_events.push_back({
        FFTSmdNoteEvent {.velocity = 90, .relative_key = 0, .delta_time = 190},
        FFTSmdOpcodeEvent {.opcode = 0x90, .params = {}},
    });
    literal_delta.track_count = 1;
    const auto literal_bytes = serialize_smd_file(literal_delta, &error);
    if (literal_bytes.empty()) {
        return fail("serializer rejected legal 190-tick literal-delta note: " + error);
    }
    const auto literal_parsed = parse_smd_bytes(literal_bytes, &error);
    if (!literal_parsed.has_value() || literal_parsed->track_events.empty()) {
        return fail("190-tick literal-delta note did not parse back");
    }
    const auto* round_tripped_note =
        std::get_if<FFTSmdNoteEvent>(&literal_parsed->track_events[0][0]);
    if (round_tripped_note == nullptr || round_tripped_note->delta_time != 190) {
        return fail("190-tick literal-delta note did not round-trip its delta_time");
    }

    return true;
}

bool run_aerith_regression(const std::string& path, const std::string& out_path,
                           size_t target_bytes) {
    std::string error;
    const auto authored = load_smd_authoring_document(path, &error);
    if (!authored.has_value()) {
        return fail("could not load Aerith authoring document: " + error);
    }

    // Polyphony analysis: gather every note as an interval, sweep
    // ticks to find max simultaneous voices and the tick at which
    // peak polyphony occurs.
    {
        struct NoteIv { int32_t start, end; };
        std::vector<NoteIv> ivs;
        for (const auto& part : authored->parts) {
            if (part.kind == FFTSmdAuthoringPartKind::poly_track) {
                for (const auto& n : part.poly_track.notes) {
                    if (n.relative_key < 0 || n.relative_key >= 12) continue;
                    ivs.push_back({n.start_tick, n.start_tick + n.total_ticks});
                }
            } else {
                for (const auto& s : part.raw_track.spans) {
                    if (s.relative_key < 0 || s.relative_key >= 12) continue;
                    ivs.push_back({s.start_tick, s.start_tick + s.total_ticks});
                }
            }
        }
        // Sweep using tick-event sort.
        struct Ev { int32_t tick; int32_t delta; };
        std::vector<Ev> evs;
        evs.reserve(ivs.size() * 2);
        for (auto& iv : ivs) {
            evs.push_back({iv.start, +1});
            evs.push_back({iv.end, -1});
        }
        std::sort(evs.begin(), evs.end(),
            [](const Ev& a, const Ev& b) {
                if (a.tick != b.tick) return a.tick < b.tick;
                return a.delta > b.delta;  // process +1 before -1 at same tick
            });
        int32_t cur = 0, peak = 0, peak_tick = 0;
        for (auto& e : evs) {
            cur += e.delta;
            if (cur > peak) { peak = cur; peak_tick = e.tick; }
        }
        std::cerr << "[polyphony] total source notes=" << ivs.size()
                  << "  peak_simultaneous=" << peak
                  << "  peak_at_tick=" << peak_tick << "\n";
        // Histogram of how many ticks the music spends at each polyphony level.
        std::map<int32_t, int64_t> hist;
        cur = 0;
        int32_t prev_tick = evs.empty() ? 0 : evs.front().tick;
        for (auto& e : evs) {
            if (e.tick > prev_tick && cur > 0) hist[cur] += (e.tick - prev_tick);
            cur += e.delta;
            prev_tick = e.tick;
        }
        std::cerr << "[polyphony] ticks_at_each_level: ";
        for (auto& [level, ticks] : hist) {
            std::cerr << level << ":" << ticks << " ";
        }
        std::cerr << "\n";
    }

    FFTSmdGameCompileBudget budget;
    budget.target_bytes = target_bytes;
    const auto result = compile_smd_authoring_document_for_game(*authored, budget);
    if (!result.ok) {
        return fail("Aerith game compile failed: " + result.report.error);
    }
    // Always print pack + roll reports (helpful even when later checks fail).
    {
        const auto& gar = result.report.global_assign_report;
        if (gar.lanes_in > 0) {
            std::cerr << "[global-assign] lanes_in=" << gar.lanes_in
                      << " lanes_out=" << gar.lanes_out
                      << " notes=" << gar.notes_assigned
                      << " free=" << gar.free_assignments
                      << " state_bytes=" << gar.state_flip_bytes
                      << " ok=" << gar.ok << "\n";
            if (!gar.error.empty()) {
                std::cerr << "[global-assign]   error: " << gar.error << "\n";
            }
        }
        const auto& pr = result.report.pack_report;
        if (pr.lanes_in > 0) {
            std::cerr << "[packer] lanes_in=" << pr.lanes_in
                      << " lanes_out=" << pr.lanes_out
                      << " absorbed=" << pr.lanes_fully_absorbed
                      << " notes_packed=" << pr.notes_packed
                      << " state_bytes=" << pr.state_change_bytes_added << "\n";
            for (const auto& m : pr.merges) {
                std::cerr << "[merge] guest_lane=" << m.guest_lane_index
                          << " -> host_lane=" << m.host_lane_index
                          << " notes=" << m.notes_moved
                          << " ticks=[" << m.guest_min_tick << ".." << m.guest_max_tick << ")"
                          << " blocks=" << m.blocks
                          << " state_bytes=" << m.state_bytes_added << "\n";
            }
        }
        const auto& rr = result.report.roll_report;
        if (!rr.per_track.empty()) {
            std::cerr << "[roll] total_collapses=" << rr.total_collapses
                      << " bytes_saved=" << rr.total_bytes_saved << "\n";
            for (const auto& tr : rr.per_track) {
                if (tr.collapses == 0) continue;
                std::cerr << "[roll]   track=" << tr.track_index
                          << " collapses=" << tr.collapses
                          << " bytes " << tr.bytes_before
                          << " -> " << tr.bytes_after << "\n";
            }
        }
    }

    const auto bytes = serialize_smd_file(result.compiled.smd, &error);
    if (bytes.empty()) {
        return fail("Aerith serialization failed: " + error);
    }
    if (bytes.size() > target_bytes) {
        return fail("Aerith export exceeded " + std::to_string(target_bytes) + " bytes");
    }
    if (result.compiled.smd.track_count > 24) {
        return fail("Aerith export exceeded 24 tracks");
    }
    if (has_loop_opcode(result.compiled.smd)) {
        return fail("Aerith targeted export still contains generated loop opcodes");
    }
    if (!out_path.empty()) {
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const auto reparsed = parse_smd_bytes(bytes, &error);
    if (!reparsed.has_value()) {
        return fail("Aerith exported SMD did not parse: " + error);
    }
    // Byte-identity gate: parse → re-serialize must round-trip exactly.
    // Catches any drift in the SMD codec across refactors.
    const auto reserialized = serialize_smd_file(*reparsed, &error);
    if (reserialized.empty()) {
        return fail("Aerith re-serialization failed: " + error);
    }
    if (reserialized != bytes) {
        return fail("Aerith SMD codec is not byte-identical on parse/re-serialize round-trip");
    }
    const int32_t cap = result.report.trim_tick_cap;
    if (cap <= 0) {
        return fail("Aerith targeted export did not report a positive trim cap");
    }

    const auto source_notes = note_sigs_from_authored(*authored, cap);
    const auto imported_notes = note_sigs_from_imported_smd_without_control(*reparsed, cap);
    if (imported_notes.empty()) {
        return fail("Aerith reimport produced no musical notes");
    }
    int phantom = 0;
    for (const auto& note : imported_notes) {
        if (!std::binary_search(source_notes.begin(), source_notes.end(), note)) {
            if (phantom < 5) {
                std::cerr << "  PHANTOM start=" << note.start
                          << " duration=" << note.duration
                          << " key=" << note.key << "\n";
            }
            phantom += 1;
        }
    }
    if (phantom > 0) {
        return fail("Aerith reimport has " + std::to_string(phantom) +
                    " notes not in source (first " +
                    std::to_string(std::min(phantom, 5)) + " printed above)");
    }

    std::cout << "duration_packing=ok\n";
    std::cout << "aerith_export=ok\n";
    std::cout << "bytes=" << bytes.size() << "\n";
    std::cout << "tracks=" << result.compiled.smd.track_count << "\n";
    std::cout << "trim_tick_cap=" << cap << "\n";

    const auto& pr = result.report.pack_report;
    if (pr.lanes_in > 0) {
        std::cout << "[packer] lanes_in=" << pr.lanes_in
                  << " lanes_out=" << pr.lanes_out
                  << " absorbed=" << pr.lanes_fully_absorbed
                  << " notes_packed=" << pr.notes_packed
                  << " state_bytes=" << pr.state_change_bytes_added << "\n";
        for (const auto& m : pr.merges) {
            std::cout << "[merge] guest_lane=" << m.guest_lane_index
                      << " -> host_lane=" << m.host_lane_index
                      << " notes=" << m.notes_moved
                      << " ticks=[" << m.guest_min_tick << ".." << m.guest_max_tick << ")"
                      << " blocks=" << m.blocks
                      << " state_bytes=" << m.state_bytes_added << "\n";
        }
    }
    if (!out_path.empty()) {
        std::cout << "wrote=" << out_path << "\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string aerith_path = argc > 1 ? argv[1] : "/home/curry/Downloads/aerith.5.fftauth";
    const std::string out_path = argc > 2 ? argv[2] : "";
    const size_t target_bytes = argc > 3 ? static_cast<size_t>(std::atoll(argv[3])) : 2048;
    require_or_die(run_duration_packing_test(), "duration packing regression failed");
    require_or_die(run_aerith_regression(aerith_path, out_path, target_bytes), "Aerith regression failed");
    return 0;
}
