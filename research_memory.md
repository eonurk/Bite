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
- `EXP-0005` linked-history blocks are retained in `cblink`/`clblink`. Seeding
  each block with one 65,535-byte prior window reduces independent-lazy size
  2.286% to 4,324,638 bytes, keeps match state bounded, and leaves decode speed
  unchanged. Encode throughput falls 20.386% and RSS rises 4.734% versus
  independent lazy because prior history is re-indexed for each block.
- `EXP-0006` combines linked history and static Huffman in `cblinkh`/`clblinkh`.
  The lazy mode is Bite's current minimum-size point: 3,920,565 bytes (3.492
  bits/byte), 25.60 MB/s encode, 56.21 MB/s decode, and 6.90 MB encoder peak
  RSS. It is 0.523% smaller than gzip-1, but zstd-1 remains smaller and faster.
- `EXP-0007` field-split entropy mode `clblinks` is Bite's new minimum-size
  point: 3,615,800 bytes (3.220 bits/byte), 21.71 MB/s encode, 54.27 MB/s decode,
  and 6.36 MB encoder RSS. Five exact-counted order-0 alphabets reduce linked
  joint-Huffman size 7.773% with no per-file size regression.
- `EXP-0008` gated lazy mode `clblinksg` is retained beside full max lazy. It
  limits lookahead to current matches under 32 bytes, gaining 8.188% validated
  encode throughput for 0.087% aggregate size growth and 2.344% lower encoder
  RSS. It is a neighboring internal Pareto point, not the new default.
- `EXP-0009` adaptive delta-stride mode `clblinksd` is retained provisionally.
  It totals 2,357,354 bytes (2.099 bits/byte), 18.78 MB/s encode, 58.21 MB/s
  decode, and 5.44 MB encoder RSS. On the frozen development corpus it creates
  a size/encode frontier point between xz-6 and xz-1, but broader raw/scientific
  evidence is required because the largest gain is a synthetic RGB gradient.
- `EXP-0010` adds an 8-bit Huffman prefix lookup for streams with at least 256
  decoded symbols, retaining tree fallback and `dslow` as a reproducible control.
  On identical mode-7 streams, five-repeat decode throughput improves 10.837%
  to 64.04 MB/s with unchanged size, encode speed, binary size, and RSS.

## What consistently fails

- `EXP-0004` two-byte nearby/short matches with four-byte escapes was rejected.
  Exact selection improved aggregate size 1.161%, but computing a second parse
  cut encode throughput 37.960%, decoder throughput fell 7.119%, and RSS rose
  5.900%. Almost all size benefit came from the generated pseudo-executable;
  only four small Canterbury files changed. The implementation was reverted.
- `EXP-0011` four literal Huffman streams keyed by the previous byte's top two
  bits was rejected. Exact selection improved size only 0.258%, while encode
  throughput fell 11.319%, decode throughput fell 4.535%, and encoder RSS rose
  19.940%. Mode 8 won 29 of 152 blocks, but one generated pseudo-text file
  contributed 83.6% of the 6,084-byte gain. The implementation was reverted.
- `EXP-0012` persistent circular match chains with 64-bit absolute hash heads
  was rejected. It produced byte-identical streams to the rebuilt-history
  parent but reduced encode throughput 7.132%. The larger hot head table and
  ring predecessor indirection outweighed saved history insertion. Generated
  source improved 2.15%, but almost every large file slowed.

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
- Delta-stride mode changes only generated fixtures: RGB -99.655%, scientific
  -10.593%, and three small stride-pattern files -31% to -74%. No Canterbury
  file changes. Selected transformed blocks use stride 1 (3 blocks), stride 3
  (18), or stride 8 (13), matching the mechanism's intended structures.
- Generated pseudo-executable data is 4.340 bits/byte in Bite versus 4.003 in
  LZ4, 1.966 in zstd-3, and 0.392 in xz-6. A 64 KiB window and fixed match
  representation miss strong long/structured repetition.
- Coarse previous-byte literal contexts help selectively rather than broadly:
  five Canterbury files improved by 992 bytes total, while generated
  `pseudo_text.bin` improved 5,088 bytes and 17 of 26 files were unchanged.
  Multiple Huffman trees cost too much for this small, concentrated gain.
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
   distance, length, and literal bytes in one alphabet. `EXP-0007` fixes field
   mixing in mode 6. `EXP-0011` shows that adding four independent static
   literal trees for coarse byte context costs much more than its small gain;
   distance recency and length/distance correlation remain unmodeled.
