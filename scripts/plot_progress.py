#!/usr/bin/env python3
"""Plot experiment progress on the fixed public research corpus.

Every point is a measured research-suite aggregate from results/*/summary.csv;
nothing is interpolated. Experiments screened only on samples have no
research-corpus number and are drawn as a rug of ticks along the bottom.
Writes docs/progress-light.png and docs/progress-dark.png.
"""

import csv
import json
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
RESEARCH_BYTES = 13_787_888
REFERENCE_DIR = "results/exp-0067-strong-references-research"
# (codec, label, label above the line); xz and zstd sit close, so xz's label goes below.
REFERENCES = (("brotli-11", "Brotli 11", True), ("xz-9e", "xz -9e", False), ("zstd-19", "zstd -19", True))

# Short labels for experiments that set a new research-corpus best.
LABELS = {
    18: "optimal parse", 20: "iterated parse", 21: "costed distances",
    30: "compact models", 31: "wide history", 33: "arithmetic coding",
    36: "static models", 37: "static-cost parse", 38: "second parse pass",
    39: "1 MiB blocks", 40: "16 MiB blocks", 41: "512-deep chain",
    43: "1,024-deep chain", 45: "delta transform", 47: "literal bit contexts",
    48: "matched literals", 50: "mixed literals", 53: "control contexts",
    57: "2,048-deep chain", 58: "recent distances", 68: "small-alphabet order-4",
    74: "literal context mix", 75: "BWT candidate",
}

THEMES = {
    "light": dict(surface="#fcfcfb", text="#0b0b0b", text2="#52514e", grid="#e6e5e0",
                  kept="#2a78d6", pending="#eb6834", rejected="#b3b2ab", ref="#8a8984"),
    "dark": dict(surface="#1a1a19", text="#f0efec", text2="#c3c2b7", grid="#2e2e2c",
                 kept="#3987e5", pending="#d95926", rejected="#5e5d58", ref="#8f8e88"),
}


def research_points():
    """Return ({experiment: bytes}, {experiment: parent bytes}).

    Takes the smallest candidate across an experiment's runs.
    """
    best, parents = {}, {}
    for summary in sorted(ROOT.glob("results/exp-*/summary.csv")):
        match = re.match(r"exp-(\d{4})", summary.parent.name)
        meta_path = summary.parent / "metadata.json"
        if not match or not meta_path.exists() or summary.parent.name.startswith("exp-0067"):
            continue
        if not str(json.loads(meta_path.read_text()).get("corpus_dir", "")).endswith("corpus/research"):
            continue
        rows = [r for r in csv.DictReader(summary.open()) if r["codec"].startswith("bite")]
        if not rows or int(rows[-1]["original_size_bytes"]) != RESEARCH_BYTES:
            continue
        # The candidate is the last Bite codec listed; parents come first.
        size = int(rows[-1]["compressed_size_bytes"])
        number = int(match.group(1))
        best[number] = min(size, best.get(number, size))
        if len(rows) > 1:
            parents[number] = int(rows[0]["compressed_size_bytes"])
    return best, parents


def bpb(size):
    return 8 * size / RESEARCH_BYTES


