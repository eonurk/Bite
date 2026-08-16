#!/usr/bin/env python3
"""Generate deterministic heterogeneous development or held-out fixtures."""

import argparse
import hashlib
import json
import math
import random
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def write(base: Path, relative: str, data: bytes, manifest: list[dict]) -> None:
    path = base / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    manifest.append({"path": relative, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})


def generate(base: Path, seed: int) -> None:
    rng = random.Random(seed)
    manifest: list[dict] = []

    paragraphs = []
    nouns = ["compressor", "stream", "model", "window", "literal", "parser", "symbol"]
    verbs = ["observes", "predicts", "encodes", "measures", "compares", "reuses"]
    for i in range(7000):
        paragraphs.append(
            f"Experiment {i:05d}: the {nouns[i % len(nouns)]} {verbs[(i * 3) % len(verbs)]} "
            f"a {nouns[(i * 5 + 2) % len(nouns)]} in block {i // 37}. "
            "Lossless reconstruction remains the non-negotiable invariant.\n"
        )
    write(base, "text/research_notes.txt", "".join(paragraphs).encode(), manifest)

    code = ["#include <stdint.h>\n\n"]
    for i in range(5000):
        code.append(
            f"static uint32_t transform_{i % 41}(uint32_t x) {{ "
            f"return (x ^ {i * 2654435761 & 0xffffffff}u) * 0x9e3779b1u; }} /* row {i} */\n"
        )
    write(base, "source_code/generated.c", "".join(code).encode(), manifest)

    events = []
    for i in range(9000):
        events.append({
            "id": i, "kind": ["click", "view", "purchase", "logout"][i % 4],
            "user": f"user-{(i * 17) % 503:04d}", "ok": i % 19 != 0,
            "values": [round(math.sin(i + j), 6) for j in range(3)],
        })
    write(base, "json/events.json", json.dumps(events, separators=(",", ":")).encode(), manifest)

    xml = ["<?xml version=\"1.0\"?><catalog>\n"]
    for i in range(10000):
        xml.append(f'<item id="{i}" class="c{i % 23}"><name>object-{i % 311}</name><value>{i * 7}</value></item>\n')
    xml.append("</catalog>\n")
    write(base, "xml/catalog.xml", "".join(xml).encode(), manifest)

    opcodes = bytearray()
    templates = [bytes([0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x20]),
                 bytes([0x48, 0x8B, 0x45, 0xF8, 0x48, 0x01, 0xD0, 0xC3])]
    for i in range(50000):
        opcodes.extend(templates[i & 1])
        opcodes.extend(struct.pack("<I", (i * 65537) & 0xffffffff))
    write(base, "executables/pseudo_text.bin", bytes(opcodes), manifest)

    write(base, "binary_data/random.bin", rng.randbytes(512 * 1024), manifest)

    readings = bytearray()
    for i in range(100000):
        readings.extend(struct.pack("<d", math.sin(i / 200.0) + (rng.random() - 0.5) * 1e-5))
    write(base, "scientific_data/readings.f64", bytes(readings), manifest)

    image = bytearray()
    for y in range(512):
        for x in range(768):
            image.extend(((x + y) & 255, (2 * x + y // 4) & 255, (x // 8 + 3 * y) & 255))
    write(base, "images_raw/gradient.rgb", bytes(image), manifest)

    for size in (0, 1, 17, 103, 1024, 4096):
        data = bytes((i * 31 + size) & 255 for i in range(size))
        write(base, f"small_files/small_{size:04d}.bin", data, manifest)

    mixed = bytearray()
    for i in range(256):
        mixed.extend(f"record={i};type={i % 7};payload=".encode())
        mixed.extend(bytes([i & 255]) * (64 + i % 31))
        mixed.extend(rng.randbytes(128))
    write(base, "mixed/records.bin", bytes(mixed), manifest)

    (base / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--held-out", action="store_true", help="generate the isolated held-out fixtures")
    args = parser.parse_args()
    if args.held_out:
        generate(ROOT / "corpus" / "heldout", 0xB17E5EED)
        print("generated held-out corpus; do not use for routine iteration")
    else:
        generate(ROOT / "corpus" / "dev" / "generated", 0xC0DEC0DE)
        print("generated development corpus")


if __name__ == "__main__":
    main()

