# Bite compression research

Bite is a deliberately small lossless-compression research implementation. The
initial codec is a 64 KiB-window LZSS design with a bounded hash-chain match
finder and grouped literal/match control bits. It is intended for controlled
experiments, not production use.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/progress-dark.png">
  <img alt="Research-corpus bits per byte by experiment: kept improvements step down from 2.50 to 1.94, past zstd -19 and xz -9e; the unvalidated EXP-0075 BWT candidate reaches 1.88, below Brotli 11" src="docs/progress-light.png">
</picture>

Every point is a measured aggregate on the fixed 16-file public research
corpus, taken from `results/*/summary.csv`. Experiments before EXP-0014 predate
that corpus, and ticks along the bottom mark experiments screened only on
samples. Regenerate with `python3 scripts/plot_progress.py`.

The current EXP-0074 size-max endpoint is
`clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3`. On the fixed public research corpus
it compresses 13,787,888 bytes to 3,347,119 bytes (1.9421 bpb). That is 13,689
bytes (0.41%) smaller than xz-9e and 37,354 bytes (1.10%) smaller than zstd-19,
but Brotli 11 remains ahead at 3,283,714 bytes: a 63,405-byte (1.93%) gap.
EXP-0074 saves 1,173 bytes over EXP-0068 on research and 261 bytes on the frozen
development aggregate, with exact selection preventing per-file regressions.
Bite remains slower and heavier than the practical references. These are
corpus-specific results, not a claim that Bite is generally better than xz,
zstd, or Brotli. Reference measurements are in
`results/exp-0067-strong-references-research/`; retained EXP-0074 measurements
are in `results/exp-0074-context-mix3-tuned-{research,dev}-inprocess/`.

EXP-0072 adds the separate
`clblinksopt2dfsbwasp2b16x2tdlmmdcra4f` fast/high-ratio preset. It emits the
same 1,127,219-byte E. coli stream in 0.139 seconds instead of spending about
97 seconds evaluating losing LZ candidates, uses a 1,024-deep discovery chain
and a 16-deep distance-cost chain elsewhere, and omits intermediate wide
representations that did not win the retained pipeline. On the research suite
it produces 3,359,272 bytes in 37.58 seconds: 1,536 bytes smaller than xz-9e
and 4.76 times faster than EXP-0068, but still 75,558 bytes larger and 1.62
times slower than Brotli 11. The fast preset is a Pareto tradeoff, not a new
size maximum; on development it is 15,989 bytes larger than EXP-0068.

## Reproduce

```sh
make
make corpus
make test
python3 scripts/benchmark.py --repeats 3
```

Short presets are available for current work:

```sh
bin/bite cmax INPUT OUTPUT.bite  # EXP-0074 candidates plus exact-selected BWT
bin/bite cfast INPUT OUTPUT.bite # unchanged EXP-0072 fast preset
bin/bite cbwt INPUT OUTPUT.bite  # standalone BWT candidate, with raw fallback
bin/bite d OUTPUT.bite RESTORED
make test-bwt
python3 scripts/test_bwt.py --sanitize
```

EXP-0075 adds block mode 25: a cyclic Burrows–Wheeler transform, move-to-front
ranks, zero-run lengths, and adaptive order-1 arithmetic coding. The payload
starts with a four-byte primary row index, followed by the arithmetic stream.
A nonzero rank is one token; rank-zero runs are token zero followed by an
unsigned base-128 run length. The arithmetic stream carries its token count.
The block limit remains 16 MiB. Prefix-doubling counting sorts bound transform
construction to O(n log n) time and O(n) memory, including periodic inputs.

