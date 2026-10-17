# EXP-0017 Silesia holdout

## Protocol

- Candidate source and benchmark harness frozen from commit `e32a817`.
- Corpus: Silesia, 12 files, 211,938,580 bytes.
- Candidate encode mode: `clblinksdf`.
- Candidate decoder: `dwork`.
- Process-level pass: one repeat per file, with round-trip validation and RSS measurement.
- Startup-free comparison: five median-timed repeats per file in both codec orders,
  comparing the parent `d` decoder with `dwork`.
- The active worktree was not compiled or benchmarked.

## Result

Process-level candidate result:

- Compressed size: 67,104,959 bytes (2.5330 bits/byte).
- Encode: 22.14 MB/s.
- Decode: 152.47 MB/s.
- All 12 round trips passed.

Compared with frozen EXP-0010 on the same corpus, EXP-0017 is 0.0517% larger,
16.14% faster to encode, and 0.44% faster to decode in the one-repeat
process-level runs. The size and encode change primarily comes from the sampled
delta filter retained in EXP-0013, not the EXP-0017 decoder gate.

The startup-free `dwork` comparison produced:

- Parent-first order: +0.224% aggregate decode throughput; 6/12 files faster.
- Candidate-first order: +1.282% aggregate decode throughput; 9/12 files faster.
- Order-balanced geometric estimate: approximately +0.75%.
- Only 3/12 files were faster in both orders; the other nine changed sign.

This preserves a small positive aggregate sign but does not reproduce the
3.0-3.7% dev-corpus gain. The Silesia evidence is therefore weak and consistent
with a small optimization near the timing-noise floor.

Using the previously measured external-codec rows from the same Silesia corpus,
EXP-0017 is not Pareto-optimal. `zstd-3` is smaller and substantially faster for
both encoding and decoding; `zstd-6`, Brotli 6, and `xz-1` also dominate it on
compressed size and encode speed.

## Raw results

- `results/holdout-silesia-exp0017-process/summary.csv`
- `results/holdout-silesia-exp0017-inprocess/summary.csv`
- `results/holdout-silesia-exp0017-inprocess-reversed/summary.csv`
