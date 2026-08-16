#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
downloads="$root/corpus/downloads"
calgary_archive="$downloads/calgary.tar.gz"
large_archive="$downloads/large.tar.gz"
calgary_destination="$root/corpus/research/calgary"
large_destination="$root/corpus/research/large"
calgary_expected=e109eebdc19c5cee533c58bd6a49a4be3a77cc52f84ba234a089148a4f2093b7
large_expected=7df00ff4cb8c9ce4187d9fa4100aa88208a72d9a227e3cd8bab23b71971eaf41

mkdir -p "$downloads" "$calgary_destination" "$large_destination"
if [ ! -f "$calgary_archive" ]; then
    curl -L --fail --silent --show-error \
        https://corpus.canterbury.ac.nz/resources/calgary.tar.gz -o "$calgary_archive"
fi
if [ ! -f "$large_archive" ]; then
    curl -L --fail --silent --show-error \
        https://corpus.canterbury.ac.nz/resources/large.tar.gz -o "$large_archive"
fi

calgary_actual=$(shasum -a 256 "$calgary_archive" | awk '{print $1}')
large_actual=$(shasum -a 256 "$large_archive" | awk '{print $1}')
if [ "$calgary_actual" != "$calgary_expected" ]; then
    echo "Calgary archive hash mismatch: $calgary_actual" >&2
    exit 1
fi
if [ "$large_actual" != "$large_expected" ]; then
    echo "Large corpus archive hash mismatch: $large_actual" >&2
    exit 1
fi

# Calgary pic is byte-identical to development/canterbury/ptt5, and paper3-6
# are not part of the current 14-file Calgary definition. Exclude all five.
tar -xzf "$calgary_archive" -C "$calgary_destination" \
    bib book1 book2 geo news obj1 obj2 paper1 paper2 progc progl progp trans
tar -xzf "$large_archive" -C "$large_destination" E.coli bible.txt world192.txt

calgary_count=$(find "$calgary_destination" -type f | wc -l | tr -d ' ')
large_count=$(find "$large_destination" -type f | wc -l | tr -d ' ')
if [ "$calgary_count" != 13 ] || [ "$large_count" != 3 ]; then
    echo "research corpus contains unexpected files; expected 13 Calgary + 3 Large" >&2
    exit 1
fi
echo "research corpora ready (16 non-duplicate files; pinned archive hashes verified)"
