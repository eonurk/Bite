# EXP-0081 holdout-2026 fetch record

This directory holds metadata only. The corpus is never committed.

- `fetch.log` records a full fetch from an empty download cache: all 36 pinned
  download hashes were verified and the corpus was installed with lock
  `6a6061d06fd8037f81a13309b4783f61018bd335751e2909cb14d45082297c2a` (37 files,
  144,681,868 bytes).
- `dry-run.log` records `--dry-run` on the downloads cached by that fetch: all 36
  hashes were re-verified, the corpus was rebuilt in scratch and matched the same
  lock, and `corpus/holdout-2026` was left untouched.
- `MANIFEST.json` is byte-identical to the installed `corpus/holdout-2026/MANIFEST.json`.

Both logs ran script SHA-256 `2b0b8ee3c9645bc6c0f4791a785c777ebfb5fdf66eb5816bfeca1848edf7b168`
on Linux x86_64 (GNU tar 1.35, XZ Utils 5.4.5, Info-ZIP UnZip 6.00, Python 3.13).

Additional checks, run the same day:

- `sha256sum -c` over the manifest's file hashes, computed independently of the
  script's Python, gave 37/37 OK.
- Tamper test: one byte appended to a cached download made the script stop
  with `HappyFish.jpg hash mismatch` (exit 1) before extracting anything.
- An earlier run from an empty cache was cut off by an HTTP 429 from Maven Central
  after 33 verified downloads. The retry count was raised, the failure message now
  names the item, and the logs above are from the corrected script.
- Overlap check: none of the 37 file hashes matches any of the 57 distinct
  corpus file hashes in `results/*/metadata.json` (dev, generated, research,
  Silesia).
- Publisher checksums match for ripgrep, jq, fzf, pytest (PyPI), typescript
  (npm sha512) and guava (Maven sha1). See `docs/experiments/EXP-0081.md`.

No Bite mode or other compressor was run on any hold-out file.
