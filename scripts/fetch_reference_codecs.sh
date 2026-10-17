#!/bin/sh
# Build the size-oriented reference codecs used by EXP-0080 from pinned upstream
# tags into build/refs/bin. Each checkout is verified by commit and by the SHA-256
# of `git archive --format=tar HEAD`.
#
# bzip2, 7-Zip (PPMd) and zpaq come from the distribution's package manager:
#   apt-get install bzip2 7zip zpaq   (Ubuntu 24.04: 1.0.8-5.1ubuntu0.1, 23.01+dfsg-11, 7.15+repack-1)
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/refs/src"
bin="$root/build/refs/bin"
mkdir -p "$src" "$bin"

fetch() {  # name url tag commit archive_sha256
    name=$1 url=$2 tag=$3 commit=$4 expected=$5
    if [ ! -d "$src/$name/.git" ]; then
        git clone --quiet --depth 1 --branch "$tag" "$url" "$src/$name"
    fi
    actual_commit=$(git -C "$src/$name" rev-parse HEAD)
    actual=$(git -C "$src/$name" archive --format=tar HEAD | shasum -a 256 | awk '{print $1}')
    if [ "$actual_commit" != "$commit" ] || [ "$actual" != "$expected" ]; then
        echo "$name $tag mismatch: commit $actual_commit, archive sha256 $actual" >&2
        exit 1
    fi
}

fetch libbsc https://github.com/IlyaGrebnov/libbsc v3.3.12 \
    5e5c2ef0fb1298626936b091f6e4ae539e5b0071 22f450eda5167d2010759c52d5e149ef74f90c9aad36736a415fa9bc4a2f671e
cmake -S "$src/libbsc" -B "$root/build/refs/bsc-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$root/build/refs/bsc-build" -j >/dev/null
cp "$root/build/refs/bsc-build/bsc" "$bin/bsc"

fetch cmix https://github.com/byronknoll/cmix v21 \
    c443679c0773b8ae5b05423827804063d82ae7a8 a3b1c4fee9e2af3fbf989ddcaf788291a7ea578d707aa9c0608d5b6b5d21ffe1
# Upstream's `make all` flags; -march=native output decodes only on the same CPU.
make -C "$src/cmix" cmix CC="${CXX:-clang++}" LFLAGS="-std=c++14 -Wall -Ofast -march=native" >/dev/null
cp "$src/cmix/cmix" "$bin/cmix"
echo "cmix v21 (c443679), no dictionary" > "$root/build/refs/bin/cmix.version"

echo "reference codecs ready in $bin"
