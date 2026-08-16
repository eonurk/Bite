# Bite compression research

Bite is a deliberately small lossless-compression research implementation. The
initial codec is a 64 KiB-window LZSS design with a bounded hash-chain match
finder and grouped literal/match control bits. It is intended for controlled
experiments, not production use.

## Reproduce

```sh
make
make corpus
make test
python3 scripts/benchmark.py --repeats 3
```

The development corpus combines deterministic heterogeneous fixtures with the
public Canterbury corpus. `scripts/fetch_canterbury.sh` pins the archive by
SHA-256. The generated held-out corpus is produced only with the explicit
`--held-out` flag and is not part of routine experiments.

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
