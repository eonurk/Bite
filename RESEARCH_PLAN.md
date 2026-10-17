# Bite research direction

The objective is a competitive lossless compressor, with minimum complete
stream size as the first target. Encoding time, decoding time, peak memory,
and model/decoder costs remain separate reported dimensions. A corpus-size
win is evidence for that workload, not a universal ranking.

## Current experiment: EXP-0075

Hypothesis: block sorting captures long text contexts that the current LZ
parser leaves expensive. Test a self-contained cyclic BWT + move-to-front +
zero-run + adaptive arithmetic candidate, then select by full payload size.
Keep EXP-0074 available and preserve all historical mode names.

The candidate is frozen before combined-preset validation. There is no
filename, extension, or corpus-position dispatch. The new `cbwt` preset
isolates its costs; `cmax` includes the previous best representation at each
block. Raw fallback remains available.

Acceptance checks:

- Exact round trips on every development and research file, including files
  excluded from aggregates; no per-file size regression against EXP-0074.
- Forced new-codec tests, including periodic inputs, all byte values, random
  inputs, run-length boundaries, and multi-block framing.
- ASan/UBSan validation and bounded malformed-payload tests.
- Fresh Brotli 11, bzip2-9, xz-9e, and zstd-19 reference size audit on identical
  file hashes. Never compare wall-clock and CPU timings as if interchangeable.

## Next evidence needed

1. Add size-oriented references beyond practical codecs. Candidates include
   [cmix](https://www.byronknoll.com/cmix.html) and
   [ZPAQ](https://www.mattmahoney.net/zpaq/); record their resource requirements
   and complete decoder/model costs where applicable. Their own benchmark
   numbers do not substitute for measurements on Bite's corpus.
2. Pre-register an independent corpus with real text, source, structured data,
   executables, scientific arrays, and already-compressed inputs. Freeze file
   hashes and settings before evaluating. The current generated hold-out has
   duplicate fixtures, and Silesia has already been observed; neither should
   be relabeled as untouched evidence.
3. Report per-file wins and losses, aggregate size, and encode/decode/memory
   tradeoffs. Keep any further tuning separate from the locked evaluation.
4. Profile candidate selection. A cheaper dispatcher is a separate speed
   preset unless it can preserve the exhaustive minimum-size decision.
5. Explore a stronger BWT rank model or a full-stream context model as a new
   experiment with its own format identity and parent comparison. Do not
   silently alter an existing decoder's probability model.

Broader references and independent evaluation are required before claiming
leadership. EXP-0075 is one concrete step toward that objective.
