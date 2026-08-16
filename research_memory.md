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
- `EXP-0013` retains mode 7 through `clblinksdf` but replaces nine full-block
  concentration scans with at most 4,095 sampled bytes from the block's start,
  middle, and end. A 1.2x gate reduces transformed parses from 52 to 36 while
  retaining all 34 winning dev blocks and identical compressed streams. Five-
  repeat encode throughput improves 15.261% to 20.96 MB/s.
- `EXP-0017` retains optional decoder `dwork`, which builds the 256-entry
  Huffman prefix table only when encoded-bit work exceeds one residual edge per
  symbol plus 2,048 worst-case construction edges. Two reversed-order,
  30-repeat in-process runs improve identical-stream dev decode by 3.038% and
  3.746%, and public research by 1.217% and 0.622%. Size, format, encoder,
  binary size, and process-level RSS are unchanged.
- `EXP-0018` retains maximum-ratio mode `clblinksopt`. It derives literal,
  distance, and length bit costs from the parent block's actual split-Huffman
  trees, precomputes the longest match at every position, and runs a backward
  dynamic program over literals plus the eight longest match lengths. The
  result is re-Huffman-coded and exact-selected as ordinary mode 6. Dev size
  falls 2.509% to 2,298,212 bytes; public research falls 3.616% to 4,154,139
  bytes, with all 16 public files improving and no file on either suite
  regressing. It is intentionally slow: encode throughput falls 70–74%.
- `EXP-0020` retains internal maximum-ratio mode `clblinksopt2`. It rebuilds
  split-field costs from EXP-0018's optimized parse and runs the bounded DP once
  more, exact-selecting the second result. Dev size falls another 0.482% to
  2,287,142 bytes (15/26 files improve, none regress); public research falls
  0.498% to 4,133,447 bytes (14/16 improve, none regress). Encode throughput
  falls 39.8–42.3% and encoder RSS rises 7.2–11.3% versus EXP-0018. It is a
  useful convergence result, but remains externally dominated on dev.
- `EXP-0021` retains internal maximum-ratio mode `clblinksopt2d`. For each of
  the DP's eight candidate lengths, it chooses the lowest modeled-cost distance
  among up to 64 hash-chain matches. Dev size falls another 0.737% to 2,270,290
  bytes (15/26 improve, none regress); public research falls 0.767% to 4,101,724
  bytes with all 16 files improving. Encode throughput falls 68.1% on dev and
  47.6% on research, while RSS rises 62.4% and 11.3%. Startup-free dev decode
  changes only -0.377%; format and binary size are unchanged. The mode remains
  externally dominated on dev.
- `EXP-0022` retains memory-frontier mode `clblinksopt2df`. It generates streams
  byte-identical to EXP-0021 on all 43 observed files, but enumerates candidates
  during the backward DP and stores one selected offset rather than eight per
  position. Three-repeat encoder RSS falls 11.931% on dev and 10.253% on public
  research. Encode changes -0.091%/+0.297%, both noise; size, decode format, and
  binary behavior are unchanged.
- `EXP-0023` retains speed-frontier mode `clblinksopt2dfs`. It builds one
  immutable predecessor-chain/longest-match index per block and reuses it across
  all three optimized parses. Streams are byte-identical to EXP-0022 on all 43
  observed files. Three-repeat encode throughput improves 15.709% on dev and
  25.347% on public research, for RSS changes of +0.746% and +3.777%. Size,
  format, and decode behavior are exact controls.
- Routine candidate evaluation now preserves the original frozen dev corpus and
  adds a separately addressable 13,787,888-byte public research suite: 13
  non-duplicate Calgary files plus the three-file Large corpus. Both archives
  have pinned SHA-256 hashes, and the suite has no exact overlap with dev or
  Silesia. Silesia remains excluded from tuning.
