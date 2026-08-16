#!/usr/bin/env python3
import hashlib
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = sorted(p for p in (ROOT / "corpus" / "dev").rglob("*") if p.is_file() and p.name != "MANIFEST.json")

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    for mode in ("c", "cl"):
        for index, source in enumerate(FILES):
            encoded = temp / f"{mode}-{index}.bite"
            decoded = temp / f"{mode}-{index}.decoded"
            subprocess.run([ROOT / "bin/bite", mode, source, encoded], check=True)
            subprocess.run([ROOT / "bin/bite", "d", encoded, decoded], check=True)
            source_hash = hashlib.sha256(source.read_bytes()).digest()
            decoded_hash = hashlib.sha256(decoded.read_bytes()).digest()
            if source_hash != decoded_hash:
                raise SystemExit(f"round-trip mismatch ({mode}): {source}")
        print(f"all {len(FILES)} files round-trip exactly in mode {mode}")
