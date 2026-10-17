#!/usr/bin/env python3
"""Generate the self-contained Bite validation report at repo-root/viz.html."""

import csv
import json
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_summary(relative: str) -> tuple[Path, list[dict[str, str]]]:
    path = ROOT / relative / "summary.csv"
    with path.open(newline="") as handle:
        return path, list(csv.DictReader(handle))


def codec_row(rows: list[dict[str, str]], codec: str) -> dict[str, str]:
    for row in rows:
        if row["codec"] == codec:
            return row
    raise SystemExit(f"missing codec {codec!r}")


def measurement(row: dict[str, str]) -> dict:
    return {
        "codec": row["codec"],
        "size": int(row["compressed_size_bytes"]),
        "bpb": float(row["bits_per_byte"]),
        "ratio": float(row["compression_ratio"]),
        "encodeSeconds": float(row["encode_time_seconds"]),
        "decodeSeconds": float(row["decode_time_seconds"]),
        "encodeSpeed": float(row["encode_MB_per_second"]),
        "decodeSpeed": float(row["decode_MB_per_second"]),
        "encodeMemory": int(row.get("peak_memory_encode", 0) or 0),
        "decodeMemory": int(row.get("peak_memory_decode", 0) or 0),
        "correct": row["correct"].lower() == "true",
    }


def comparison(bite_relative: str, bite_codec: str, reference_relative: str,
               reference_codec: str, reference_label: str,
               label: str, caveat: str) -> dict:
    bite_path, bite_rows = read_summary(bite_relative)
    reference_path, reference_rows = read_summary(reference_relative)
    bite = measurement(codec_row(bite_rows, bite_codec))
    reference = measurement(codec_row(reference_rows, reference_codec))
    byte_delta = bite["size"] - reference["size"]
    return {
        "label": label,
        "source": f"{bite_path.relative_to(ROOT)} + {reference_path.relative_to(ROOT)}",
        "originalSize": int(bite_rows[0]["original_size_bytes"]),
        "aggregateFiles": int(bite_rows[0]["aggregate_files"]),
        "excludedFiles": int(bite_rows[0]["excluded_files"]),
        "caveat": caveat,
        "bite": bite,
        "reference": reference,
        "referenceLabel": reference_label,
        "byteDelta": byte_delta,
        "sizeDeltaPct": byte_delta / reference["size"] * 100,
        "encodeTimeMultiple": bite["encodeSeconds"] / reference["encodeSeconds"],
        "decodeTimeMultiple": bite["decodeSeconds"] / reference["decodeSeconds"],
        "memoryMultiple": bite["encodeMemory"] / reference["encodeMemory"],
    }


SAMPLE_RUNS = [
    ("EXP-0069", "exp-0069-fast-pareto-research-screen", "Candidate", True),
    ("EXP-0070", "exp-0070-fast-cost48-research-screen", "Candidate", True),
    ("EXP-0071", "exp-0071-fast-cost32-research-screen", "Candidate", True),
    ("EXP-0072", "exp-0072-fast-cost16-research-screen", "Fast endpoint", True),
    ("EXP-0073", "exp-0073-fast-cost8-research-screen", "Rejected: loses xz", True),
]


def sample_runs() -> list[dict]:
    result = []
    for experiment_id, directory, status, timing_valid in SAMPLE_RUNS:
        path, rows = read_summary(f"results/{directory}")
        value = measurement(rows[0])
        value.update({
            "id": experiment_id,
            "status": status,
            "timingValid": timing_valid,
            "source": str(path.relative_to(ROOT)),
        })
        result.append(value)
    baseline_size = result[0]["size"]
    baseline_time = result[0]["encodeSeconds"]
    for row in result:
        row["bytesAdded"] = row["size"] - baseline_size
        row["secondsSaved"] = baseline_time - row["encodeSeconds"]
        row["sizeDeltaPct"] = (row["size"] - baseline_size) / baseline_size * 100
    return result


def build_payload() -> dict:
    return {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "endpoint": "EXP-0072 fast endpoint · EXP-0068 size-max retained",
        "mode": "clblinksopt2dfsbwasp2b16x2tdlmmdcra4f",
        "research": comparison(
            "results/exp-0072-fast-cost16-research-screen",
            "bite-link-split-optimal-wide-arithmetic-static-parse2-fast",
            "results/exp-0067-strong-references-research",
            "brotli-11",
            "Brotli 11",
            "Independent public research corpus",
            "Fast Bite is 1,536 bytes smaller than xz-9e. Brotli 11 remains 75,558 bytes smaller and 1.62× faster.",
        ),
        "development": comparison(
            "results/exp-0072-fast-cost16-dev-screen",
            "bite-link-split-optimal-wide-arithmetic-static-parse2-fast",
            "results/exp-0058-final-dev-external",
            "xz-6",
            "xz-6",
            "Frozen development aggregate",
            "This corpus influenced development. Its result is useful diagnostically, not evidence of general superiority.",
        ),
        "sampleRuns": sample_runs(),
        "checks": [
            {"label": "16 + 26 corpus files", "detail": "EXP-0072 round-tripped every research and development file, including the excluded random fixture."},
            {"label": "16 MiB boundaries", "detail": "Boundary −1, exact, and +1 random inputs round-tripped."},
            {"label": "ASan + UBSan", "detail": "The actual fast early-return mode-23 path encoded and decoded cleanly."},
            {"label": "Malformed streams", "detail": "A truncated mode-23 stream and the retained malformed-header cases rejected cleanly."},
            {"label": "Portable hash input", "detail": "Four-byte hash loads use explicit little-endian assembly."},
        ],
    }


def main() -> None:
    payload = build_payload()
    template = (ROOT / "scripts" / "viz_template.html").read_text()
    rendered = template.replace("__BITE_DATA__", json.dumps(payload, separators=(",", ":")))
    output = ROOT / "viz.html"
    output.write_text(rendered)
    print(f"wrote {output} from final validation and {len(payload['sampleRuns'])} sample runs")


if __name__ == "__main__":
    main()