- A complementary in-process runner now loads each file once, performs one
  untimed warm-up, and records per-file medians across repeated encode/decode
  calls with direct byte comparisons every time. At 20 repeats, retained
  `clblinksdf` measures 29.94 MB/s encode and 277.29 MB/s decode on dev, and
  14.80/196.73 MB/s on public research. Compressed totals exactly match the
  process harness. The runner deliberately leaves RSS/startup/external audits
  to the existing process harness.

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
- `EXP-0014` a local raw-token-density lazy rule was rejected by the new public
  research screen. It appeared 44 bytes smaller on dev, but grew 1,407 bytes
  on the research suite, regressed 12 of 16 files, and reduced encode throughput
  6.723%. A one-step 25-bit match versus 33-bit literal-plus-match proxy does
  not represent downstream entropy cost or parsing consequences.
- `EXP-0015` modulo deltas between consecutive 16-bit match distances was
  rejected. Its exact-selected representation won zero blocks across all 42
  dev+research files, so sizes were unchanged, while generating five extra
  Huffman streams reduced encode throughput 8.870% on dev and 6.547% on the
  public suite. Simple first-order distance recency is not useful here.
- `EXP-0016` work-estimated Huffman lookup eligibility was rejected as
  inconclusive. On identical streams and five repeats it improved aggregate
  decode 1.736% on dev and 0.712% on public research, both below the established
  process-level noise threshold. Direction was broad (21/26 and 13/16 files),
  but several files still regressed and the policy was reverted without tuning.
  This was a measurement rejection, not an algorithmic failure; unchanged
  `EXP-0017` later retained it after startup-free repeated evidence.
- `EXP-0019` gating the bounded parser on whether the parent split-Huffman
  representation beats raw storage was rejected. It preserved every observed
  compressed stream and improved five-repeat dev encode throughput 4.239%, but
  random, pseudo-text, and RGB generated fixtures contributed 64.6 ms saved
  against only 59.1 ms aggregate. Public research encode throughput regressed
  0.578%. This gate skips work mainly on synthetic/raw-selected blocks rather
  than reducing the parser's broad compressible-data cost.

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
- The generated 15-file hold-out is not independent: 12 files, representing
  4,788,364 of 6,172,098 bytes, are byte-identical to development fixtures. Its
  favorable aggregate is only a deterministic seed-stability check.
- On the public 211,938,580-byte Silesia corpus, frozen EXP-0010 produces
  67,070,271 bytes at 19.06 MB/s encode and 151.80 MB/s decode. It is dominated
  in both size and speed by zstd-3 (66,216,569 bytes), Brotli-6 (58,441,670),
  and xz-1 (58,717,040). The development frontier does not generalize.
- The EXP-0014 distribution shift is directionally decisive despite small
  aggregate byte changes: dev splits evenly at 8 improvements / 8 regressions,
  while the separate Calgary+Large suite has 4 improvements / 12 regressions.
  The apparent dev win came mainly from generated XML/JSON; public books and
  text generally worsened.
- Bounded entropy-cost parsing generalizes broadly. On dev, generated source
  improves 27.0%, JSON 10.0%, XML 11.2%, and large Canterbury text 3–4%. On
  public research, E. coli improves 4.52%, bible 3.87%, world 2.77%, and every
  Calgary file improves 0.29–3.65%. This is not driven by synthetic transforms.
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
   literal trees for coarse byte context costs much more than its small gain.
   `EXP-0015` rules out simple modulo distance deltas; length/distance
   correlation and distance classes remain unmodeled.
2. Greedy parsing chooses the locally longest match and every match costs three
   bytes regardless of distance/length. It cannot trade a literal or short
   match for a materially better next match. Bounded single-hash chains also
   limit match choice.
   `EXP-0008` shows a fixed length gate recovers some search cost, but its
   dataset interaction is sharp. `EXP-0014` shows a local raw-token density
   proxy does not generalize; actual entropy costs or a longer parsing horizon
   are required. `EXP-0018` demonstrates that block-derived field costs plus a
   whole-block bounded DP produce broad ratio gains, at substantial compute and
   memory cost. `EXP-0021` further shows that using only the longest match's
   distance leaves broad ratio on the table; length-specific entropy-costed
   distances help, but exhaustive chain evaluation is too slow and memory-heavy.
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
   point does not reproduce on Silesia. `EXP-0013` removes much of the selector
   cost, but the underlying ratio remains dependent on synthetic structure.

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

Ranking after `EXP-0012` (first item completed by retained `EXP-0013`):

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

Ranking after `EXP-0013` and the first Silesia audit (first item rejected by
`EXP-0014`):

