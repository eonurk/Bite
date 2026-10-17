# EXP-0030 compact canonical Huffman models

## Hypothesis

The five split Huffman streams repeatedly pay to serialize explicit tree
topology and leaf symbols. Reusing the same code lengths with canonical codes,
and choosing a packed 256-length table or sparse symbol/length list per stream,
should reduce model overhead broadly without changing parsing or encoded bit
counts.

## Change

- Add block mode 8 and encoder `clblinksopt2dfsbc`.
- Prefix each of the five mode-8 substreams with a selector and exact-select the
  existing tree format or the compact canonical format independently.
- Pack common code lengths into 4-bit entries with escapes for lengths 15+, or
  use a sparse symbol/length list when smaller.
- Exact-select the complete mode-8 block against all existing raw, LZ, Huffman,
  split, optimized-parse, and delta candidates.
- Retain strict canonical-model validation and the existing fast-decode lookup.

The preceding literal-residual prototype (`EXP-0029`) was removed after saving
only 28 bytes on dev and zero bytes on the public research suite.

## Validation

All 36 normal and boundary fixtures round-trip exactly in all 22 retained modes.
ASan+UBSan round trips passed all 42 dev+research files.

Three-repeat startup-free results against `clblinksopt2dfsb`:

| Suite | Parent bytes | Candidate bytes | Size change | Encode change | Decode change |
|---|---:|---:|---:|---:|---:|
| Dev | 2,270,290 | 2,252,702 | -0.775% | -8.14% | +0.13% |
| Calgary+Large | 4,101,724 | 3,999,152 | -2.501% | +2.01% | -0.65% |

The candidate improves 16/26 dev files with no regression and all 16 public
research files. A three-repeat process validation on dev measures encode
throughput -3.04%, decode +0.10% noise, and encoder RSS +11.75%. The production
binary is 111,832 bytes, 20.83% larger than EXP-0028's 92,552-byte binary.

Focused external checks:

- Dev xz-6 is 10,386 bytes (0.461%) smaller and 3.25x faster to encode, while
  Bite is 1.63x faster to decode and uses much less encoder memory. Neither
  dominates the other on the size/decode tradeoff.
- On public research, Bite is 4.35% smaller than zstd-3 but much slower. Brotli-6
  and xz-6 remain smaller and faster, so Bite is not a general external winner.

## Decision

Retain `clblinksopt2dfsbc` as Bite's new maximum-ratio mode. The improvement is
broad, deterministic, and strongest on independent public data. It is not yet a
general-purpose breakthrough over established codecs; canonical decoder/code
size and the remaining entropy-model gap are the next targets.

