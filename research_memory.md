# Compression research memory

## What currently works

- `BASE-0001` is a correct, readable 64 KiB-window LZSS codec. It uses a
  65,536-entry last-position hash table, per-input hash chains capped at 64
  probes, greedy parsing, 4..259-byte matches, and eight-token control groups.
- The v1 decoder is strict about magic/version, truncation, invalid offsets,
  decoded size, and trailing data.
- All 26 development files (8,982,882 bytes across Canterbury plus generated
  heterogeneous fixtures) round-trip byte-for-byte with SHA-256 verification.
- The benchmark records per-file and aggregate size, bits/byte, ratio, median
  encode/decode time, throughput, peak RSS, binary size, exact tool versions,
  corpus hashes, and correctness. Any mismatch aborts the run.
- Baseline Bite totals 4,678,316 bytes (4.166 bits/byte), 40.0 MB/s encode,
  76.5 MB/s decode, 9.99 MB encoder peak RSS, and 3.90 MB decoder peak RSS.
- The strongest relative Bite results are highly repetitive generated text,
  source, JSON, and XML. Despite its simple coding, it beats LZ4's size there.
- `EXP-0001` one-token lazy parsing is retained as optional mode `cl`. It
  reduces total size 0.937% with no per-file size regression, while reducing
  encode throughput 18.99%. Decoder format, decode speed, and memory are
  unchanged. It is useful as a higher-compression research baseline but does
  not create an external Pareto point.
- `EXP-0002` v2 blocked modes are retained as the balanced architecture. The
  lazy variant totals 4,425,821 bytes (3.942 bits/byte), 39.57 MB/s encode,
  80.07 MB/s decode, and 5.51 MB encoder peak RSS. Relative to v1 lazy this is
  4.503% smaller, 24.273% faster to encode, 3.549% faster to decode, and 44.828%
  lower encoder RSS. The v1 modes remain useful where cross-block matches matter.
- `EXP-0003` static Huffman coding over complete LZ token payloads is retained
  in `cbh`/`clbh` high-compression modes. Blocked lazy size falls 9.606% to
  4,000,675 bytes (3.563 bits/byte), with encode throughput 31.74 MB/s and decode
  55.79 MB/s. Exact block selection guarantees no stored-size regression.

## What consistently fails

- `EXP-0004` two-byte nearby/short matches with four-byte escapes was rejected.
  Exact selection improved aggregate size 1.161%, but computing a second parse
  cut encode throughput 37.960%, decoder throughput fell 7.119%, and RSS rose
  5.900%. Almost all size benefit came from the generated pseudo-executable;
  only four small Canterbury files changed. The implementation was reverted.

## Dataset-specific observations

- Canterbury: Bite 2.993 bits/byte versus LZ4 3.499, zstd-3 1.806, xz-6
  1.401. Matches are useful, but the literal/token representation leaves a
  large entropy-coding gap.
- Generated structured data compresses well with raw matches: Bite reaches
  0.434 bits/byte on repetitive text, 1.260 on source, 1.635 on JSON, and
  1.472 on XML. zstd-3 remains substantially smaller on each.
- Uniform random expands to 9.000 bits/byte; the fixed one-control-bit cost is
  visible exactly. Scientific doubles and the raw RGB gradient also remain
  near 9 bits/byte, while xz-6 exploits byte-plane/stride structure in both.
- In v2, all 2,504,057 bytes from random, scientific, and RGB categories select
  raw blocks. They shrink by about 11% relative to v1, leaving only file and
  block framing overhead rather than token-control expansion.
- Generated pseudo-executable data is 4.340 bits/byte in Bite versus 4.003 in
  LZ4, 1.966 in zstd-3, and 0.392 in xz-6. A 64 KiB window and fixed match
  representation miss strong long/structured repetition.
- Huffman token coding is broadly effective on compressible categories:
  Canterbury -22.057%, pseudo-executable -28.925%, JSON -13.918%, source
  -20.906%, repetitive text -27.784%, and XML -21.684% versus v2 lazy. It has
  no size effect on raw-selected random/scientific/RGB blocks or small files.
- Process startup dominates the six generated small files: their aggregate
  Bite throughput is only about 0.23 MB/s. Size overhead is 16-byte header plus
  token control bytes.