1. Cost-aware lazy gating using token/entropy estimates instead of a fixed
   match-length threshold, targeting a broadly useful speed/ratio point.
2. Distance recency or length/distance correlation modeling within the split
   token representation, with complete model costs counted.
3. Adaptive Huffman lookup eligibility based on stream length and estimated
   tree-walk work.
4. Parse decisions based on actual split-entropy bit costs rather than longest
   match length, starting with a bounded one-step cost comparison.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0014` and the public research-suite addition (first item
rejected by `EXP-0015`):

1. Distance recency or length/distance correlation modeling within the split
   token representation, with complete model costs counted and dual-corpus
   screening.
2. Adaptive Huffman lookup eligibility based on stream length and estimated
   tree-walk work; require identical-stream speed evidence on both suites.
3. Parse decisions based on actual split-entropy costs over a bounded horizon,
   explicitly avoiding the rejected raw 25/33-bit proxy.
4. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.
5. Block-level entropy-model selection that shares parsed tokens and exact-costs
   every representation, prioritizing broad public-corpus gains.

Ranking after `EXP-0015` (first item rejected as inconclusive by `EXP-0016`):

1. Adaptive Huffman lookup eligibility based on stream length and estimated
   tree-walk work; require identical-stream speed evidence on both suites.
2. Parse decisions based on actual split-entropy costs over a bounded horizon,
   explicitly avoiding the rejected raw 25/33-bit proxy.
3. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.
4. Block-level entropy-model selection that shares parsed tokens and exact-costs
   every representation, prioritizing broad public-corpus gains.
5. Joint length/distance classes or conditional models; do not retry plain
   first-order distance deltas without new evidence.

Ranking after `EXP-0016` (first item completed by BENCH-0002 and `EXP-0017`):

1. Add an in-process benchmark/library entry point before more sub-3% speed
   policies; process-per-file timing cannot resolve them reliably.
2. Parse decisions based on actual split-entropy costs over a bounded horizon,
   explicitly avoiding the rejected raw 25/33-bit proxy.
3. Block-level entropy-model selection that shares parsed tokens and exact-costs
   every representation, prioritizing broad public-corpus gains.
4. Joint length/distance classes or conditional models; do not retry plain
   first-order distance deltas without new evidence.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0017` (first item completed by retained `EXP-0018`):

1. Parse decisions based on actual split-entropy costs over a bounded horizon,
   explicitly avoiding the rejected raw 25/33-bit proxy.
2. Block-level entropy-model selection that shares parsed tokens and exact-costs
   every representation, prioritizing broad public-corpus gains.
3. Joint length/distance classes or conditional models; do not retry plain
   first-order distance deltas without new evidence.
4. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.
5. Promote `dwork` to the default decoder only after a periodic process audit;
   keep explicit `d` meanwhile for historical comparability.

Current ranking after `EXP-0018`:

1. Share token demultiplexing, Huffman statistics, and match precomputation
   between parent and optimized candidates to recover EXP-0018 encode speed.
2. Iterate the optimized parse's entropy model once and test whether model/parse
   convergence adds broad ratio without disproportionate extra work.
3. Add alternative match distances or joint length/distance costs to the DP;
   current choices use only the longest match's distance.
4. Block-level entropy-model selection that exact-costs representations while
   reusing parsed streams, prioritizing public-corpus gains.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0019`:

1. Iterate the optimized parse's entropy model once and exact-select it against
   EXP-0018, testing whether model/parse convergence gives broad ratio gains.
2. Share parent/optimized token demultiplexing, Huffman statistics, and match
   precomputation structurally; do not substitute another corpus-sensitive
   raw-block work gate.
3. Add alternative match distances or joint length/distance costs to the DP;
   current choices use only the longest match's distance.
4. Block-level entropy-model selection that reuses parsed streams and exact
   costs every representation.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0020`:

1. Add alternative match distances or joint length/distance costs to the DP;
   current parsing considers lengths only at the single longest-match distance.
2. Share match precomputation and split-field statistics across optimization
   passes; EXP-0020 makes the convergence benefit measurable but compounds the
   dominant encoder work.
