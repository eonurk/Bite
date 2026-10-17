#!/bin/sh
# Single-file adapter for archivers (7-Zip, zpaq) that cannot write a bare
# compressed stream. The input is stored under the fixed name "f", so archive
# overhead does not depend on corpus paths.
#
# usage: archive_codec.sh <7z-ppmd|zpaq> <c|d> INPUT OUTPUT [archiver options...]
set -eu

tool=$1 mode=$2 input=$3 output=$4
shift 4
input=$(CDPATH= cd -- "$(dirname -- "$input")" && pwd)/$(basename -- "$input")
output=$(CDPATH= cd -- "$(dirname -- "$output")" && pwd)/$(basename -- "$output")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
cd "$work"

case "$tool:$mode" in
    7z-ppmd:c)
        cp "$input" f
        7z a -t7z -bd -bso0 -bsp0 "$@" a.7z f
        mv a.7z "$output" ;;
    7z-ppmd:d)
        cp "$input" a.7z
        7z e -bd -bso0 -bsp0 a.7z
        mv f "$output" ;;
    zpaq:c)
        cp "$input" f
        zpaq a a.zpaq f "$@" >/dev/null
        mv a.zpaq "$output" ;;
    zpaq:d)
        cp "$input" a.zpaq
        zpaq x a.zpaq >/dev/null
        mv f "$output" ;;
    *)
        echo "usage: $0 <7z-ppmd|zpaq> <c|d> INPUT OUTPUT [options...]" >&2
        exit 2 ;;
esac