## Architecture weaknesses

1. V1 has no raw-block fallback. V2 fixes incompressible expansion and
   `EXP-0003` adds order-0 entropy coding, but the model mixes control, match,
   distance, length, and literal bytes in one alphabet. It cannot exploit
   context-dependent literal probabilities or token-field-specific statistics.
2. Greedy parsing chooses the locally longest match and every match costs three
   bytes regardless of distance/length. It cannot trade a literal or short
   match for a materially better next match. Bounded single-hash chains also
   limit match choice.
3. V1 stores a four-byte predecessor per input byte and has a 2 GiB limit. V2
   bounds match-state allocation to 64 KiB and cuts measured encoder RSS by
   44.8%, but file I/O and final output are still whole-file buffers rather than
   streaming. The process-per-file harness also conflates startup latency with
   steady-state throughput on tiny files.
4. V2 independent blocks lose cross-boundary history. Size regresses 7.106% on
   Canterbury, 7.220% on JSON, 9.101% on source, and 9.164% on repetitive text
   relative to v1 lazy, despite improving total heterogeneous-corpus size.
5. The experimental Huffman decoder walks one tree edge per bit. Its 20.754%
   decode-throughput cost prevents the size gain from creating an external
   Pareto point; table-based or canonical decoding is an optimization target.

## Promising unexplored directions

Completed initial ranking:

1. `EXP-0001` — one-token lazy parsing: emit a literal when the next-position
   match is at least two bytes longer than the current match. This isolates
   parsing quality while preserving the format and decoder.
2. `EXP-0002` — independent blocks with a raw-block mode. This should eliminate
   pathological expansion and bound working memory; per-block selection tests
   whether mixed files benefit without filename/type special cases.
3. `EXP-0003` — block-static canonical Huffman coding for literals, with an
   explicit raw/Huffman mode decision including table cost. This measures the
   literal-entropy gap on Canterbury and structured text.
4. `EXP-0004` — distance/length classes with short encodings for nearby matches
   and varint escape forms. This tests whether fixed three-byte matches are the
   main representation bottleneck.
5. `EXP-0005` — reversible stride/byte-plane transform candidates selected by
   sampled estimated cost per block. This targets scientific and raw image data
   while remaining data-driven and general-purpose.

Ranking after `EXP-0002` (first item now completed):

1. `EXP-0003` — block-static literal entropy coding with explicit table cost
   and raw fallback. The strongest remaining evidence is the large Canterbury
   and structured-text gap even when matches are plentiful.
2. `EXP-0004` — variable distance/length coding, especially cheap nearby
   matches, to address fixed three-byte token cost.
3. Linked-history v2 blocks — preserve independent block decisions and raw mode
   while allowing matches into the immediately preceding 64 KiB history.
4. Gated lazy parsing — avoid the `EXP-0001` extra search on long current matches
   or low-match-density data.
5. `EXP-0005` — sampled reversible byte-plane/stride transforms for scientific
  and raw image blocks.

Ranking after `EXP-0003` (`EXP-0004` now rejected):

1. `EXP-0004` — variable match coding with cheap forms for short distances and
   common lengths. Fixed three-byte matches remain a direct format inefficiency.
2. Linked-history v2 blocks to recover the 4–9% cross-boundary loss while
   preserving raw fallback and bounded state.
3. Split entropy alphabets/models for control bytes, literals, distances, and
   lengths, retaining exact mode-cost selection.
4. Gated lazy parsing to recover the 13–19% encode cost of unconditional
   lookahead.
5. `EXP-0005` reversible stride/byte-plane transforms.

Current ranking after `EXP-0004`:

1. Linked-history v2 blocks to recover cross-boundary compression while keeping
   raw fallback and bounded match state.
2. Split entropy alphabets/models for token controls, literals, distances, and
   lengths.
3. Gated lazy parsing to retain most size benefit with fewer lookahead searches.
4. `EXP-0005` reversible stride/byte-plane transforms.
5. Faster Huffman decoding through canonical/table lookup without changing the
   probability model.

## Interactions between components

- Lazy parsing interacts strongly with structured/repetitive text: size changes
  were Canterbury -2.261%, JSON -3.174%, source -3.046%, XML -4.555%, and
  generated text -11.184%. It had no effect on random, raw image, scientific,
  or small fixtures because those inputs present few qualifying match choices.
