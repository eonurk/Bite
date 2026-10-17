#!/usr/bin/env python3
"""Forced BWT round trips, framing boundaries and malformed mode-25 streams."""
import argparse
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wno-unused-function"]
    flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else ["-O2"]
    env = dict(os.environ)
    if args.sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=1:quarantine_size_mb=32"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        binary, unit = temp / "bite", temp / "unit"
        for source, target in [(ROOT / "src/bite.c", binary), (ROOT / "tests/test_bwt.c", unit)]:
            subprocess.run([os.environ.get("CC", "cc"), *flags, str(source), "-o", str(target)], check=True)
        subprocess.run([str(unit)], check=True, env=env, timeout=120)
        source, encoded, decoded = temp / "input", temp / "encoded", temp / "decoded"

        def invoke(mode, src, dst, valid=True):
            result = subprocess.run([str(binary), mode, str(src), str(dst)],
                                    capture_output=True, env=env, timeout=60)
            if result.returncode not in (0, 1) or b"Sanitizer" in result.stderr or b"runtime error:" in result.stderr:
                raise AssertionError(result.stderr.decode(errors="replace"))
            if valid and result.returncode:
                raise AssertionError(result.stderr.decode(errors="replace"))
            return result.returncode

        # Empty input and both sides of the actual 16 MiB framing boundary.
        for size in ([0, 1, 65537] if args.sanitize else [0, 1, 65537, 16777215, 16777216, 16777217]):
            data = (b"abracadabra-" * ((size + 11) // 12))[:size]
            source.write_bytes(data)
            invoke("cbwt", source, encoded)
            invoke("d", encoded, decoded)
            assert decoded.read_bytes() == data

        source.write_bytes(b"banana bandana\n" * 128)
        invoke("cbwt", source, encoded)
        stream = encoded.read_bytes()
        assert stream[16] == 25, "malformed tests must exercise mode 25"
        for mode in ("dwork", "dslow"):
            invoke(mode, encoded, decoded)
            assert decoded.read_bytes() == source.read_bytes()
        # The combined preset must select the exact smaller representation.
        # Exercise both BWT wins and raw/legacy fallback, including empty input.
        parent = "clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3"
        for data in (b"", b"banana bandana\n" * 128, bytes(range(256)),
                     random.Random(75).randbytes(512)):
            source.write_bytes(data)
            invoke(parent, source, encoded)
            legacy = encoded.read_bytes()
            invoke("cbwt", source, encoded)
            transformed = encoded.read_bytes()
            invoke("cmax", source, encoded)
            expected = transformed if len(transformed) < len(legacy) else legacy
            assert encoded.read_bytes() == expected
            invoke("d", encoded, decoded)
            assert decoded.read_bytes() == data
        cases = [stream[:i] for i in range(len(stream))]
        cases += [stream + b"trailing"]
        bad = bytearray(stream)
        struct.pack_into("<I", bad, 25, struct.unpack_from("<I", stream, 17)[0])
        cases.append(bytes(bad))  # Out-of-range primary row.
        bad = bytearray(stream)
        struct.pack_into("<I", bad, 29, 0xffffffff)
        cases.append(bytes(bad))  # Token count rejected before allocating.
        for case in cases:
            encoded.write_bytes(case)
            assert invoke("d", encoded, decoded, valid=False) == 1
        # Mutations need not be rejected: this format has no integrity checksum.
        # They must terminate without out-of-bounds access or undefined behavior.
        rng = random.Random(75)
        for _ in range(100):
            bad = bytearray(stream)
            offset = rng.randrange(25, len(bad))
            bad[offset] ^= 1 << rng.randrange(8)
            encoded.write_bytes(bad)
            invoke("d", encoded, decoded, valid=False)
        print(f"framing, {len(cases)} rejection cases and 100 mutations passed")


if __name__ == "__main__":
    main()
