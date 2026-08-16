#!/usr/bin/env python3
import argparse
import concurrent.futures
import hashlib
import os
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = sorted(p for p in (ROOT / "corpus" / "dev").rglob("*") if p.is_file() and p.name != "MANIFEST.json")


def digest(path: Path) -> bytes:
    value = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(1024 * 1024):
            value.update(chunk)
    return value.digest()


def roundtrip(task: tuple[str, int, Path, bytes, Path]) -> None:
    mode, index, source, source_hash, temp = task
    encoded = temp / f"{mode}-{index}.bite"
    decoded = temp / f"{mode}-{index}.decoded"
    subprocess.run([ROOT / "bin/bite", mode, source, encoded], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    subprocess.run([ROOT / "bin/bite", "d", encoded, decoded], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if source.stat().st_size != decoded.stat().st_size or source_hash != digest(decoded):
        raise RuntimeError(f"round-trip mismatch ({mode}): {source}")


parser = argparse.ArgumentParser()
parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
args = parser.parse_args()
if args.jobs < 1:
    raise SystemExit("jobs must be positive")

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    edge_files = []
    rng = random.Random(0xB10C)
    for size in (65535, 65536, 65537, 131072, 131073):
        for kind, data in (("random", rng.randbytes(size)),
                           ("repeat", (b"block-boundary-test-" * ((size + 19) // 20))[:size])):
            path = temp / f"{kind}-{size}.bin"
            path.write_bytes(data)
            edge_files.append(path)
    sources = FILES + edge_files
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        hashes = list(executor.map(digest, sources))
        modes = ("c", "cl", "cb", "clb", "cbh", "clbh", "cblink", "clblink")
        tasks = [(mode, index, source, hashes[index], temp)
                 for mode in modes for index, source in enumerate(sources)]
        list(executor.map(roundtrip, tasks))
    print(f"all {len(sources)} files round-trip exactly in {len(modes)} modes ({args.jobs} jobs)")
