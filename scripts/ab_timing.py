#!/usr/bin/env python3
"""Same-run A/B encode timing of Bite binaries.

Background load hits every variant equally: within each repeat the variants
alternate per file, and the starting variant rotates between repeats. Every
variant's stream must be byte-identical to the first variant's, so this is
only for speed changes that keep the format. Reports the median over repeats
of each variant's total wall and CPU time.

  python3 scripts/ab_timing.py --repeats 3 --label ab-example \\
      --variant old=/path/old/bite --variant new=/path/new/bite:BITE_THREADS=8 \\
      --mode cmax --corpus-dir corpus/research
"""

import argparse
import json
import os
import resource
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def parse_variant(text: str) -> tuple[str, str, dict[str, str]]:
    name, _, rest = text.partition("=")
    binary, *settings = rest.split(":")
    env = dict(setting.split("=", 1) for setting in settings)
    return name, binary, env


def children_cpu() -> float:
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", action="append", required=True,
                        help="NAME=BINARY[:VAR=VALUE...]; the first is the reference stream")
    parser.add_argument("--mode", required=True)
    parser.add_argument("--corpus-dir", type=Path, default=ROOT / "corpus" / "research")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--label", required=True)
    args = parser.parse_args()
    variants = [parse_variant(v) for v in args.variant]
    files = sorted(p for p in args.corpus_dir.rglob("*") if p.is_file() and p.name != "MANIFEST.json")
    totals = {name: {"wall": [], "cpu": []} for name, _, _ in variants}
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        for repeat in range(args.repeats):
            order = variants[repeat % len(variants):] + variants[:repeat % len(variants)]
            wall = {name: 0.0 for name, _, _ in variants}
            cpu = {name: 0.0 for name, _, _ in variants}
            for index, source in enumerate(files):
                # Alternate direction per file as well, so neither variant always goes first.
                for name, binary, extra in (order if index % 2 == 0 else order[::-1]):
                    output = temp / f"{name}.bite"
                    env = {**os.environ, **extra}
                    cpu_start, start = children_cpu(), time.perf_counter()
                    subprocess.run([binary, args.mode, str(source), str(output)], env=env, check=True)
                    wall[name] += time.perf_counter() - start
                    cpu[name] += children_cpu() - cpu_start
                reference = (temp / f"{variants[0][0]}.bite").read_bytes()
                for name, _, _ in variants[1:]:
                    if (temp / f"{name}.bite").read_bytes() != reference:
                        raise SystemExit(f"{name} stream differs from {variants[0][0]} on {source}")
            for name in wall:
                totals[name]["wall"].append(wall[name])
                totals[name]["cpu"].append(cpu[name])
            print(f"repeat {repeat + 1}: " + ", ".join(f"{n} {wall[n]:.2f}s" for n in wall), flush=True)
    reference = statistics.median(totals[variants[0][0]]["wall"])
    summary = {name: {"median_wall_seconds": statistics.median(t["wall"]),
                      "median_cpu_seconds": statistics.median(t["cpu"]),
                      "speedup_vs_first": reference / statistics.median(t["wall"]),
                      "wall_seconds": t["wall"], "cpu_seconds": t["cpu"]}
               for name, t in totals.items()}
    output_dir = ROOT / "results" / args.label
    output_dir.mkdir(parents=True, exist_ok=True)
    metadata = {"mode": args.mode, "corpus_dir": str(args.corpus_dir), "files": len(files),
                "repeats": args.repeats, "platform": os.uname().version,
                "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "variants": {n: {"binary": b, "env": e} for n, b, e in variants}}
    (output_dir / "ab.json").write_text(json.dumps({"metadata": metadata, "summary": summary}, indent=2))
    for name, s in summary.items():
        print(f"{name}: wall {s['median_wall_seconds']:.2f}s, cpu {s['median_cpu_seconds']:.2f}s, "
              f"{s['speedup_vs_first']:.2f}x vs {variants[0][0]}")


if __name__ == "__main__":
    main()
