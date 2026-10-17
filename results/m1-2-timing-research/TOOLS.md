# m1-2 timing run: environment

- Machine: M1 Mac (4 performance + 4 efficiency cores, 16 GB), macOS 27.0, idle
  during the run apart from the benchmark.
- Bite: commit `1b288da`, built with the repository Makefile (Apple clang 21).
- Harness: `scripts/benchmark.py --stage validate --repeats 3 --corpus-dir
  corpus/research --aggregate-exclude` under Python 3.13.16; timings are
  medians of process wall time.
- Reference codecs, built from upstream source at the versions used for the
  earlier laptop references:

| Codec | Source | SHA-256 | Build |
|---|---|---|---|
| zstd 1.5.7 | github.com/facebook/zstd release `zstd-1.5.7.tar.gz` | `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3` | `make -C programs zstd HAVE_LZMA=0 HAVE_ZLIB=0 HAVE_LZ4=0` (default -O3) |
| Brotli 1.2.0 | github.com/google/brotli tag `v1.2.0` archive | `816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec` | `cc -O3 -DNDEBUG` over `c/common`, `c/dec`, `c/enc`, `c/tools/brotli.c` |
| xz 5.8.2 | github.com/tukaani-project/xz release `xz-5.8.2.tar.gz` | `ce09c50a5962786b83e5da389c90dd2c15ecd0980a258dd01f70f9e7ce58a8f1` | `./configure --disable-shared` (default -O2) |
| gzip | Apple gzip 487.0.1 (system) | | |

The zstd format integrations disabled above (xz/gzip/lz4 container support)
do not affect zstd's own compression.