2. Greedy parsing chooses the locally longest match and every match costs three
   bytes regardless of distance/length. It cannot trade a literal or short
   match for a materially better next match. Bounded single-hash chains also
   limit match choice.
   `EXP-0008` shows a fixed length gate recovers some search cost, but its
   dataset interaction is sharp and a better cost-aware gate remains open.
3. V1 stores a four-byte predecessor per input byte and has a 2 GiB limit. V2
   bounds match-state allocation to 64 KiB and cuts measured encoder RSS by
   44.8%, but file I/O and final output are still whole-file buffers rather than
   streaming. The process-per-file harness also conflates startup latency with
   steady-state throughput on tiny files.
4. V2 independent blocks lose cross-boundary history. Size regresses 7.106% on
   Canterbury, 7.220% on JSON, 9.101% on source, and 9.164% on repetitive text
   relative to v1 lazy, despite improving total heterogeneous-corpus size.
   `EXP-0005` linked mode recovers nearly all of this ratio, but its repeated
   history indexing is slower than both independent v2 and whole-file v1.
   `EXP-0012` shows that straightforward persistence is also slower when it
   doubles hash-head width and adds circular predecessor indirection.
5. Huffman decoding now has an 8-bit prefix fast path, but table construction
   still adds overhead to streams that barely meet the 256-symbol gate. Binary,
   executable, image, and tiny-file category decode rates regress 1–5% while
   entropy-heavy categories improve strongly.
6. Linked Huffman improves size but compounds costs: versus independent Huffman
   it is 22.589% slower to encode, 5.227% slower to decode, and uses 18.927%
   more encoder RSS. It does not create an external frontier point.
7. Split entropy serializes and decodes five trees per selected block. It costs
   19.257% encode and 7.651% decode throughput versus joint linked Huffman; the
   bit-at-a-time tree walker magnifies this overhead.
8. The delta prefilter scans up to eight transformed histograms and sometimes
   runs an extra parse. Encode throughput falls 20.374% even on the aggregate;
   mode selection protects size but not compute. Its apparent external frontier
   point is not yet robust beyond generated raw/scientific fixtures.

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

Ranking after `EXP-0004` (first item completed by `EXP-0005`):

1. Linked-history v2 blocks to recover cross-boundary compression while keeping
   raw fallback and bounded match state.
2. Split entropy alphabets/models for token controls, literals, distances, and
   lengths.
3. Gated lazy parsing to retain most size benefit with fewer lookahead searches.
4. `EXP-0005` reversible stride/byte-plane transforms.
5. Faster Huffman decoding through canonical/table lookup without changing the
   probability model.

Ranking after `EXP-0005` (first item completed by `EXP-0006`):

1. Combine linked-history parsing with exact-selected static Huffman coding.
   This directly tests whether the two strongest size mechanisms compound.
2. Split entropy alphabets/models for token controls, literals, distances, and
   lengths.
3. Gated lazy parsing to retain most ratio gain with fewer lookahead searches.
4. Reversible stride/byte-plane transforms for scientific and raw image blocks.
5. Faster Huffman decoding through canonical/table lookup.

Ranking after `EXP-0006` (first item completed by `EXP-0007`):

1. Split token controls, literals, distances, and lengths into separate entropy
   alphabets or contexts while preserving exact block selection.
2. Gated lazy parsing to reduce needless next-match searches.
3. Reversible stride/byte-plane transforms for scientific and raw image blocks.
4. Faster canonical/table Huffman decoding for the retained entropy modes.
5. Persistent circular match chains across blocks to avoid linked-history
   re-indexing while preserving bounded memory.

Ranking after `EXP-0007` (first item completed by `EXP-0008`):

1. Gated lazy parsing to avoid next-match searches when the current match is
   already long or local match density is low.
2. Reversible stride/byte-plane transforms for scientific and raw image blocks.
3. Faster canonical/table Huffman decoding, now more valuable because mode 6
   may decode five streams per block.
4. Context-split literal models (for example previous-byte classes) within the
   literal stream, with exact table-cost selection.
5. Persistent circular match chains across linked blocks.

Ranking after `EXP-0008` (first item completed by `EXP-0009`):

