#!/usr/bin/env python3
"""EXP-0078 decode-only process timing, codecs interleaved per file.

usage: N=15 decode_interleaved.py DIR_WITH_CDEC_STREAMS FILES...
Streams are DIR/<name>.c from `bite cdec`; references are re-encoded on first use.
Each decode is a fresh process writing a file; per file the median of N runs.
"""
import subprocess, sys, time, statistics, pathlib, os
out = pathlib.Path(sys.argv[1]); files = [pathlib.Path(f) for f in sys.argv[2:]]
W = pathlib.Path(os.environ.get("WORK", "/tmp/exp0078-dt")); W.mkdir(exist_ok=True)
bite = str(pathlib.Path(__file__).resolve().parents[2] / "bin/bite")
codecs = {"bite": None, "zstd-19": (["zstd","-q","-19","-c"], ["zstd","-q","-d","-c"]),
          "xz-9e": (["xz","-9e","-c"], ["xz","-d","-c"]), "brotli-11": (["brotli","-q","11","-c"], ["brotli","-d","-c"])}
N = int(os.environ.get("N", "7"))
tot = {k: 0.0 for k in codecs}
for f in files:
    row = [f.name]
    for name, cmd in codecs.items():
        if name == "bite":
            comp = out / (f.name + ".c")
            run = lambda: subprocess.run([bite, "d", str(comp), str(W / "o")], check=True)
        else:
            comp = W / (f.name + "." + name)
            if not comp.exists():
                with open(comp, "wb") as h: subprocess.run(cmd[0] + [str(f)], stdout=h, check=True)
            def run(cmd=cmd, comp=comp):
                with open(W / "o", "wb") as h: subprocess.run(cmd[1] + [str(comp)], stdout=h, check=True)
        ts = []
        for _ in range(N):
            t = time.perf_counter(); run(); ts.append(time.perf_counter() - t)
        m = statistics.median(ts); tot[name] += m; row.append(f"{name}={m*1000:.1f}ms")
    print(" ".join(row))
print("TOTAL", " ".join(f"{k}={v:.4f}s" for k, v in tot.items()), "bite/zstd=%.2f" % (tot["bite"]/tot["zstd-19"]))
