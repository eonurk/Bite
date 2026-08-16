#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
archive="$root/corpus/downloads/cantrbry.tar.gz"
destination="$root/corpus/dev/canterbury"
expected=f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb

mkdir -p "$(dirname "$archive")" "$destination"
if [ ! -f "$archive" ]; then
    curl -L --fail --silent --show-error \
        https://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz -o "$archive"
fi
actual=$(shasum -a 256 "$archive" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
    echo "Canterbury archive hash mismatch: $actual" >&2
    exit 1
fi
tar -xzf "$archive" -C "$destination"
echo "Canterbury corpus ready ($actual)"

