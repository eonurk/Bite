#!/usr/bin/env python3
"""Reproducible per-file compressor benchmark with correctness and RSS checks."""

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TIME = "/usr/bin/time"


@dataclass(frozen=True)
class Codec:
    name: str
    executable: str
    compress: tuple[str, ...]
    decompress: tuple[str, ...]
    file_output: bool = False


CODECS = [
    Codec("bite-greedy", str(ROOT / "bin/bite"), ("c", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("gzip-1", "gzip", ("-1", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("gzip-9", "gzip", ("-9", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("zstd-1", "zstd", ("-q", "-1", "-c", "{input}"), ("-q", "-d", "-c", "{input}")),
    Codec("zstd-3", "zstd", ("-q", "-3", "-c", "{input}"), ("-q", "-d", "-c", "{input}")),
    Codec("zstd-6", "zstd", ("-q", "-6", "-c", "{input}"), ("-q", "-d", "-c", "{input}")),
    Codec("zstd-19", "zstd", ("-q", "-19", "-c", "{input}"), ("-q", "-d", "-c", "{input}")),
    Codec("brotli-1", "brotli", ("-q", "1", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("brotli-6", "brotli", ("-q", "6", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("brotli-11", "brotli", ("-q", "11", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("xz-1", "xz", ("-1", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("xz-6", "xz", ("-6", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("xz-9", "xz", ("-9", "-c", "{input}"), ("-d", "-c", "{input}")),
    Codec("lz4", "lz4", ("-q", "-c", "{input}"), ("-q", "-d", "-c", "{input}")),
]


def version(executable: str) -> str:
    for flag in ("--version", "-V"):
        result = subprocess.run([executable, flag], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip().splitlines()[0]
    return "version unavailable"


def run_timed(command: list[str], stdout_path: Path | None) -> tuple[float, int]:
    started = time.perf_counter()
    stdout = open(stdout_path, "wb") if stdout_path else subprocess.DEVNULL
    try:
        result = subprocess.run([TIME, "-l", *command], stdout=stdout, stderr=subprocess.PIPE, text=False)
    finally:
        if stdout_path:
            stdout.close()
    elapsed = time.perf_counter() - started
    stderr = result.stderr.decode(errors="replace")
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}\n{stderr}")
    match = re.search(r"(\d+)\s+maximum resident set size", stderr)
    return elapsed, int(match.group(1)) if match else 0


def command(codec: Codec, template: tuple[str, ...], source: Path, destination: Path) -> list[str]:
    return [codec.executable, *(part.format(input=source, output=destination) for part in template)]


def digest(path: Path) -> bytes:
    h = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(1024 * 1024):
            h.update(chunk)
    return h.digest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--codecs", nargs="*", help="optional codec-name subset")
    parser.add_argument("--label", default="baseline")
    args = parser.parse_args()
    selected = [c for c in CODECS if not args.codecs or c.name in args.codecs]
    if not selected:
        raise SystemExit("no codecs selected")
    missing = [c.executable for c in selected if not (Path(c.executable).exists() or shutil.which(c.executable))]
    if missing:
        raise SystemExit(f"missing tools: {', '.join(missing)}")
    files = sorted(p for p in (ROOT / "corpus" / "dev").rglob("*") if p.is_file() and p.name != "MANIFEST.json")
    if not files:
        raise SystemExit("development corpus is empty; run make corpus")

    output_dir = ROOT / "results" / args.label
    output_dir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "repeats": args.repeats,
        "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "tools": {c.name: version(c.executable) for c in selected},
        "files": [{"path": str(p.relative_to(ROOT / "corpus" / "dev")), "bytes": p.stat().st_size,
                   "sha256": digest(p).hex()} for p in files],
    }
    (output_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = []
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        for codec in selected:
            print(f"benchmarking {codec.name} ({len(files)} files, {args.repeats} repeats)", flush=True)
            for file_index, source in enumerate(files):
                encoded = temp / f"{file_index}.compressed"
                decoded = temp / f"{file_index}.decoded"
                encode_times, decode_times, encode_rss, decode_rss = [], [], [], []
                compressed_sizes = []
                for _ in range(args.repeats):
                    encoded.unlink(missing_ok=True)
                    decoded.unlink(missing_ok=True)
                    c_cmd = command(codec, codec.compress, source, encoded)
                    elapsed, rss = run_timed(c_cmd, None if codec.file_output else encoded)
                    encode_times.append(elapsed); encode_rss.append(rss); compressed_sizes.append(encoded.stat().st_size)
                    d_cmd = command(codec, codec.decompress, encoded, decoded)
                    elapsed, rss = run_timed(d_cmd, None if codec.file_output else decoded)
                    decode_times.append(elapsed); decode_rss.append(rss)
                    if source.stat().st_size != decoded.stat().st_size or digest(source) != digest(decoded):
                        raise RuntimeError(f"LOSSLESS FAILURE: {codec.name} on {source}")
                if len(set(compressed_sizes)) != 1:
                    raise RuntimeError(f"nondeterministic compressed size: {codec.name} on {source}")
                size = source.stat().st_size
                enc = statistics.median(encode_times)
                dec = statistics.median(decode_times)
                rows.append({
                    "codec": codec.name, "file": str(source.relative_to(ROOT / "corpus" / "dev")),
                    "original_size_bytes": size, "compressed_size_bytes": compressed_sizes[0],
                    "bits_per_byte": 8 * compressed_sizes[0] / size if size else 0,
                    "compression_ratio": size / compressed_sizes[0] if compressed_sizes[0] else 0,
                    "encode_time_seconds": enc, "decode_time_seconds": dec,
                    "encode_MB_per_second": size / 1_000_000 / enc if enc else 0,
                    "decode_MB_per_second": size / 1_000_000 / dec if dec else 0,
                    "peak_memory_encode": max(encode_rss), "peak_memory_decode": max(decode_rss),
                    "binary_size": Path(shutil.which(codec.executable) or codec.executable).stat().st_size,
                    "correct": True,
                })

    fields = list(rows[0])
    with open(output_dir / "per_file.csv", "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
    summaries = []
    for codec in selected:
        subset = [r for r in rows if r["codec"] == codec.name]
        original = sum(r["original_size_bytes"] for r in subset)
        compressed = sum(r["compressed_size_bytes"] for r in subset)
        enc_time = sum(r["encode_time_seconds"] for r in subset)
        dec_time = sum(r["decode_time_seconds"] for r in subset)
        summaries.append({
            "codec": codec.name, "original_size_bytes": original, "compressed_size_bytes": compressed,
            "bits_per_byte": 8 * compressed / original, "compression_ratio": original / compressed,
            "encode_time_seconds": enc_time, "decode_time_seconds": dec_time,
            "encode_MB_per_second": original / 1_000_000 / enc_time,
            "decode_MB_per_second": original / 1_000_000 / dec_time,
            "peak_memory_encode": max(r["peak_memory_encode"] for r in subset),
            "peak_memory_decode": max(r["peak_memory_decode"] for r in subset),
            "binary_size": subset[0]["binary_size"], "correct": all(r["correct"] for r in subset),
        })
    with open(output_dir / "summary.csv", "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=summaries[0]); writer.writeheader(); writer.writerows(summaries)
    print(f"wrote {output_dir / 'summary.csv'}")


if __name__ == "__main__":
    main()