def main():
    decisions = {int(r["experiment_id"][4:]): r["decision"]
                 for r in csv.DictReader((ROOT / "experiments.csv").open())}
    points, parents = research_points()
    # Reference runs measure other codecs; their Bite rows are not experiment results.
    points = {n: v for n, v in points.items() if decisions.get(n) != "REFERENCE_TARGET"}
    references = {r["codec"]: int(r["compressed_size_bytes"])
                  for r in csv.DictReader((ROOT / REFERENCE_DIR / "summary.csv").open())}
    # Only experiments whose sole evidence is a sample or targeted screen.
    screened = {int(d.name[4:8]) for d in ROOT.glob("results/exp-*")
                if "sample" in d.name or "target" in d.name}
    sample_only = sorted(n for n in screened if n > min(points) and n not in points)

    out_dir = ROOT / "docs"
    out_dir.mkdir(exist_ok=True)
    for theme_name, t in THEMES.items():
        plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
        fig, ax = plt.subplots(figsize=(11, 5.6), dpi=160)
        fig.patch.set_facecolor(t["surface"])
        ax.set_facecolor(t["surface"])

        xs = sorted(points)
        running = parents[xs[0]]
        frontier_x, frontier_y = [], []
        for x in xs:
            if decisions.get(x, "PENDING").startswith("KEEP"):
                running = min(running, points[x])
            frontier_x.append(x)
            frontier_y.append(bpb(running))
        ax.step(frontier_x, frontier_y, where="post", color=t["kept"], lw=2, zorder=2,
                label="Best retained so far")

        for codec, name, above in REFERENCES:
            y = bpb(references[codec])
            ax.axhline(y, color=t["ref"], lw=1, ls=(0, (4, 3)), zorder=1)
            ax.text(xs[0] - 0.6, y + (0.002 if above else -0.003), f"{name}  {y:.3f}",
                    color=t["text2"], fontsize=8.5, va="bottom" if above else "top", ha="left")

        previous_best = parents[xs[0]]
        for x in xs:
            y = bpb(points[x])
            decision = decisions.get(x, "PENDING")
            new_best = points[x] < previous_best
            if decision == "PENDING":
                style = dict(color=t["pending"], s=64, zorder=4)
            elif decision.startswith("KEEP") and new_best:
                style = dict(color=t["kept"], s=56, zorder=4)
            elif decision.startswith("KEEP"):
                style = dict(facecolors=t["surface"], edgecolors=t["kept"], linewidths=1.6, s=44, zorder=3)
            else:
                style = dict(color=t["rejected"], s=36, zorder=3)
            ax.scatter([x], [y], edgecolor=style.pop("edgecolors", t["surface"]),
                       linewidths=style.pop("linewidths", 1.5), **style)
            if decision.startswith(("KEEP", "PENDING")) and new_best and x in LABELS:
                ax.annotate(LABELS[x], (x, y), xytext=(4, 5), textcoords="offset points",
                            rotation=32, fontsize=8, color=t["text2"], ha="left", va="bottom")
            if decision.startswith(("KEEP", "PENDING")):
                previous_best = min(previous_best, points[x])

        y_low, y_high = 1.84, max(bpb(points[x]) for x in xs) + 0.06
        ax.set_ylim(y_low, y_high)
        rug_y = y_low + 0.012
        ax.scatter(sample_only, [rug_y] * len(sample_only), marker="|", s=90, lw=1.6,
                   color=t["rejected"], zorder=3)

        # Legend proxies keep marker meaning independent of color alone.
        ax.scatter([], [], color=t["kept"], s=56, edgecolor=t["surface"], label="Kept: new best")
        ax.scatter([], [], facecolors=t["surface"], edgecolors=t["kept"], linewidths=1.6, s=44,
                   label="Kept for speed or memory")
        ax.scatter([], [], color=t["rejected"], s=36, edgecolor=t["surface"], label="Rejected")
        if any(x not in decisions for x in xs):
            ax.scatter([], [], color=t["pending"], s=64, edgecolor=t["surface"],
                       label="Not yet validated")
        ax.scatter([], [], marker="|", s=90, lw=1.6, color=t["rejected"],
                   label="Screened on a sample only")
        legend = ax.legend(loc="upper right", frameon=False, fontsize=8.5, labelcolor=t["text"])

        latest = max(xs)
        kept_count = sum(1 for d in decisions.values() if d.startswith("KEEP"))
        experiments = sum(1 for d in decisions.values() if d != "REFERENCE_TARGET")
        experiments += latest not in decisions
        ax.set_title(f"Bite: {experiments} experiments, "
                     f"{kept_count} kept", loc="left", color=t["text"], fontsize=13,
                     fontweight="bold", pad=12)
        ax.set_xlabel("Experiment", color=t["text2"])
        ax.set_ylabel("Research corpus bits per byte (lower is better)", color=t["text2"])
        ax.set_xlim(xs[0] - 1, latest + 3)
        ax.grid(axis="y", color=t["grid"], lw=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(t["grid"])
        ax.tick_params(colors=t["text2"], labelsize=9)
        legend.set_zorder(5)
        fig.tight_layout()
        fig.savefig(out_dir / f"progress-{theme_name}.png", facecolor=t["surface"])
        plt.close(fig)
    print(f"{len(points)} research points, {len(sample_only)} sample-only experiments")


if __name__ == "__main__":
    main()
