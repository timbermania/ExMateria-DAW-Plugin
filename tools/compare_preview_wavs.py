#!/usr/bin/env python3
import argparse
import math
import struct
import wave


def load_wav(path: str) -> list[int]:
    with wave.open(path, "rb") as wav:
        if wav.getnchannels() != 2:
            raise ValueError(f"{path}: expected stereo WAV")
        if wav.getsampwidth() != 2:
            raise ValueError(f"{path}: expected 16-bit WAV")
        frames = wav.readframes(wav.getnframes())
    count = len(frames) // 2
    return list(struct.unpack("<" + "h" * count, frames))


def rms(samples: list[int]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(s * s for s in samples) / len(samples))


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two stereo preview-note WAVs.")
    parser.add_argument("reference")
    parser.add_argument("candidate")
    args = parser.parse_args()

    ref = load_wav(args.reference)
    cand = load_wav(args.candidate)
    n = min(len(ref), len(cand))
    ref = ref[:n]
    cand = cand[:n]
    diffs = [abs(a - b) for a, b in zip(ref, cand)]

    print(f"samples={n}")
    print(f"ref_rms={rms(ref):.3f}")
    print(f"cand_rms={rms(cand):.3f}")
    print(f"mean_abs_diff={sum(diffs) / n:.3f}" if n else "mean_abs_diff=0.000")
    print(f"max_abs_diff={max(diffs) if diffs else 0}")
    print(f"sample_exact={'yes' if diffs and max(diffs) == 0 else 'no'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
