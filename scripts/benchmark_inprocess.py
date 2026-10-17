#!/usr/bin/env python3
"""Startup-free Bite benchmark: one process, one warm-up, repeated codec calls."""

import argparse
import csv
import hashlib
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "bin" / "bite-bench"
CODECS = {
    "bite-max-bwt": ("cmax", "d"),
    "bite-bwt": ("cbwt", "d"),
    "bite-bwt2": ("cbwt2", "d"),
    "bite-fast": ("cfast", "d"),
    "bite-link-split-delta-filter": ("clblinksdf", "d"),
    "bite-link-split-optimal": ("clblinksopt", "d"),
    "bite-link-split-optimal-iterated": ("clblinksopt2", "d"),
    "bite-link-split-optimal-distances": ("clblinksopt2d", "d"),
    "bite-link-split-optimal-distances-fused": ("clblinksopt2df", "d"),
    "bite-link-split-optimal-distances-shared": ("clblinksopt2dfs", "d"),
    "bite-link-split-optimal-distances-bucketed": ("clblinksopt2dfsb", "d"),
    "bite-link-split-optimal-compact-models": ("clblinksopt2dfsbc", "d"),
    "bite-link-split-optimal-wide-history": ("clblinksopt2dfsbw", "d"),
    "bite-link-split-optimal-wide-arithmetic": ("clblinksopt2dfsbwa", "d"),
    "bite-link-split-optimal-wide-arithmetic-order1": ("clblinksopt2dfsbwa1", "d"),
    "bite-link-split-optimal-wide-arithmetic-context": ("clblinksopt2dfsbwac", "d"),
    "bite-link-split-optimal-wide-arithmetic-static": ("clblinksopt2dfsbwas", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse": ("clblinksopt2dfsbwasp", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2": ("clblinksopt2dfsbwasp2", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-large-block": ("clblinksopt2dfsbwasp2b", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-whole-block": ("clblinksopt2dfsbwasp2b16", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-deep": ("clblinksopt2dfsbwasp2b16x", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-extreme": ("clblinksopt2dfsbwasp2b16x2", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-transform": ("clblinksopt2dfsbwasp2b16x2t", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-distance-context": ("clblinksopt2dfsbwasp2b16x2td", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-literal-bits": ("clblinksopt2dfsbwasp2b16x2tdl", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-matched-literals": ("clblinksopt2dfsbwasp2b16x2tdlm", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-mixed-matched-literals": ("clblinksopt2dfsbwasp2b16x2tdlmm", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-control-context": ("clblinksopt2dfsbwasp2b16x2tdlmmdc", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-rep-distances": ("clblinksopt2dfsbwasp2b16x2tdlmmdcr", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-rep-aware": ("clblinksopt2dfsbwasp2b16x2tdlmmdcrg", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-rep-aware-iterated": ("clblinksopt2dfsbwasp2b16x2tdlmmdcrg2", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-rep-beam": ("clblinksopt2dfsbwasp2b16x2tdlmmdcrb", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-small-alphabet-order4": ("clblinksopt2dfsbwasp2b16x2tdlmmdcra4", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-context-mix3": ("clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3", "d"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-fast": ("clblinksopt2dfsbwasp2b16x2tdlmmdcra4f", "d"),
    "bite-link-split-delta-workdec": ("clblinksdf", "dwork"),
    "bite-link-split-delta-slowdec": ("clblinksdf", "dslow"),
}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus-dir", type=Path, default=ROOT / "corpus" / "dev")
    parser.add_argument("--codecs", nargs="+", default=["bite-link-split-delta-filter"])
    parser.add_argument("--repeats", type=int, default=20)
    parser.add_argument("--label", default="inprocess")
    parser.add_argument("--aggregate-exclude", nargs="*", default=["binary_data/random.bin"],
                        help="measured path suffixes excluded from aggregate; pass empty to include all")
    args = parser.parse_args()
    if args.repeats < 1:
        raise SystemExit("repeats must be positive")
    unknown = sorted(set(args.codecs) - set(CODECS))
    if unknown:
        raise SystemExit(f"unknown in-process codecs: {', '.join(unknown)}")
    corpus_dir = args.corpus_dir if args.corpus_dir.is_absolute() else ROOT / args.corpus_dir
    corpus_dir = corpus_dir.resolve()
    files = sorted(path for path in corpus_dir.rglob("*")
                   if path.is_file() and path.name != "MANIFEST.json")
    if not files:
        raise SystemExit(f"corpus is empty: {corpus_dir}")
    if not RUNNER.exists():
        raise SystemExit("missing bin/bite-bench; run make")

    output_dir = ROOT / "results" / args.label
    output_dir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "benchmark_schema": 2,
        "aggregate_excluded_files": args.aggregate_exclude,
        "benchmark_kind": "in_process",
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "repeats": args.repeats,
        "warmups_per_file": 1,
        "clock": "CLOCK_MONOTONIC",
        "corpus_dir": str(corpus_dir),
        "runner_sha256": digest(RUNNER),
        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                             text=True).strip(),
        "files": [{"path": str(path.relative_to(corpus_dir)), "bytes": path.stat().st_size,
                   "sha256": digest(path)} for path in files],
        "codecs": {name: CODECS[name] for name in args.codecs},
    }
    (output_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    rows = []
    for codec_name in args.codecs:
        compress_mode, decompress_mode = CODECS[codec_name]
        command = [str(RUNNER), compress_mode, decompress_mode, str(args.repeats),
                   *(str(path) for path in files)]
        result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE)
        decoded_rows = list(csv.DictReader(result.stdout.splitlines(), delimiter="\t"))
        if len(decoded_rows) != len(files) or not all(row["correct"] == "1" for row in decoded_rows):
            raise RuntimeError(f"incomplete or incorrect runner output for {codec_name}")
        for row, source in zip(decoded_rows, files):
            size = int(row["original_size_bytes"])
            compressed = int(row["compressed_size_bytes"])
            encode_time = float(row["encode_time_seconds"])
            decode_time = float(row["decode_time_seconds"])
            rows.append({
                "codec": codec_name,
                "file": str(source.relative_to(corpus_dir)),
                "original_size_bytes": size,
                "compressed_size_bytes": compressed,
                "bits_per_byte": 8 * compressed / size if size else 0,
                "compression_ratio": size / compressed if compressed else 0,
                "encode_time_seconds": encode_time,
                "decode_time_seconds": decode_time,
                "encode_MB_per_second": size / 1_000_000 / encode_time if encode_time else 0,
                "decode_MB_per_second": size / 1_000_000 / decode_time if decode_time else 0,
                "in_aggregate": not any(
                    str(source.relative_to(corpus_dir)) == suffix or
                    str(source.relative_to(corpus_dir)).endswith("/" + suffix)
                    for suffix in args.aggregate_exclude),
                "correct": True,
            })

    with open(output_dir / "per_file.csv", "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    summaries = []
    for codec_name in args.codecs:
        measured = [row for row in rows if row["codec"] == codec_name]
        subset = [row for row in measured if row["in_aggregate"]]
        original = sum(row["original_size_bytes"] for row in subset)
        compressed = sum(row["compressed_size_bytes"] for row in subset)
        encode_time = sum(row["encode_time_seconds"] for row in subset)
        decode_time = sum(row["decode_time_seconds"] for row in subset)
        summaries.append({
            "codec": codec_name,
            "aggregate_files": len(subset),
            "excluded_files": len(measured) - len(subset),
            "original_size_bytes": original,
            "compressed_size_bytes": compressed,
            "bits_per_byte": 8 * compressed / original if original else 0,
            "compression_ratio": original / compressed if compressed else 0,
            "encode_time_seconds": encode_time,
            "decode_time_seconds": decode_time,
            "encode_MB_per_second": original / 1_000_000 / encode_time if encode_time else 0,
            "decode_MB_per_second": original / 1_000_000 / decode_time if decode_time else 0,
            "correct": all(row["correct"] for row in measured),
        })
    with open(output_dir / "summary.csv", "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    print(f"wrote {output_dir / 'summary.csv'}")


if __name__ == "__main__":
    main()