3. Block-level entropy-model selection that reuses parsed streams and exact
   costs every representation.
4. Test a principled convergence/stopping signal before any third parse; do not
   perform an iteration-count sweep.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0021`:

1. Share match enumeration/precomputation across EXP-0020 and EXP-0021 passes,
   preserving costed-distance choices while removing the extra full chain scan
   and large per-position offset table.
2. Block-level entropy-model selection that reuses parsed streams and exact
   costs every representation.
3. Test a principled convergence/stopping signal before any further parse; do
   not perform an iteration-count or chain-depth sweep.
4. Explore joint length/distance classes only if they can reuse EXP-0021's
   enumerated matches rather than adding another representation pass.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0022`:

1. Share predecessor-chain construction and longest-match precomputation across
   all three optimized passes. EXP-0022 removes the offset table but each pass
   still rebuilds identical match structure.
2. Block-level entropy-model selection that reuses parsed streams and exact
   costs every representation.
3. Test a principled convergence/stopping signal before any further parse; do
   not perform an iteration-count or chain-depth sweep.
4. Explore joint length/distance classes only if they reuse the existing match
   enumeration rather than adding another representation pass.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

Ranking after `EXP-0023`:

1. Block-level entropy-model selection that reuses parsed streams and exact
   costs every representation, looking for ratio gains beyond parsing alone.
2. Test a principled convergence/stopping signal before any further parse; do
   not perform an iteration-count or chain-depth sweep.
3. Explore joint length/distance classes only if they reuse the shared match
   index rather than adding another representation pass.
4. Share or stream split-field demultiplexing and Huffman statistics between
   parse/model stages; match search is no longer duplicated in the shared mode.
5. Compact persistent match chains using 32-bit/epoch-tagged positions, only
   after profiling the rejected 64-bit design.

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
- `EXP-0014` confirms that local parsing proxies interact with distribution.
  Its stricter token-density rule slightly helped generated XML/JSON and public
  E. coli, but generally hurt public books/text. Parser decisions must account
  for more than immediate raw token count.
- `EXP-0015` shows that representation alternatives must cheaply prequalify or
  share entropy work. Re-encoding all five streams costs 6.5–8.9% even when
  exact selection rejects every block, repeating the compute lesson from
  EXP-0004 and EXP-0011.
- `EXP-0016` suggests eligibility based on encoded-bit work may be directionally
  useful, but the current harness cannot distinguish its 0.7–1.7% aggregate
  gain from startup/timing noise. Do not tune or revive it until an in-process
  decoder benchmark exists.
- `EXP-0017` demonstrates why measurement infrastructure is part of algorithm
  research: the unchanged gate was inconclusive under process timing but showed
  repeatable positive steady-state gains after startup removal. Reversing codec
  order preserved the sign on both suites, avoiding a threshold sweep.
- `EXP-0018` succeeds where the EXP-0014 local proxy failed: costs come from
  real block Huffman trees and decisions compose through a whole-block DP.
  Exact selection protects size, but not the extra all-position match search,
  DP, second token stream, or five-tree entropy encode that drive its 70–74%
  encode regression.
- `EXP-0019` shows that parent representation competitiveness is not a broad
  proxy for avoidable parser work. It cheaply removes DP from raw-selected
  blocks, but observed savings concentrate in three generated fixtures and do
  not reproduce on Calgary+Large. Parser acceleration must share or reduce work
  on compressible blocks, where EXP-0018 spends most of its time.
- `EXP-0020` shows that the cost model and optimized parse have not converged
  after one pass. A second model/parse pass improves roughly 0.5% on both suites
  with broad real-corpus coverage, but costs 66–73% more encode time. The
  diminishing gain relative to EXP-0018's 2.5–3.6% first pass argues for shared
  machinery or an evidence-based stopping rule rather than blind iteration.
- `EXP-0021` establishes a second independent parsing weakness: match length
  and distance must be optimized jointly under field costs. Every public
  research file improves, so this is not a synthetic-data effect. Its third
  parser pass, full comparison of all visited matches, and eight offsets per
  position explain the severe encode/RSS cost; the next work should preserve
  choices while sharing enumeration, not weaken the model with a heuristic.
