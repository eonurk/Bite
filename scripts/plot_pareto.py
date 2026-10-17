"""Plot size against encode and decode speed on the public research corpus.

Every point comes from one process-harness run on one idle M1 Mac
(results/m1-2-timing-research): same machine, same tool versions, 3 repeats,
single-threaded. Writes docs/pareto-light.png and docs/pareto-dark.png.
"""

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
RESEARCH_BYTES = 13_787_888
RUN = "m1-2-timing-research"

# codec -> (label, family); codecs not listed are left out of the chart.
CODECS = {
    "bite-max-bwt": ("cmax", "bite"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-context-mix3": ("EXP-0074", "bite"),
    "bite-link-split-optimal-wide-arithmetic-static-parse2-fast": ("cfast", "bite"),
    "bite-bwt": ("cbwt", "bite"),
    "brotli-11": ("Brotli 11", "ref"),
    "brotli-6": ("Brotli 6", "ref"),
    "xz-9e": ("xz -9e", "ref"),
    "zstd-19": ("zstd -19", "ref"),
    "zstd-3": ("zstd -3", "ref"),
    "gzip-9": ("gzip -9", "ref"),
}

THEMES = {
    "light": dict(surface="#fcfcfb", text="#0b0b0b", text2="#52514e", grid="#e6e5e0",
                  bite="#2a78d6", ref="#eb6834", frontier="#8a8984"),
    "dark": dict(surface="#1a1a19", text="#f0efec", text2="#c3c2b7", grid="#2e2e2c",
                 bite="#3987e5", ref="#d95926", frontier="#8f8e88"),
}

# Label offsets in points, tuned to avoid collisions: (encode, decode).
OFFSETS = {
    "cmax": ((6, -4), (6, -4)), "EXP-0074": ((6, 2), (-22, 7)),
    "cfast": ((6, 2), (6, -11)), "cbwt": ((7, -6), (-26, -14)),
    "xz -9e": ((-46, -12), (6, -10)), "zstd -19": ((-20, 6), (6, 2)),
    "zstd -3": ((6, 2), (-40, 4)), "gzip -9": ((6, 2), (6, -12)),
}


def load():
    rows = []
    with (ROOT / "results" / RUN / "summary.csv").open() as handle:
        for row in csv.DictReader(handle):
            if row["codec"] not in CODECS:
                continue
            if int(row["original_size_bytes"]) != RESEARCH_BYTES:
                raise SystemExit(f"{row['codec']} is not the research corpus aggregate")
            label, family = CODECS[row["codec"]]
            size = int(row["compressed_size_bytes"])
            rows.append(dict(label=label, family=family, process=True,
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
            ax.xaxis.set_major_locator(LogLocator(subs=(1, 2, 5)))
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
