#!/usr/bin/env python3
"""EXP-0078 focused tests: `cdec` round trips and malformed mode-27 streams.

Truncations must be rejected. Other corruptions must either decode (mode 27
carries no checksum, so a flipped bit in a near-uniform tANS stream can yield
different bytes, as with zstd without --check) or exit with status 1 through
`fail()`; a signal or sanitizer report is a test failure.
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(binary: Path, *args: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True)


def check_rejected_or_clean(binary: Path, stream: bytes, temp: Path, label: str) -> str:
    damaged = temp / "damaged.bite"
    output = temp / "damaged.out"
    damaged.write_bytes(stream)
    result = run(binary, "d", damaged, output)
    if "Sanitizer" in result.stderr or "runtime error" in result.stderr:
        raise SystemExit(f"sanitizer report on {label}:\n{result.stderr}")
    if result.returncode not in (0, 1):
        raise SystemExit(f"decoder crashed (status {result.returncode}) on {label}")
    return "decoded" if result.returncode == 0 else "rejected"


def block_offsets(stream: bytes) -> list[tuple[int, int, int]]:
    blocks, ip = [], 16
    while ip + 9 <= len(stream):
        mode = stream[ip]
        stored = int.from_bytes(stream[ip + 5:ip + 9], "little")
        blocks.append((mode, ip + 9, stored))
        ip += 9 + stored
    return blocks


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "bin/bite")
    parser.add_argument("--seed", type=int, default=0x27)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    samples = {
        "text": (b"the quick brown fox jumps over the lazy dog. " * 400)
                + bytes(rng.randrange(97, 123) for _ in range(6000)),
        "dna": bytes(rng.choice(b"acgt") for _ in range(30000)),
        "binary": b"".join(bytes([i & 0xff, (i >> 8) & 0xff, rng.randrange(4), 0x7f]) for i in range(6000)),
        "runs": b"a" * 5000 + b"ab" * 3000 + bytes(range(256)) * 20,
        "tiny": b"abcabcabcabcabcabcabcabc",
    }
    rejected = decoded = 0
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        for name, data in samples.items():
            source = temp / f"{name}.bin"
            encoded = temp / f"{name}.bite"
            restored = temp / f"{name}.out"
            source.write_bytes(data)
            for step in (run(args.binary, "cdec", source, encoded),
                         run(args.binary, "d", encoded, restored)):
                if step.returncode:
                    raise SystemExit(f"round trip failed on {name}: {step.stderr}")
            if restored.read_bytes() != data:
                raise SystemExit(f"round-trip mismatch on {name}")
            stream = encoded.read_bytes()
            blocks = [b for b in block_offsets(stream) if b[0] == 27]
            if not blocks:
                raise SystemExit(f"{name} did not select mode 27")
            _, start, stored = blocks[0]
            # Truncations: every cut inside small payloads, sampled cuts otherwise.
            cuts = range(0, stored) if stored <= 400 else sorted(
                set(rng.randrange(stored) for _ in range(150)) | {0, 1, 2, stored - 1})
            for cut in cuts:
                damaged = bytearray(stream[:start + cut])
                result = check_rejected_or_clean(args.binary, bytes(damaged), temp,
                                                 f"{name} truncated at {cut}")
                if result != "rejected":
                    raise SystemExit(f"truncated stream accepted: {name} at {cut}")
                rejected += 1
            # Truncated payload with a consistent block header: decoder must reject.
            for cut in (1, 2, 8, stored // 2, stored - 1):
                if not 0 < cut < stored:
                    continue
                damaged = bytearray(stream[:start + cut])
                damaged[start - 4:start] = cut.to_bytes(4, "little")
                result = check_rejected_or_clean(args.binary, bytes(damaged), temp,
                                                 f"{name} short payload {cut}")
                if result != "rejected":
                    raise SystemExit(f"short payload accepted: {name} at {cut}")
                rejected += 1
            # Random byte and bit corruption anywhere in the payload.
            for trial in range(300):
                damaged = bytearray(stream)
                position = start + rng.randrange(stored)
                if trial % 2:
                    damaged[position] ^= 1 << rng.randrange(8)
                else:
                    damaged[position] = rng.randrange(256)
                if check_rejected_or_clean(args.binary, bytes(damaged), temp,
                                           f"{name} corrupt byte {position}") == "rejected":
                    rejected += 1
                else:
                    decoded += 1
            # Oversized declared counts in the varint header.
            damaged = bytearray(stream)
            damaged[start:start + 1] = b"\xff\xff\xff\xff\x0f"
            check_rejected_or_clean(args.binary, bytes(damaged), temp, f"{name} huge count")
    print(f"mode-27 malformed-stream tests passed: {rejected} rejected, "
          f"{decoded} corruptions decoded (no checksum) without fault")


if __name__ == "__main__":
    sys.exit(main())