- `EXP-0022` separates EXP-0021's memory and compute costs. Fusing candidate
  enumeration into the DP removes about 0.9–1.1 MB of measured peak RSS with
  identical streams, but encode time is unchanged. The per-position offset
  table caused memory overhead; repeated full candidate comparisons and three
  independently built match structures dominate runtime.
- `EXP-0023` confirms that match structure is invariant across entropy-model
  iterations. Sharing it recovers 16–25% throughput with identical streams,
  while small RSS increases show that retaining one block index is cheap. The
  remaining maximum-mode cost is DP/model/Huffman work and costed candidate
  comparison, not rebuilding ordinary longest matches.
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
- The delta proxy was highly separable on the development corpus: 52 blocks had
  any concentration improvement, only 34 transformed representations won, and
  every winner exceeded a 4.2x concentration ratio. `EXP-0013`'s sampled 1.2x
  gate avoids 16 discarded parses without changing any selected representation.

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
- `EXP-0019` research screening suggested a 6.6% identical-stream encode gain,
  while five-repeat validation measured a 0.6% regression. Even apparently
  large one-repeat process-timing effects need validation when no size signal
  independently supports the candidate.
- `EXP-0012` parent and candidate compressed streams were byte-identical, yet
  their measured decode rates differed -2.772% over five repeats. This directly
  bounds such process-level decode deltas as noise, not decoder effects.
- EXP-0021 process validation measured dev decode -10.4%, while its screen had
  moved in the opposite direction and public validation was -0.5%. A three-
  repeat in-process control measured only -0.377% (280.09 versus 281.15 MB/s),
  so the process result is rejected as startup/scheduling noise.
- EXP-0023 public process validation reports decode -4.35% even though every
  compressed byte is identical to its parent. It is necessarily measurement
  noise and is not part of the decision.
- Per-file medians are summed, so aggregate time weights startup once per file.
  A later in-process/library benchmark is needed before making startup or
  steady-state speed claims.
- `EXP-0016` is a direct stopping rule for micro-speed work: five repeats on
  identical streams showed broad but sub-3% gains that remain below harness
  resolution. Build the in-process benchmark before another lookup threshold.
- In-process output uses picosecond decimal serialization so sub-microsecond
  tiny-file medians do not round to zero. It remains complementary: use process
  runs for RSS/startup/external comparisons and in-process runs for steady-state
  codec deltas.
- EXP-0018 increases the production binary from 73,496 to 91,000 bytes
  (+23.816%). Treat this as part of the maximum-mode tradeoff; future parser
  work should seek shared machinery or compile-time optionality.
- EXP-0020 increases the production binary from 91,000 to 92,424 bytes
  (+1.565%) and compounds parser time. Its ordinary mode-6 output adds no
  decoder representation, so the code-size increase is encoder/CLI machinery.
- EXP-0021 leaves the measured production binary at 92,424 bytes despite its
  source expansion. The mode still carries substantial runtime and transient
  memory cost, which binary size alone does not capture.
- EXP-0022's additional control mode makes the measured binary 92,456 bytes,
  only 32 bytes larger than EXP-0021. Its meaningful improvement is transient
  encoder memory, not code size.
- EXP-0023 makes the measured binary 92,552 bytes (+96, +0.104%). The speed
  gain comes from shared runtime state rather than code-size expansion.
- The new in-process baseline exposes the prefix lookup's steady-state effect:
  versus `dslow`, decode is 47.7% faster on dev (277.29 vs 187.73 MB/s) and
  68.3% faster on public research (196.73 vs 116.86 MB/s). The earlier process
  measurements substantially understated decoder work by including startup.
- Routine experiments must use staged evaluation: one-repeat new-mode/parent
  screening, then 3–5 repeat validation only for promising candidates. Reuse a
  cached full external audit while tool, platform, corpus hashes, compiler, and
  benchmark method remain unchanged; refresh periodically or after changes.
- Ratio-changing candidates now require screens on both frozen `corpus/dev` and
  pinned `corpus/research`. The research suite is training/validation evidence,
  not an untouched hold-out; it may guide decisions and can itself eventually
  overfit. Periodic audits must therefore use a different locked corpus. Silesia
  is already observed and remains excluded from routine optimization.