- The extra match search costs encode throughput even when it finds no useful
  alternative. `EXP-0001` throughput fell 18.99% overall, suggesting a future
  lazy policy should cheaply gate lookahead by current match length or local
  match density.
- Block reset interacts with both speed and size. Short chains stay cache-local,
  making v2 lazy 24.3% faster than v1 lazy, but discarded history costs 4–9%
  on compressible categories. This makes linked-history blocks promising if
  match-state memory can remain bounded.
- Raw fallback was selected for 42 of 152 blocks (2,504,057 decoded bytes); 110
  LZ blocks covered the remaining 6,478,825 bytes. Selection cleanly separated
  the generated random/scientific/RGB categories without type-specific logic.
- Huffman was selected for 108 of the 110 previously LZ-coded blocks; only two
  small-file blocks (5,120 decoded bytes) retained plain LZ. Counting the
  serialized tree prevented regressions on all files without heuristic cutoffs.
- Huffman compounds the lazy-parser gain but costs 12.902% encode and 20.754%
  decode throughput versus v2 lazy. It ends only 1.509% larger than gzip-1, but
  gzip-1 is still faster in both directions on this process-per-file benchmark.
- `EXP-0004` shows that exact block selection prevents size regressions but not
  computation regressions: generating two complete parses is too expensive for
  a representation chosen on only a few blocks. Future alternative token modes
  should share one parsed token sequence or use a cheap pre-decision.

## Known benchmark artifacts

- Process startup dominates very small per-file timings; aggregate throughput must
  not be interpreted as steady-state codec throughput.
- A five-repeat Bite rerun reproduced compressed size and peak RSS exactly.
  Encode throughput changed -0.20%; decode throughput changed +2.01%. Treat
  timing deltas below 3% as noise unless additional repetitions confirm them.
- Per-file medians are summed, so aggregate time weights startup once per file.
  A later in-process/library benchmark is needed before making startup or
  steady-state speed claims.
- Routine experiments must use staged evaluation: one-repeat new-mode/parent
  screening, then 3–5 repeat validation only for promising candidates. Reuse a
  cached full external audit while tool, platform, corpus hashes, compiler, and
  benchmark method remain unchanged; refresh periodically or after changes.
- The generated corpus is deterministic and its manifest is hashed. Canterbury
  is pinned to archive SHA-256
  `f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb`.
- Held-out generation uses a separate seed and is only invoked explicitly with
  `--held-out`; it has not been generated or inspected during initial tuning.

## Open hypotheses

- Byte-plane structure explains the unusually large xz advantage on generated
  floating-point and RGB data.
- Gating lazy lookahead to shorter current matches may retain most of its size
  gain while recovering enough encoder speed to approach a useful frontier.
- A one-block linked history may recover most v2 compressible-data regression
  without returning to v1's per-input predecessor allocation.
- Separating literal and match-field entropy streams may outperform the current
  single order-0 token-byte alphabet without a more complex probability model.

## Initial external Pareto frontier (2026-08-16)

All points use 26 files, three repetitions, summed per-file median times. The
encode size/speed frontier is: xz-6 (2,242,316 B, 5.67 MB/s), xz-1 (2,853,460 B,
20.09 MB/s), Brotli-6 (3,250,726 B, 34.19 MB/s), zstd-6 (3,500,213 B,
50.14 MB/s), zstd-3 (3,579,507 B, 54.37 MB/s), Brotli-1 (3,743,568 B,
58.03 MB/s), and LZ4 (4,676,048 B, 70.16 MB/s).

The size/decode-speed frontier is: xz-9 (2,242,316 B, 39.40 MB/s), Brotli-11
(2,795,544 B, 51.51 MB/s), Brotli-6 (3,250,726 B, 57.19 MB/s), zstd-6
(3,500,213 B, 63.73 MB/s), gzip-1 (3,941,192 B, 70.64 MB/s), LZ4 (4,676,048 B,
72.69 MB/s), and Bite (4,678,316 B, 76.51 MB/s). The small Bite/LZ4 decode
difference needs an in-process benchmark before it is considered meaningful.
