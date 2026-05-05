from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CSV_PATH = ROOT / "data" / "fft_gm_mapping_rules.csv"
INC_PATH = ROOT / "src" / "fft_gm_mapping_rules_generated.inc"


def vec_string_literal(values: list[str]) -> str:
    if not values:
        return "{}"
    return "{ " + ", ".join(f'"{value}"' for value in values) + " }"


def vec_int_literal(values: list[str]) -> str:
    parsed = [value.strip() for value in values if value.strip()]
    if not parsed:
        return "{}"
    return "{ " + ", ".join(str(int(value)) for value in parsed) + " }"


def main() -> None:
    with CSV_PATH.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))

    lines = []
    for row in rows:
        fallback_groups = [item.strip() for item in row["fallback_groups"].split("|") if item.strip()]
        forbidden = [item.strip() for item in row["forbidden_played_sample_ids"].split("|") if item.strip()]
        notes = row.get("notes", "").replace("\\", "\\\\").replace('"', '\\"')
        primary = row["primary_group"].replace("\\", "\\\\").replace('"', '\\"')
        lines.append(
            "    FFTMidiMappingRule {"
            f".gm_program = {int(row['gm_program'])}, "
            f'.primary_group = "{primary}", '
            f".fallback_groups = {vec_string_literal(fallback_groups)}, "
            f".forbidden_played_sample_ids = {vec_int_literal(forbidden)}, "
            f'.notes = "{notes}"'
            " },"
        )
    INC_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} rows to {INC_PATH}")


if __name__ == "__main__":
    main()