- EXP-0020's first result files were generated while the working benchmark
  harness contained unrelated uncommitted aggregation changes. They were
  discarded and regenerated with the exact committed `6b23b64` script over all
  frozen files. Result metadata now names that commit and matches repository
  behavior, preserving reproducibility.
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
- The generated hold-out is contaminated by 12/15 byte-identical fixtures and
  must not be cited as independent generalization evidence. It remains useful
  only for seed/reproducibility stability.
- Silesia audit `results/holdout-silesia-exp0010/` is the first meaningful
  distribution-shift test. It freezes candidate commit `a239a2f`, pins corpus
  commit/archive hash, verifies all round trips, and shows Bite dominated by
  zstd-3, Brotli-6, and xz-1. Silesia is now observed and must not be tuned on
  or described as untouched held-out data in future work.
- The generated corpus is deterministic and its manifest is hashed. Canterbury
  is pinned to archive SHA-256
  `f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb`.
- Held-out generation uses a separate seed and is only invoked explicitly with
  `--held-out`; it has not been generated or inspected during initial tuning.

## Open hypotheses

- Fixed-stride delta structure explains much of the previous xz advantage on
  generated floating-point and RGB data, but its apparent Pareto benefit fails
  to generalize to Silesia. Other genuinely independent raw/scientific evidence
  would be needed before reviving that claim.
- A parser using actual split-stream code costs over a bounded horizon may
  improve on both longest-match lazy parsing and the rejected raw-token proxy.
  EXP-0018 confirms this; open questions are model iteration, alternative
  distances, and how much computation can be shared or avoided. EXP-0019 rules
  out parent-split-versus-raw gating as a broadly useful avoidance mechanism.
- Distance modeling may need joint length/distance classes or recency ranks;
  plain modulo deltas between consecutive distances won no tested block.
  EXP-0021 confirms that length-specific selection among existing distances is
  broadly useful; the open problem is retaining it without exhaustive extra
  search and storage.
- EXP-0022 resolves the storage half of that problem without changing output;
  EXP-0023 then shares the invariant match structure across model iterations,
  recovering broad speed. Costed candidate comparison itself remains open.
- Literal context may still be useful only if its coding tables are shared or
  much cheaper than four independent serialized Huffman trees; do not retry the
  rejected top-two-bit split without a mechanism that addresses table/compute
  cost and the pseudo-text concentration.
- Huffman lookup should be enabled by estimated decoded bit work, not decoded
  symbol count alone. `EXP-0017` resolves the earlier ambiguity with repeated
  startup-free evidence and retains the work gate as optional `dwork`.

## Initial development-corpus external Pareto frontier (2026-08-16)

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
These are valid measurements for the frozen development corpus, not evidence of
general performance. The later Silesia audit shows EXP-0010 dominated by
zstd-3, Brotli-6, and xz-1 and supersedes any broad Pareto interpretation.
On this same dev harness, EXP-0018 adds a maximum-ratio internal point at
2,298,212 bytes and 6.25 MB/s encode: it is 2.50% larger but about 7% faster
than the cached xz-6 measurement, so neither point dominates the other. This
does not override the Silesia conclusion; no held-out data informed EXP-0018.
EXP-0020 reaches 2,287,142 bytes at 3.58 MB/s. Cached xz-6 is both smaller
(2,242,316 bytes) and faster (5.82 MB/s), so EXP-0020 is retained only as Bite's
internal maximum-ratio endpoint and is not an external Pareto point.
EXP-0021 reaches 2,270,290 bytes at 1.18 MB/s. It closes most of Bite's dev size
gap to xz-6 but remains 1.25% larger and about 80% slower, so it is likewise an
internal endpoint rather than an external Pareto point.
EXP-0022 preserves EXP-0021's size and effective speed while reducing encoder
RSS from 7,553,024 to 6,651,904 bytes on dev. It supersedes EXP-0021 on Bite's
internal size/speed/memory frontier, but remains externally dominated by xz-6.
EXP-0023 preserves the same 2,270,290-byte output while raising dev encode
throughput from 1.36 to 1.58 MB/s at nearly equal RSS. It supersedes EXP-0022
on Bite's internal size/speed frontier, but xz-6 remains smaller and faster.
