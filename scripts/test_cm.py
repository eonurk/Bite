#!/usr/bin/env python3
"""ccm round trips, exact selection against cmax, and malformed mode-28 streams."""
import argparse
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def words(seed, size):
    rng = random.Random(seed)
    vocab = ["the", "model", "context", "mixing", "of", "bits", "and", "a", "byte",
             "predicts", "stream", "coder", "weight", "table", "\n", "is", "to"]
    out = []
    while sum(map(len, out)) < size:
        out.append(rng.choice(vocab) + rng.choice([" ", " ", ", ", ". "]))
    return "".join(out).encode()[:size]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic"]
    flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else ["-O2"]
    env = dict(os.environ)
    if args.sanitize:
        # Apple's AddressSanitizer aborts when asked for leak detection it lacks.
        leaks = "0" if sys.platform == "darwin" else "1"
        env["ASAN_OPTIONS"] = f"detect_leaks={leaks}:quarantine_size_mb=32"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        binary = temp / "bite"
        subprocess.run([os.environ.get("CC", "cc"), *flags, str(ROOT / "src/bite.c"), "-o", str(binary)],
                       check=True)
        source, encoded, decoded = temp / "input", temp / "encoded", temp / "decoded"

        def invoke(mode, src, dst, valid=True):
            result = subprocess.run([str(binary), mode, str(src), str(dst)],
                                    capture_output=True, env=env, timeout=600)
            if result.returncode not in (0, 1) or b"Sanitizer" in result.stderr or b"runtime error:" in result.stderr:
                raise AssertionError(result.stderr.decode(errors="replace"))
            if valid and result.returncode:
                raise AssertionError(result.stderr.decode(errors="replace"))
            return result.returncode

        # Round trips and exact selection: ccm is cmax unless mode 28 is smaller.
        rng = random.Random(79)
        inputs = [b"", b"a", bytes(range(256)), rng.randbytes(4096), b"ab" * 3000,
                  words(1, 1), words(2, 255), words(3, 20000), bytes(rng.randrange(4) for _ in range(5000))]
        for data in inputs:
            source.write_bytes(data)
            invoke("cmax", source, encoded)
            parent = encoded.read_bytes()
            invoke("ccm", source, encoded)
            child = encoded.read_bytes()
            if child != parent:
                assert len(child) < len(parent) and child[16] == 28, "ccm must only add mode 28"
            invoke("d", encoded, decoded)
            assert decoded.read_bytes() == data

        source.write_bytes(words(4, 6000))
        invoke("ccm", source, encoded)
        stream = encoded.read_bytes()
        assert stream[16] == 28, "malformed tests must exercise mode 28"
        stored = struct.unpack_from("<I", stream, 21)[0]
        assert len(stream) == 25 + stored
        for mode in ("dwork", "dslow"):
            invoke(mode, encoded, decoded)
            assert decoded.read_bytes() == source.read_bytes()
        cases = [stream[:i] for i in range(len(stream))]
        cases.append(stream + b"trailing")
        bad = bytearray(stream[:-1])  # Stored size agrees, but the coder runs out.
        struct.pack_into("<I", bad, 21, stored - 1)
        cases.append(bytes(bad))
        bad = bytearray(stream + b"\0")  # Stored size agrees, but bytes are left over.
        struct.pack_into("<I", bad, 21, stored + 1)
        cases.append(bytes(bad))
        bad = bytearray(stream)
        struct.pack_into("<I", bad, 21, 3)  # Shorter than the coder's four-byte state.
        cases.append(bytes(bad[:28]))
        for case in cases:
            encoded.write_bytes(case)
            assert invoke("d", encoded, decoded, valid=False) == 1
        # No integrity checksum: mutations must terminate without memory errors or UB.
        for _ in range(100):
            bad = bytearray(stream)
            offset = rng.randrange(25, len(bad))
            bad[offset] ^= 1 << rng.randrange(8)
            encoded.write_bytes(bad)
            invoke("d", encoded, decoded, valid=False)
        print(f"{len(inputs)} exact-selection round trips, {len(cases)} rejection cases "
              "and 100 mutations passed")


if __name__ == "__main__":
    main()
