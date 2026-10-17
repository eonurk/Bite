# Bite

Bite is a small lossless compressor written in C for controlled compression
experiments, not production use. Each idea is tested as a numbered experiment
against its parent, and only exact, measured improvements are kept.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/progress-dark.png">
  <img alt="Research-corpus bits per byte by experiment: kept improvements step down from 2.50 to 1.94, past zstd -19 and xz -9e; the unvalidated EXP-0075 BWT candidate reaches 1.88, below Brotli 11" src="docs/progress-light.png">
</picture>

## Results

Measured on the fixed public research corpus (16 Calgary and Canterbury Large
Corpus files, 13,787,888 bytes):

| Codec | Bytes | Bits/byte | Encode | Decode |
|---|---:|---:|---:|---:|
| Bite `cmax` (EXP-0075, not yet validated) | 3,247,227 | 1.884 | 239.4 s* | 0.86 s* |
| Brotli 11 | 3,283,714 | 1.905 | 23.2 s | 0.14 s |
| Bite EXP-0074 (last validated size maximum) | 3,347,119 | 1.942 | 265.8 s* | 0.75 s* |
| Bite `cfast` (EXP-0072) | 3,359,272 | 1.949 | 37.6 s | 0.59 s |
| xz -9e | 3,360,808 | 1.950 | 5.6 s | 0.23 s |
| zstd -19 | 3,384,473 | 1.964 | 4.2 s | 0.11 s |

\* Timed by the in-process runner; other rows use the process harness.

Bite wins on size and pays for it in speed. `cmax` encodes about 10 times
slower than Brotli 11 and 57 times slower than zstd -19, and Bite decodes 3–8
times slower than the reference codecs. These results are specific to this
corpus and are not a claim that Bite is generally better than xz, zstd or
Brotli.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/pareto-dark.png">
  <img alt="Bits per byte against encode and decode speed: the reference codecs form the Pareto frontier; Bite reaches the smallest sizes but encodes and decodes more slowly" src="docs/pareto-light.png">
</picture>

## Quick start

```sh
make
make corpus
make test
```

```sh
bin/bite cmax INPUT OUTPUT.bite   # smallest output, slowest
bin/bite cfast INPUT OUTPUT.bite  # about 3.5% larger, much faster
bin/bite d OUTPUT.bite RESTORED   # decodes every mode
```

## How it works

- **LZ parsing.** A 16 MiB match window, a 2,048-deep hash-chain search and a
  dynamic-programming parser that prices matches with the entropy coder's
  actual costs.
- **Entropy coding.** Separate streams for literals, lengths, distances and
  control bits, each coded with Huffman or adaptive arithmetic coding and
  context models: previous bytes, match state, recent distances and a mix of
  order-1 to order-3 literal contexts.
- **Other representations.** A delta transform for structured data, an order-4
  model for small alphabets such as DNA, and a Burrows–Wheeler transform for
  text.
- **Exact selection.** Each block is encoded every way and the smallest
  complete payload wins, with raw storage as a fallback. Adding a mode can
  therefore never make a block larger.

## Research method

Every experiment starts with a written hypothesis, is screened on samples or
the frozen development corpus, is measured on the public research corpus, and
is recorded as kept or rejected along with its results. The Silesia corpus is
reserved for hold-out checks and not used for tuning.

- [experiments.csv](experiments.csv): one row per experiment, with its decision
- [research_memory.md](research_memory.md): what each experiment found
- [RESEARCH_PLAN.md](RESEARCH_PLAN.md): the current experiment and next steps
- [docs/benchmarking.md](docs/benchmarking.md): corpora and benchmark harnesses
- [docs/format.md](docs/format.md): the stream format and every mode
- `results/`: raw measurements for every run

Regenerate the charts with `python3 scripts/plot_progress.py` and
`python3 scripts/plot_pareto.py`.