`cmax` compares the complete mode-25 payload against every EXP-0074 candidate
and raw storage, with ties retaining the earlier representation. Historical
mode names preserve their encoding behavior; older decoders do not support
mode 25. `cbwt` isolates the new path for experiments. The implementation is
self-contained C; the established block-sorting approach is also described in
the [bzip2 documentation](https://www.sourceware.org/bzip2/manual/manual.html).
This is a complementary representation in Bite, not a claim of inventing BWT.

Reproduce the combined-preset research screen with:

```sh
python3 scripts/benchmark_inprocess.py --corpus-dir corpus/research \
  --codecs bite-max-bwt --repeats 1 --label exp-0075-bwt-research-inprocess
```

The in-process harness now applies the same default random-file aggregate
exclusion as the process harness. All files are still round-trip checked;
`--aggregate-exclude` with no values includes everything. Schema-2 metadata
and per-file `in_aggregate` flags make this explicit.

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

Compressed format v1 starts with a 16-byte header, followed by groups of up to
eight tokens. A control bit of zero denotes one literal byte. A one denotes a
three-byte match `(offset-1:uint16le, length-4:uint8)`.

Format v2 uses independent 64 KiB blocks after the same 16-byte file header.
Each block has a nine-byte `(mode, decoded_size, stored_size)` header and is
stored as either raw bytes or a v1 token payload, whichever is strictly smaller.
Modes `cb` and `clb` select blocked greedy and blocked lazy compression;
decompression detects the stream version automatically.

Experimental modes `cbh` and `clbh` add a third exact-cost candidate: a static
Huffman encoding of the LZ token payload. Its payload includes the decoded token
size and a compact serialized Huffman tree; blocks use it only when the complete
representation is smaller than both raw and plain-LZ alternatives.

Experimental modes `cblink` and `clblink` seed each block's match finder with
up to 65,535 prior decoded bytes. Linked LZ blocks use mode 4 and may reference
that bounded history; raw blocks remain valid history for following blocks.
Modes `cblinkh` and `clblinkh` additionally exact-select Huffman-coded linked
token payloads (mode 5), counting the serialized tree in the decision.
Modes `cblinks` and `clblinks` also consider a field-split entropy payload
(mode 6) with separate Huffman streams for controls, literals, distance-low,
distance-high, and lengths; all five stream headers and trees are counted.
Mode `clblinksg` uses the same mode-6 representation but limits lazy lookahead
to current matches shorter than 32 bytes.
Mode `clblinksd` additionally considers mode 7: a reversible byte-delta at one
adaptively selected stride from 1–8, followed by independent LZ and field-split
entropy coding. The stride byte and complete transformed payload are counted.
Mode `clblinksdf` uses the same representation with a start/middle/end sample
and conservative concentration gate to avoid low-confidence transformed parses.
Mode `clblinksopt` exact-selects a second mode-6 token stream from a bounded
dynamic parser using the parent block's split-Huffman field costs.
Mode `clblinksopt2` repeats cost estimation and parsing once, exact-selecting
the second optimized stream against the first.
Mode `clblinksopt2d` additionally considers, for each candidate length, the
lowest modeled-cost distance among all matches visited by the hash chain.
Mode `clblinksopt2df` fuses that match enumeration into the backward DP and
stores only the selected distance per position.
Mode `clblinksopt2dfs` shares one immutable match index across all optimized
parsing passes.
Mode `clblinksopt2dfsb` preserves those choices while bucketizing repeated
distance-cost comparisons. Mode `clblinksopt2dfsbc` additionally exact-selects
mode 8, which preserves Huffman
code lengths but uses a canonical code and compact packed/sparse length model
when that is smaller than the serialized tree for each split field.
Mode `clblinksopt2dfsbw` also exact-selects mode 9, with a persistent whole-file
match index, 24-bit distances, 16-bit lengths, and seven compact-canonical split
streams. Existing modes retain their 64 KiB window and byte-identical formats.
Mode `clblinksopt2dfsbwa` also exact-selects mode 10, which independently chooses
adaptive arithmetic coding or compact Huffman for each of the seven wide fields.
Mode `clblinksopt2dfsbwa1` also exact-selects mode 11, adding adaptive order-1
arithmetic coding as a third per-field choice.
Mode `clblinksopt2dfsbwac` also exact-selects mode 12, coding literals with four
reconstructed-previous-byte contexts and no stored context tags.
Mode `clblinksopt2dfsbwas` also exact-selects mode 13, adding a bounded static
frequency arithmetic model as a third per-field choice.
Mode `clblinksopt2dfsbwasp` adds an exact-selected parse using Q8 Shannon costs
from those static arithmetic models; the decoder format is unchanged.
Mode `clblinksopt2dfsbwasp2` repeats that static-cost estimation and parse once,
again exact-selecting the fully encoded result.
Mode `clblinksopt2dfsbwasp2b` uses the same choices with 1 MiB block framing;
historical modes retain byte-identical 64 KiB blocks.
Mode `clblinksopt2dfsbwasp2b16` raises that fixed framing cap to 16 MiB, matching
the wide-history window and making the current benchmark files single-block.
Mode `clblinksopt2dfsbwasp2b16x` doubles the wide longest-match chain to 512 and
the entropy-costed distance chain to 64, with all other bounds unchanged.
Mode `clblinksopt2dfsbwasp2b16x2` raises the wide longest-match chain to 2,048
and the entropy-costed distance chain to 128 in the retained size-max build.
Mode `clblinksopt2dfsbwasp2b16x2t` additionally exact-selects mode 14: a
sample-selected stride delta followed by a block-local 256/32 wide arithmetic
parse with one static-cost pass.
Mode `clblinksopt2dfsbwasp2b16x2td` additionally exact-selects mode 15, which
context-codes distance bytes by match-length state (4, 5, 6, or at least 7).
Mode `clblinksopt2dfsbwasp2b16x2tdl` additionally exact-selects mode 16, using
a full previous-byte-conditioned adaptive bit tree for unmatched literals.
Mode `clblinksopt2dfsbwasp2b16x2tdlm` additionally exact-selects mode 17, using
the last match distance as a matched-literal bit predictor until first mismatch.
Mode `clblinksopt2dfsbwasp2b16x2tdlmm` additionally exact-selects mode 18,
mixing that model with a gradually trusted coarse two-byte literal context.
Mode `clblinksopt2dfsbwasp2b16x2tdlmmdc` additionally exact-selects mode 20,
using an eight-decision binary control context and a fallback-mixed full
order-2 literal model.
Mode `clblinksopt2dfsbwasp2b16x2tdlmmdcr` additionally exact-selects mode 22,
using a four-entry move-to-front recent-distance cache. Repeat matches emit a
cache code and omit all three raw distance bytes; cache misses retain split
24-bit distance streams. This is the EXP-0058 size-max endpoint.
Mode `clblinksopt2dfsbwasp2b16x2tdlmmdcra4` additionally exact-selects mode 23
for blocks with at most eight distinct bytes. It serializes the alphabet and
adaptive-arithmetic-codes ranks with an order-4 context. The generic gate and
complete-payload comparison avoid file-type assumptions and guarantee that the
new representation cannot enlarge an encoded block. This is the EXP-0068
parent of the current size-max endpoint.
Mode `clblinksopt2dfsbwasp2b16x2tdlmmdcra4cm3` additionally exact-selects mode
24 for generic literals. It mixes the retained order-1 and exact order-2 bit
models with cautiously trusted hashed order-3 and four-character word-suffix
contexts. Contexts are reconstructed from decoded content, not filenames or
extensions. The complete mode-24 payload is compared against mode 22 and all
older candidates, so it cannot enlarge a block. This is the EXP-0074 size-max
endpoint.
Mode `clblinksopt2dfsbwasp2b16x2tdlmmdcra4f` is the EXP-0072 fast/high-ratio
preset. For whole-file blocks with at most eight symbols it encodes mode 23
first and returns immediately when that payload beats raw storage. For other
inputs it uses 1,024/16 match-search depths and encodes only the final
recent-distance wide candidate alongside the legacy fallbacks. It introduces
no new decoder format, but unlike the size-max mode it does not compare every
possible representation and therefore can produce larger output on some files.
Decoder `dwork` builds the 8-bit Huffman lookup only when estimated tree-edge
work exceeds one residual edge per symbol plus worst-case table construction.

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
