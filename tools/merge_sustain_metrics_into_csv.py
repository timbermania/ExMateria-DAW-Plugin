from __future__ import annotations

import csv
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CSV_PATH = ROOT / "data" / "fft_instrument_metadata.csv"
JSON_PATH = ROOT.parents[1] / "fft-project" / "research" / "tools" / "sound_synth" / "sustain_loop_scores.json"

METRIC_COLUMNS = [
    "repeat_tail_score",
    "sustain_timbre_score",
    "long_hold_risk_score",
    "upper_mid_energy_ratio",
    "presence_energy_ratio",
    "high_freq_energy_ratio",
    "mean_centroid_hz",
]


def main() -> None:
    if not CSV_PATH.exists():
        raise SystemExit(f"Missing CSV: {CSV_PATH}")
    if not JSON_PATH.exists():
        raise SystemExit(f"Missing sustain metrics JSON: {JSON_PATH}")

    with CSV_PATH.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
        fieldnames = list(rows[0].keys()) if rows else []

    for column in METRIC_COLUMNS:
        if column not in fieldnames:
            fieldnames.append(column)

    data = json.loads(JSON_PATH.read_text(encoding="utf-8"))
    measurements = data.get("measurements", [])
    by_instrument = {int(row["instrument_id"]): row for row in measurements}

    for row in rows:
        measurement = by_instrument.get(int(row["played_sample_id"]))
        for column in METRIC_COLUMNS:
            row[column] = ""
        if measurement is None:
            continue
        for column in METRIC_COLUMNS:
            value = measurement.get(column)
            row[column] = "" if value is None else str(value)

    with CSV_PATH.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(f"Updated {CSV_PATH} from {JSON_PATH}")


if __name__ == "__main__":
    main()