1. Reversible stride/byte-plane transforms for scientific and raw image blocks.
2. Faster canonical/table Huffman decoding for multi-stream mode 6.
3. Context-split literal models within the isolated literal stream.
4. Persistent circular match chains across linked blocks.
5. Cost-aware lazy gating using estimated token/entropy cost rather than a fixed
   match-length threshold.

Ranking after `EXP-0009` (first item completed by `EXP-0010`):

1. Faster canonical/table Huffman decoding for multi-stream modes 6 and 7.
2. Context-split literal models within the isolated literal stream.
3. Persistent circular match chains across linked blocks.
4. Cheaper transform prefiltering that estimates stride benefit from a sample
   rather than eight full-block histogram passes.
5. Cost-aware lazy gating.

Ranking after `EXP-0010` (first item completed by rejected `EXP-0011`):

1. Context-split literal models within the isolated literal stream.
2. Persistent circular match chains across linked blocks.
3. Cheaper sampled transform prefiltering.
4. Cost-aware lazy gating.
5. Adaptive Huffman lookup eligibility based on stream length and tree/code
   depth, avoiding table construction for low-benefit distributions.

Ranking after `EXP-0011` (first item completed by rejected `EXP-0012`):

1. Persistent circular match chains across linked blocks, eliminating repeated
   history indexing without changing the retained token representation.
2. Cheaper sampled transform prefiltering to avoid eight full-block histogram
   passes and an unprofitable extra parse.
3. Cost-aware lazy gating using token/entropy estimates instead of a fixed
   match-length threshold.
4. Adaptive Huffman lookup eligibility based on stream length and estimated
   tree-walk work.
5. Distance recency or length/distance correlation modeling within the split
   token representation, with complete model costs counted.

Current ranking after `EXP-0012`:

1. Cheaper sampled transform prefiltering to avoid eight full-block histogram
   passes and unprofitable transformed parses.
2. Cost-aware lazy gating using token/entropy estimates instead of a fixed
   match-length threshold.
3. Adaptive Huffman lookup eligibility based on stream length and estimated
   tree-walk work.
4. Distance recency or length/distance correlation modeling within the split
   token representation, with complete model costs counted.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, but
   only after profiling confirms the wider head table caused `EXP-0012`.

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
- Linked history and independent blocks expose a clean ratio/cache tradeoff.
  Linked size improves 6.586% on Canterbury, 6.649% JSON, 8.233% source, 8.052%
  text, and 4.142% XML versus independent lazy, with no category regression.
  It is only 0.052–0.375% larger than v1 lazy on those structured categories.
- Re-indexing the prior 65,535 bytes costs 20.386% encode throughput. A future
  streaming/circular match finder should preserve chains across block headers
  rather than rebuilding history.
- Linked history and Huffman partially overlap rather than add independently.
  Linked Huffman is 2.002% smaller than independent Huffman, not the 2.286% raw
  linked gain plus 9.606% Huffman gain. The combined mode still improves
  Canterbury 6.459%, JSON 5.089%, source 7.436%, text 14.834%, and XML 6.025%
  versus independent Huffman. Canterbury `ptt5` regresses only four bytes.
- Field separation provides a larger, broad gain: versus linked joint Huffman
  it improves Canterbury 17.133%, pseudo-executable 29.475%, JSON 15.461%,
  mixed 5.512%, scientific 3.078%, source 14.686%, text 36.281%, and XML
  28.682%. Random/RGB/small blocks safely retain earlier representations.
- Scientific data is notable: split entropy makes an LZ-derived representation
  24,625 bytes smaller than raw framing even though plain and joint-Huffman LZ
  previously selected raw. Token fields exposed structure hidden when mixed.
- Gating lazy lookahead at 32 bytes is a useful but dataset-sensitive policy.
  It makes generated source 9.318% larger while making generated text 12.066%
  smaller; Canterbury grows only 0.074%, and other categories are size-neutral.
  Encode throughput improves broadly, including Canterbury 10.571%, JSON
  11.645%, source 20.444%, text 37.137%, and XML 8.960%.
- Delta-stride and split entropy interact extremely strongly on low-order raw
  structure: the RGB gradient falls from 1,179,826 to roughly 4 KB. Scientific
  readings fall by 82,151 bytes. Exact selection prevents all size regressions,
  but the mechanism does not help the public Canterbury corpus.
