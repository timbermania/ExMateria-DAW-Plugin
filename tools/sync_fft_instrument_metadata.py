from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CSV_PATH = ROOT / "data" / "fft_instrument_metadata.csv"
INC_PATH = ROOT / "src" / "fft_instrument_catalog_generated.inc"


def cpp_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def main() -> None:
    if not CSV_PATH.exists():
        raise SystemExit(f"Missing metadata CSV: {CSV_PATH}")

    with CSV_PATH.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))

    required = {
        "played_sample_id",
        "hex_id",
        "opcode_param",
        "name",
        "minor_group",
        "major_group",
        "waveset_dependency",
        "root_note_name",
        "root_midi_note",
        "root_octave",
        "is_null",
        "repeat_tail_score",
        "sustain_timbre_score",
        "long_hold_risk_score",
        "upper_mid_energy_ratio",
        "presence_energy_ratio",
        "high_freq_energy_ratio",
        "mean_centroid_hz",
    }
    missing = required.difference(rows[0].keys() if rows else set())
    if missing:
        raise SystemExit(f"CSV missing required columns: {sorted(missing)}")

    lines = []
    for row in rows:
        lines.append(
            "    FFTInstrumentCatalogEntry {"
            f".id = {int(row['played_sample_id'])}, "
            f".hex = {cpp_string(row['hex_id'])}, "
            f".opcode_param = {int(row['opcode_param'])}, "
            f".name = {cpp_string(row['name'])}, "
            f".minor_group = {cpp_string(row['minor_group'])}, "
            f".major_group = {cpp_string(row['major_group'])}, "
            f".waveset_dependency = {cpp_string(row['waveset_dependency'])}, "
            f".root_note_name = {cpp_string(row['root_note_name'])}, "
            f".root_midi_note = {int(row['root_midi_note']) if row['root_midi_note'] else -1}, "
            f".root_octave = {int(row['root_octave']) if row['root_octave'] else -1}, "
            f".is_null = {'true' if row['is_null'] == '1' else 'false'}, "
            f".repeat_tail_score = {float(row['repeat_tail_score']) if row['repeat_tail_score'] else -1.0}, "
            f".sustain_timbre_score = {float(row['sustain_timbre_score']) if row['sustain_timbre_score'] else -1.0}, "
            f".long_hold_risk_score = {float(row['long_hold_risk_score']) if row['long_hold_risk_score'] else -1.0}, "
            f".upper_mid_energy_ratio = {float(row['upper_mid_energy_ratio']) if row['upper_mid_energy_ratio'] else -1.0}, "
            f".presence_energy_ratio = {float(row['presence_energy_ratio']) if row['presence_energy_ratio'] else -1.0}, "
            f".high_freq_energy_ratio = {float(row['high_freq_energy_ratio']) if row['high_freq_energy_ratio'] else -1.0}, "
            f".mean_centroid_hz = {float(row['mean_centroid_hz']) if row['mean_centroid_hz'] else -1.0}"
            " },"
        )

    INC_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} rows to {INC_PATH}")


if __name__ == "__main__":
    main()
