# EXP-0010 Silesia hold-out report

## Frozen protocol

- Candidate: `EXP-0010`, commit `a239a2f`, mode `clblinksd`.
- Candidate binary: compiled directly from the committed source, not the dirty
  experiment worktree.
- Corpus: the 12-file Silesia Compression Corpus, 211,938,580 bytes.
- Source archive commit: `3f3fa2cdbbb3795c903b74e774acb309e1360337` from
  <https://github.com/MiloszKrajewski/SilesiaCorpus>.
- Download SHA-256:
  `de094aa888ea8f3caaee9094e1452f57a0bfb08877096e82b9ff0eb89ecd790c`.
- Repeats: one initial distribution-shift pass.
- Every codec/file pair was decompressed and checked byte-for-byte. All passed.

## Aggregate results

| Codec | Compressed bytes | Encode MB/s | Decode MB/s |
|---|---:|---:|---:|
| Bite EXP-0010 | 67,070,271 | 19.06 | 151.80 |
| gzip-9 | 67,643,489 | 14.71 | 436.93 |
| zstd-3 | 66,216,569 | 365.24 | 695.74 |
| Brotli-6 | 58,441,670 | 52.20 | 350.27 |
| xz-1 | 58,717,040 | 63.41 | 235.96 |
| xz-6 | 49,408,952 | 4.45 | 110.63 |

Full results are in `summary.csv` and `per_file.csv` beside this report.

## Decision

The development-corpus Pareto result does not reproduce on Silesia. Bite is
dominated by zstd-3, Brotli-6, and xz-1: each is both smaller and faster in both
directions in this run. Bite narrowly beats gzip-9 on size (0.85%) and encode
speed (29.6%), but decodes 65.3% slower.

The repository's generated seed hold-out is not an independent validation set:
12 of its 15 files (4,788,364 of 6,172,098 bytes) are byte-identical to the
development fixtures. Its favorable result is therefore only a seed-stability
sanity check. Silesia is the meaningful distribution-shift result.

Silesia has now been observed and must not be treated as untouched hold-out data
for any future tuning based on these results.
