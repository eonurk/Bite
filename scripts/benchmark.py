#!/usr/bin/env python3
"""Reproducible per-file compressor benchmark with correctness and RSS checks."""

import argparse
import concurrent.futures
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
BENCHMARK_SCHEMA = 1


@dataclass(frozen=True)
class Codec:
    name: str
    executable: str
    compress: tuple[str, ...]
    decompress: tuple[str, ...]
    file_output: bool = False


CODECS = [
    Codec("bite-greedy", str(ROOT / "bin/bite"), ("c", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-lazy", str(ROOT / "bin/bite"), ("cl", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-block-greedy", str(ROOT / "bin/bite"), ("cb", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-block-lazy", str(ROOT / "bin/bite"), ("clb", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-huff-greedy", str(ROOT / "bin/bite"), ("cbh", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-huff-lazy", str(ROOT / "bin/bite"), ("clbh", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-link-greedy", str(ROOT / "bin/bite"), ("cblink", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
    Codec("bite-link-lazy", str(ROOT / "bin/bite"), ("clblink", "{input}", "{output}"), ("d", "{input}", "{output}"), True),
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


def is_external(codec_name: str) -> bool:
    return not codec_name.startswith("bite-")


def cache_fingerprint(platform: str, files: list[dict], tools: dict[str, str]) -> str:
    material = {
        "benchmark_schema": BENCHMARK_SCHEMA,
        "platform": platform,
        "files": files,
        "external_tools": {name: value for name, value in sorted(tools.items()) if is_external(name)},
    }
    return hashlib.sha256(json.dumps(material, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def truthy(value: object) -> bool:
    return value is True or str(value).lower() == "true"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", choices=("screen", "validate", "audit"), default="audit")
    parser.add_argument("--repeats", type=int)
    parser.add_argument("--codecs", nargs="*", help="optional codec-name subset")
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--baseline-cache", type=Path,
                        help="reuse fingerprint-matched external rows from a prior results directory")
    args = parser.parse_args()
    if args.repeats is None:
        args.repeats = {"screen": 1, "validate": 3, "audit": 3}[args.stage]
    if args.repeats < 1:
        raise SystemExit("repeats must be positive")
    if args.stage != "audit" and not args.codecs:
        raise SystemExit(f"--stage {args.stage} requires --codecs CANDIDATE PARENT [...]")
    selected = [c for c in CODECS if not args.codecs or c.name in args.codecs]
    unknown = sorted(set(args.codecs or ()) - {c.name for c in selected})
    if unknown:
        raise SystemExit(f"unknown codecs: {', '.join(unknown)}")
    if not selected:
        raise SystemExit("no codecs selected")
    missing = [c.executable for c in selected if not (Path(c.executable).exists() or shutil.which(c.executable))]
    if missing:
        raise SystemExit(f"missing tools: {', '.join(missing)}")
    files = sorted(p for p in (ROOT / "corpus" / "dev").rglob("*") if p.is_file() and p.name != "MANIFEST.json")
    if not files:
        raise SystemExit("development corpus is empty; run make corpus")

    platform = subprocess.check_output(["uname", "-a"], text=True).strip()
    with concurrent.futures.ThreadPoolExecutor() as executor:
        digests = list(executor.map(digest, files))
    file_metadata = [{"path": str(p.relative_to(ROOT / "corpus" / "dev")), "bytes": p.stat().st_size,
                      "sha256": value.hex()} for p, value in zip(files, digests)]
    source_digests = dict(zip(files, digests))
    tools = {c.name: version(c.executable) for c in selected}
    cached_rows = []
    cached_from = None
    if args.baseline_cache:
        cache_dir = args.baseline_cache.resolve()
        cached_metadata = json.loads((cache_dir / "metadata.json").read_text())
        if cached_metadata.get("platform") != platform or cached_metadata.get("files") != file_metadata:
            raise SystemExit("baseline cache rejected: platform or corpus fingerprint changed")
        external_codecs = [c for c in CODECS if is_external(c.name) and c.name not in {s.name for s in selected}]
        current_external_tools = {c.name: version(c.executable) for c in external_codecs}
        for name, current_version in current_external_tools.items():
            if cached_metadata.get("tools", {}).get(name) != current_version:
                raise SystemExit(f"baseline cache rejected: tool version changed for {name}")
        with open(cache_dir / "per_file.csv", newline="") as handle:
            cached_rows = [r for r in csv.DictReader(handle) if r["codec"] in current_external_tools]
        expected_rows = len(external_codecs) * len(files)
        if len(cached_rows) != expected_rows or not all(truthy(r["correct"]) for r in cached_rows):
            raise SystemExit("baseline cache rejected: external rows incomplete or incorrect")
        tools.update(current_external_tools)
        cached_from = str(cache_dir)

    output_dir = ROOT / "results" / args.label
    output_dir.mkdir(parents=True, exist_ok=True)
    try:
        git_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    except subprocess.CalledProcessError:
        git_head = "unavailable"
    bite_path = ROOT / "bin/bite"
    metadata = {
        "benchmark_schema": BENCHMARK_SCHEMA,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "stage": args.stage,
        "repeats": args.repeats,
        "platform": platform,
        "git_head": git_head,
        "compiler": version("cc"),
        "bite_sha256": digest(bite_path).hex() if bite_path.exists() else None,
        "tools": tools,
        "files": file_metadata,
        "cached_external_from": cached_from,
        "external_cache_fingerprint": cache_fingerprint(platform, file_metadata, tools),
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
                    if source.stat().st_size != decoded.stat().st_size or source_digests[source] != digest(decoded):
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

    rows.extend(cached_rows)
    fields = list(rows[0])
    with open(output_dir / "per_file.csv", "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
    summaries = []
    summary_names = [c.name for c in selected]
    summary_names.extend(sorted({r["codec"] for r in cached_rows} - set(summary_names)))
    for codec_name in summary_names:
        subset = [r for r in rows if r["codec"] == codec_name]
        original = sum(int(r["original_size_bytes"]) for r in subset)
        compressed = sum(int(r["compressed_size_bytes"]) for r in subset)
        enc_time = sum(float(r["encode_time_seconds"]) for r in subset)
        dec_time = sum(float(r["decode_time_seconds"]) for r in subset)
        summaries.append({
            "codec": codec_name, "original_size_bytes": original, "compressed_size_bytes": compressed,
            "bits_per_byte": 8 * compressed / original, "compression_ratio": original / compressed,
            "encode_time_seconds": enc_time, "decode_time_seconds": dec_time,
            "encode_MB_per_second": original / 1_000_000 / enc_time,
            "decode_MB_per_second": original / 1_000_000 / dec_time,
            "peak_memory_encode": max(int(r["peak_memory_encode"]) for r in subset),
            "peak_memory_decode": max(int(r["peak_memory_decode"]) for r in subset),
            "binary_size": int(subset[0]["binary_size"]), "correct": all(truthy(r["correct"]) for r in subset),
        })
    with open(output_dir / "summary.csv", "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=summaries[0]); writer.writeheader(); writer.writerows(summaries)
    print(f"wrote {output_dir / 'summary.csv'}")


if __name__ == "__main__":
    main()
