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