- Prefix lookup gains concentrate where split streams are large and entropic:
  scientific decode +50.69%, JSON +23.44%, XML +22.47%, Canterbury +9.42%,
  source +8.90%, and text +6.20%. The same table threshold is mildly harmful
  on binary (-2.13%), pseudo-executable (-5.30%), image (-1.14%), and tiny files
  (-1.98%), motivating distribution-aware eligibility rather than a size gate.
- Exact selection again protects size but not compute in `EXP-0011`: building
  four extra literal trees for every block costs 11.319% encode throughput even
  though the new representation wins only 29 of 152 blocks. Its decode cost is
  paid only on selected blocks, yet eight streams still reduce decode 4.535%.
- Rebuilt and persistent match chains can produce exactly the same token stream,
  so their speed difference is purely implementation architecture. In
  `EXP-0012`, avoiding repeated insertion was insufficient to pay for 64-bit
  heads and circular predecessor lookup; compact state is a prerequisite for
  reconsidering persistence.

## Known benchmark artifacts

- Process startup dominates very small per-file timings; aggregate throughput must
  not be interpreted as steady-state codec throughput.
- A five-repeat Bite rerun reproduced compressed size and peak RSS exactly.
  Encode throughput changed -0.20%; decode throughput changed +2.01%. Treat
  timing deltas below 3% as noise unless additional repetitions confirm them.
- `EXP-0008` decode timing changed from -0.393% in the screen to +8.600% in
  validation even though the decoder was unchanged and stream size barely
  moved. Do not credit this decode result without an in-process benchmark.
- `EXP-0011` screen timing incorrectly suggested the strictly-more-expensive
  encoder was 18.8% faster. Five repeats instead measured an 11.3% slowdown,
  reinforcing that one-repeat screens decide whether to validate size/correctness
  signals, not whether to credit modest timing changes.
- `EXP-0012` parent and candidate compressed streams were byte-identical, yet
  their measured decode rates differed -2.772% over five repeats. This directly
  bounds such process-level decode deltas as noise, not decoder effects.
- Per-file medians are summed, so aggregate time weights startup once per file.
  A later in-process/library benchmark is needed before making startup or
  steady-state speed claims.
- Routine experiments must use staged evaluation: one-repeat new-mode/parent
  screening, then 3–5 repeat validation only for promising candidates. Reuse a
  cached full external audit while tool, platform, corpus hashes, compiler, and
  benchmark method remain unchanged; refresh periodically or after changes.
- `EXP-0005` validated this workflow: the two-mode screen took 6.9 seconds and
  the three-repeat validation 6.7 seconds while 13 external codec summaries
  were fingerprint-validated from `EXP-0004`. Fresh timing remained serial.
- Three committed `EXP-0005-screen` artifacts unexpectedly disappeared during
  the `EXP-0006` cycle despite no results-cleaning command. They were restored
  byte-for-byte from Git before documentation; all result directories were then
  audited for the required metadata, per-file, and summary files.
- Cache compatibility originally included `uname`'s mutable hostname. A transient
  hostname change correctly stopped `EXP-0009`, but the corpus/kernel/tools were
  identical. Fingerprinting now ignores node name while retaining OS, kernel
  build, architecture, corpus hashes, and external tool versions.
- The generated RGB gradient is a known favorable artifact for fixed-stride
  transforms. Do not present the `EXP-0009` development frontier as a general
  compressor result until public raw/scientific or held-out evidence confirms it.
- The generated corpus is deterministic and its manifest is hashed. Canterbury
  is pinned to archive SHA-256
  `f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb`.
- Held-out generation uses a separate seed and is only invoked explicitly with
  `--held-out`; it has not been generated or inspected during initial tuning.

## Open hypotheses

- Fixed-stride delta structure explains much of the previous xz advantage on
  generated floating-point and RGB data; generalization beyond these fixtures
  remains open.
- A cost-aware lazy gate may avoid the generated-source ratio regression of the
  fixed 32-byte policy while retaining its broad encode-speed gain.
- Literal context may still be useful only if its coding tables are shared or
  much cheaper than four independent serialized Huffman trees; do not retry the
  rejected top-two-bit split without a mechanism that addresses table/compute
  cost and the pseudo-text concentration.
- Huffman lookup should be enabled by estimated decoded bit work, not decoded
  symbol count alone, to avoid table-build regressions on shallow/skewed trees.

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
