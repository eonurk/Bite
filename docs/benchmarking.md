# Benchmarking

## Corpora

The development corpus combines deterministic heterogeneous fixtures with the
public Canterbury corpus. `scripts/fetch_canterbury.sh` pins the archive by
SHA-256. The generated held-out corpus is produced only with the explicit
`--held-out` flag and is not part of routine experiments.

`binary_data/random.bin` is measured and round-trip checked per file but is
excluded from the `summary.csv` aggregate: it is incompressible by construction,
so it cannot move and only dilutes aggregate ratios and reported deltas. It was
22.2% of the pre-exclusion aggregate, which shrank every delta by a factor of
1.29. Per-file rows carry an `in_aggregate` column and `metadata.json` records
`aggregate_excluded_files`; `--aggregate-exclude` overrides the list, and passing
it with no values aggregates every file. Correctness still spans every file, so a
lossless failure on an excluded file still fails the run. Aggregates before and
after this change are not comparable.

The in-process harness now applies the same default random-file aggregate
exclusion as the process harness. All files are still round-trip checked;
`--aggregate-exclude` with no values includes everything. Schema-2 metadata
and per-file `in_aggregate` flags make this explicit.

## Staged experiments

Routine experiments do not rerun every historical mode and slow external tool:

```sh
# One-repeat candidate screen against its direct parent.
make benchmark-screen CODECS="bite-new bite-parent" LABEL=exp-XXXX

# Three-repeat validation after a promising screen.
make benchmark-validate CODECS="bite-new bite-parent" LABEL=exp-XXXX

# Periodic full audit after environment/method changes or several experiments.
make benchmark
```

The original `corpus/dev` suite remains frozen for longitudinal comparison.
Ratio-changing candidates must also pass a separate public research suite:

```sh
make research-corpus
python3 scripts/benchmark.py --stage screen --repeats 1 \
  --corpus-dir corpus/research --codecs bite-new bite-parent \
  --label exp-XXXX-research-screen
```

The research suite contains 13 non-duplicate Calgary files and all three Large
Corpus files from the Canterbury Corpus site. Archive hashes are pinned by the
fetch script. Silesia is observed hold-out evidence and must not be used for
routine tuning.

For small speed effects, use the complementary startup-free runner. It reads
each file once, performs one untimed warm-up, then reports the median of repeated
in-process encode/decode calls with exact comparisons on every repetition:

```sh
python3 scripts/benchmark_inprocess.py --repeats 20 \
  --codecs bite-link-split-delta-filter --label inprocess-baseline
```

This runner does not replace the process harness: peak RSS, startup latency,
binary size, and external-codec audits remain the process harness's job.

Screen and validation runs reuse external rows from `results/exp-0004` by
default. The cache is accepted only if platform, corpus paths/sizes/hashes, and
every external tool version match. Metadata records the cache source, Git head,
Bite binary hash, compiler, benchmark stage, and cache fingerprint. Timing
trials remain serial; only corpus hashing and correctness tests are parallel.

## Reproducing the EXP-0075 screen

Reproduce the combined-preset research screen with:

```sh
python3 scripts/benchmark_inprocess.py --corpus-dir corpus/research \
  --codecs bite-max-bwt --repeats 1 --label exp-0075-bwt-research-inprocess
```
