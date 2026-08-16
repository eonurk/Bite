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
