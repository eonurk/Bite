#!/usr/bin/env python3
"""Plot size against encode and decode speed on the public research corpus.

Points are measured research-suite aggregates from results/*/summary.csv.
Process-harness timings are directly comparable with the reference codecs;
EXP-0074 and EXP-0075 were timed only by the in-process runner, so they are
drawn hollow and kept off the Pareto frontier.
Writes docs/pareto-light.png and docs/pareto-dark.png.
"""

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
RESEARCH_BYTES = 13_787_888

# (results directory, codec, label, family, process-timed)
POINTS = (
    ("exp-0030-compact-models-research-external", "zstd-3", "zstd -3", "ref", True),
    ("exp-0030-compact-models-research-external", "brotli-6", "Brotli 6", "ref", True),
    ("exp-0067-strong-references-research", "zstd-19", "zstd -19", "ref", True),
    ("exp-0067-strong-references-research", "xz-9e", "xz -9e", "ref", True),
    ("exp-0067-strong-references-research", "brotli-11", "Brotli 11", "ref", True),
    ("exp-0028-research", "bite-link-split-optimal-distances-bucketed", "EXP-0028", "bite", True),
    ("exp-0030-compact-models-research-external", "bite-link-split-optimal-compact-models",
     "EXP-0030", "bite", True),
    ("exp-0067-strong-references-research",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-rep-distances", "EXP-0058", "bite", True),
    ("exp-0068-small-alphabet-order4-research",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-small-alphabet-order4",
     "EXP-0068", "bite", True),
    ("exp-0069-fast-pareto-research-screen",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-fast", "", "bite", True),
    ("exp-0070-fast-cost48-research-screen",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-fast", "", "bite", True),
    ("exp-0071-fast-cost32-research-screen",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-fast", "", "bite", True),
    ("exp-0072-fast-cost16-research-screen",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-fast", "cfast (EXP-0072)", "bite", True),
    ("exp-0074-context-mix3-tuned-research-inprocess",
     "bite-link-split-optimal-wide-arithmetic-static-parse2-context-mix3",
     "EXP-0074", "bite", False),
    ("exp-0075-bwt-research-inprocess", "bite-max-bwt", "cmax (EXP-0075)", "bite", False),
)

THEMES = {
    "light": dict(surface="#fcfcfb", text="#0b0b0b", text2="#52514e", grid="#e6e5e0",
                  bite="#2a78d6", ref="#eb6834", frontier="#8a8984"),
    "dark": dict(surface="#1a1a19", text="#f0efec", text2="#c3c2b7", grid="#2e2e2c",
                 bite="#3987e5", ref="#d95926", frontier="#8f8e88"),
}

# Label offsets in points, tuned to avoid collisions: (encode, decode).
OFFSETS = {
    "EXP-0058": ((6, 4), (6, 4)), "EXP-0068": ((6, -10), (-50, -12)),
    "EXP-0074": ((6, 2), (-48, 4)), "cmax (EXP-0075)": ((6, -4), (6, -4)),
    "cfast (EXP-0072)": ((-12, 7), (6, -11)), "xz -9e": ((6, -10), (6, -10)),
    "zstd -19": ((6, 2), (6, 2)),
}


def load():
    rows = []
    for directory, codec, label, family, process in POINTS:
        with (ROOT / "results" / directory / "summary.csv").open() as handle:
            row = next(r for r in csv.DictReader(handle) if r["codec"] == codec)
        if int(row["original_size_bytes"]) != RESEARCH_BYTES:
            raise SystemExit(f"{directory}/{codec} is not the research corpus aggregate")
        size = int(row["compressed_size_bytes"])
        rows.append(dict(label=label, family=family, process=process,
                         bpb=8 * size / RESEARCH_BYTES,
                         encode=RESEARCH_BYTES / 1e6 / float(row["encode_time_seconds"]),
                         decode=RESEARCH_BYTES / 1e6 / float(row["decode_time_seconds"])))
    return rows


def frontier(rows, key):
    """Points no other process-timed point beats on both size and speed."""
    eligible = sorted((r for r in rows if r["process"]), key=lambda r: -r[key])
    kept, best = [], float("inf")
    for r in eligible:
        if r["bpb"] < best:
            kept.append(r)
            best = r["bpb"]
    return kept


def main():
    rows = load()
    out_dir = ROOT / "docs"
    out_dir.mkdir(exist_ok=True)
    for theme_name, t in THEMES.items():
        plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
        fig, axes = plt.subplots(1, 2, figsize=(11, 5.2), dpi=160, sharey=True)
        fig.patch.set_facecolor(t["surface"])
        for index, (ax, key, title) in enumerate(zip(
                axes, ("encode", "decode"), ("Encode speed", "Decode speed"))):
            ax.set_facecolor(t["surface"])
            front = frontier(rows, key)
            ax.step([r[key] for r in front], [r["bpb"] for r in front], where="post",
                    color=t["frontier"], lw=1.2, ls=(0, (4, 3)), zorder=1)
            for r in rows:
                color = t[r["family"]]
                if r["process"]:
                    ax.scatter(r[key], r["bpb"], s=48, color=color, edgecolor=t["surface"],
                               linewidths=1.5, zorder=3)
                else:
                    ax.scatter(r[key], r["bpb"], s=44, facecolors=t["surface"], edgecolors=color,
                               linewidths=1.6, zorder=3)
                if r["label"]:
                    offset = OFFSETS.get(r["label"], ((6, 2), (6, 2)))[index]
                    ax.annotate(r["label"], (r[key], r["bpb"]), xytext=offset,
                                textcoords="offset points", fontsize=8, color=t["text2"])
            ax.set_xscale("log")
            ax.xaxis.set_major_locator(LogLocator(subs=(1, 3)))
            ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
            ax.xaxis.set_minor_formatter(NullFormatter())
            ax.set_title(title, loc="left", color=t["text"], fontsize=10.5)
            ax.set_xlabel("MB/s, log scale (faster is right)", color=t["text2"])
            ax.grid(color=t["grid"], lw=0.8)
            ax.set_axisbelow(True)
            for side in ("top", "right"):
                ax.spines[side].set_visible(False)
            for side in ("left", "bottom"):
                ax.spines[side].set_color(t["grid"])
            ax.tick_params(colors=t["text2"], labelsize=9, which="both")
        axes[0].set_ylabel("Research corpus bits per byte (lower is better)", color=t["text2"])

        axes[0].scatter([], [], s=48, color=t["bite"], edgecolor=t["surface"], label="Bite")
        axes[0].scatter([], [], s=44, facecolors=t["surface"], edgecolors=t["bite"],
                        linewidths=1.6, label="Bite, timed in-process only")
        axes[0].scatter([], [], s=48, color=t["ref"], edgecolor=t["surface"], label="Reference codec")
        axes[0].plot([], [], color=t["frontier"], lw=1.2, ls=(0, (4, 3)), label="Pareto frontier")
        fig.legend(loc="upper right", ncol=4, frameon=False, fontsize=8.5, labelcolor=t["text"],
                   bbox_to_anchor=(0.995, 0.955))
        fig.suptitle("Size versus speed on the research corpus", x=0.012, ha="left",
                     color=t["text"], fontsize=13, fontweight="bold")
        fig.tight_layout(rect=(0, 0, 1, 0.94))
        fig.savefig(out_dir / f"pareto-{theme_name}.png", facecolor=t["surface"])
        plt.close(fig)


if __name__ == "__main__":
    main()
