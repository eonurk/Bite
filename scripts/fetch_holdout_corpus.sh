#!/bin/sh
# EXP-0081: fetch and lock the pre-registered holdout-2026 corpus.
#
# Every download is pinned by SHA-256 and every derived file is covered by the
# pinned lock digest below. Files are fetched for local evaluation only and are
# never committed or redistributed. See docs/experiments/EXP-0081.md.
#
# Usage: scripts/fetch_holdout_corpus.sh [--dry-run]
#   --dry-run  download (or reuse cached downloads), verify every pinned hash,
#              build the corpus in a scratch directory, verify the lock digest,
#              then discard it without touching corpus/holdout-2026.
#
# Requires curl, shasum, unzip (Info-ZIP), xz, gzip, tar and python3.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
downloads="$root/corpus/downloads/holdout-2026"
destination="$root/corpus/holdout-2026"
staging="$root/corpus/.holdout-2026.partial"
records="$downloads/records.tsv"

# sha256 over "<sha256>  <bytes>  <path>\n" for every corpus file, sorted by path.
expected_lock=6a6061d06fd8037f81a13309b4783f61018bd335751e2909cb14d45082297c2a
expected_files=37
expected_bytes=144681868

dry_run=0
case "${1:-}" in
    "") ;;
    --dry-run) dry_run=1 ;;
    *) echo "usage: $0 [--dry-run]" >&2; exit 2 ;;
esac

raw=https://raw.githubusercontent.com
gh=https://github.com
nltk=$raw/nltk/nltk_data/550b6625bcef1f2abff2ff770a5a0d272c9c6b2a/packages/corpora
simdjson=$raw/simdjson/simdjson/edb7016c62efbe6155226896cdba5736307190e3/jsonexamples
csse=$raw/CSSEGISandData/COVID-19/4360e50239b4eb6b22f3a1759323748f36752177/csse_covid_19_data/csse_covid_19_time_series
elastic="$raw/elastic/examples/6d86454ebb7a850bcd7e80abe86fe683370018a6/Common%20Data%20Formats"
loghub=$raw/logpai/loghub/dd61d0952749ee7963bde24220d1be5ede023033
xarray=$raw/pydata/xarray-data/a35297e9da2cc99c811014f0c8a4297345a5c28d
astropy=$raw/astropy/astropy-data/1b964d39b378390b24c58fb5633cef238d22a35a
opencv=$raw/opencv/opencv/a9ea97659a120ac23d2219dfc819deb9f4082416/samples/data
pypi=https://files.pythonhosted.org/packages

# download-name sha256 url
items=$(cat <<EOF
gutenberg.zip 2d3c3ab548c653944310f37f536443ec85d0a0ad855fcae217a0c9efdce2d611 $nltk/gutenberg.zip
europarl_raw.zip ad553e177baac263840c10980e6f3e76d5d15f7f7a078bd98520b36edb69b27c $nltk/europarl_raw.zip
udhr2.zip 0796c314b09a930c989c6f9d93d226af9af13feccd88496e196c743dd266c7f3 $nltk/udhr2.zip
curl-8.10.1.tar.xz 73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee $gh/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
pytest-8.3.3.tar.gz 70b98107bd648308a7952b06e6ca9a50bc660be218d53c257cc1fc94fda10181 $pypi/8b/6c/62bbd536103af674e227c41a8f3dcd022d591f6eed5facb5a0f31ee33bbc/pytest-8.3.3.tar.gz
twitter.json 30721e496a8d73cfc50658923c34eb2c0fbe15ee6835005e43ee624d8dedf200 $simdjson/twitter.json
citm_catalog.json a73e7a883f6ea8de113dff59702975e60119b4b58d451d518a929f31c92e2059 $simdjson/citm_catalog.json
time_series_covid19_confirmed_US.csv e8fe5837b1a7ce09e6187ef778bf69fdcfa9fcd7015b1ba597f248b0736d8cf3 $csse/time_series_covid19_confirmed_US.csv
apache_logs f15c31e905f86c7b4b6ab44aee74d0a2086dce89f010187d983edea7ef0364ef $elastic/apache_logs/apache_logs
nginx_json_logs 05131a0ec5c40884fbf4a066f8a9244b6475ae6a5844b1f10fb38d42d8ff6967 $elastic/nginx_json_logs/nginx_json_logs
Android_2k.log 47641549915e662ff590291df266a45f635eedca7c5f1b41a4fa853fe5d2f409 $loghub/Android/Android_2k.log
HDFS_2k.log 7c967000980c086ed55fa6544ba4f05fe66d44622795e890c68caf8bbb635035 $loghub/HDFS/HDFS_2k.log
OpenSSH_2k.log 1e4912727fa88245113d41b16a0cd25ceadba7f931e1c406542885b91254264f $loghub/OpenSSH/OpenSSH_2k.log
Windows_2k.log 372fb809464a6d6016e599e9272d7cf1e8b644f25c90c7f76f19c936362456d0 $loghub/Windows/Windows_2k.log
ripgrep-14.1.1-x86_64-unknown-linux-musl.tar.gz 4cf9f2741e6c465ffdb7c26f38056a59e2a2544b51f7cc128ef28337eeae4d8e $gh/BurntSushi/ripgrep/releases/download/14.1.1/ripgrep-14.1.1-x86_64-unknown-linux-musl.tar.gz
ripgrep-14.1.1-aarch64-unknown-linux-gnu.tar.gz c827481c4ff4ea10c9dc7a4022c8de5db34a5737cb74484d62eb94a95841ab2f $gh/BurntSushi/ripgrep/releases/download/14.1.1/ripgrep-14.1.1-aarch64-unknown-linux-gnu.tar.gz
jq-linux-amd64 5942c9b0934e510ee61eb3e30273f1b3fe2590df93933a93d7c58b81d19c8ff5 $gh/jqlang/jq/releases/download/jq-1.7.1/jq-linux-amd64
jq-windows-amd64.exe 7451fbbf37feffb9bf262bd97c54f0da558c63f0748e64152dd87b0a07b6d6ab $gh/jqlang/jq/releases/download/jq-1.7.1/jq-windows-amd64.exe
jq-macos-arm64 0bbe619e663e0de2c550be2fe0d240d076799d6f8a652b70fa04aea8a8362e8a $gh/jqlang/jq/releases/download/jq-1.7.1/jq-macos-arm64
fzf-0.55.0-linux_amd64.tar.gz 4df2393776942780ddab2cea713ddaac06cd5c3886cd23bc9119a6d3aa1e02bd $gh/junegunn/fzf/releases/download/v0.55.0/fzf-0.55.0-linux_amd64.tar.gz
air_temperature.nc c606b89c35970a2983b914b76df4adbb409003ef34aa7cfd7f582e41f307482b $xarray/air_temperature.nc
eraint_uvz.nc 8f4cd0f75418a8e8f8f957c3b007f49e68a58d1bdd0e028adeeb057889f635d2 $xarray/eraint_uvz.nc
imerghh_730.hdf5 b36764f671203c2cbf9334b6ec35f6be6078ee8faf64edf3c63f4ccfffbaa42e $xarray/imerghh_730.hdf5
HorseHead.fits c4bcc0bccbadf1488e4e361c0d8f04255dc344ff401fd4827139b3354eafe863 $astropy/tutorials/FITS-images/HorseHead.fits
spitzer_example_image.fits 4b9cf15419de311fc683a9c378327a0b28bfee29d01e4fdbb9dc5780417f7ebc $astropy/photometry/spitzer_example_image.fits
l1448_13co.fits de4d44cd2841d34cfe98915ab322f83e302f13b9599ef8629079249e53ad6d49 $astropy/l1448/l1448_13co.fits
fruits.jpg 9c031d80a1c52da5eca790db896baffec6a7e52bf786cdb7bbfca5c7f880e6a1 $opencv/fruits.jpg
building.jpg 742a1baad62ac82e91e718e77eedf7e85c2eddc4badfb8c87c6cbc86c45a8b07 $opencv/building.jpg
HappyFish.jpg ade5b67347d7f89b1539c93843f3f7178782a7e34eb1fe586f4a785c3e60a432 $opencv/HappyFish.jpg
home.jpg 23b8cf46a1965d0ec33459b875aed43187802834db49e0daa9fa2cc842e9d8d2 $opencv/home.jpg
graf1.png 1504b769303c7bde00fa578eeaad3c68e02aceabeb1242e556f1f8d19e4bdea5 $opencv/graf1.png
basketball1.png ba06f6701f7260998b430c39b6557f775497e6ce7b1a74f0b7ea6af371bf54a6 $opencv/basketball1.png
box_in_scene.png 8b0225ff76244a42bd1400c0904f8b7afea7d97b7d8115495e42e98ad347bd51 $opencv/box_in_scene.png
guava-33.3.1-jre.jar 4bf0e2c5af8e4525c96e8fde17a4f7307f97f8478f11c4c8e35a0e3298ae4e90 https://repo1.maven.org/maven2/com/google/guava/guava/33.3.1-jre/guava-33.3.1-jre.jar
typescript-5.6.3.tgz ef67f8d8ad895858024b7339d3e34bf112cae3c5db1f538c3079038b17ae30fa https://registry.npmjs.org/typescript/-/typescript-5.6.3.tgz
jq-1.7.1.tar.gz 478c9ca129fd2e3443fe27314b455e211e0d8c60bc8ff7df703873deeee580c2 $gh/jqlang/jq/releases/download/jq-1.7.1/jq-1.7.1.tar.gz
EOF
)

sha256() {
    shasum -a 256 "$1" | awk '{print $1}'
}

url_of() {
    printf '%s\n' "$items" | awk -v name="$1" '$1 == name {print $3}'
}

mkdir -p "$downloads"
printf '%s\n' "$items" | while read -r name expected url; do
    file="$downloads/$name"
    if [ ! -f "$file" ]; then
        curl -L --fail --silent --show-error --retry 6 "$url" -o "$file.part" || {
            echo "download failed: $name from $url" >&2
            exit 1
        }
        mv "$file.part" "$file"
    fi
    actual=$(sha256 "$file")
    if [ "$actual" != "$expected" ]; then
        echo "$name hash mismatch: $actual (expected $expected)" >&2
        exit 1
    fi
    echo "verified $expected  $name"
done
download_count=$(printf '%s\n' "$items" | wc -l | tr -d ' ')
echo "all $download_count pinned download hashes verified"

rm -rf "$staging"
trap 'rm -rf "$staging"; rm -f "$records"' EXIT
mkdir -p "$staging/text" "$staging/source" "$staging/structured" \
    "$staging/executables" "$staging/scientific" "$staging/compressed"
: > "$records"

# record PATH DOWNLOAD-NAME DERIVATION
record() {
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$(url_of "$2")" "$3" >> "$records"
}

copy() {
    cp "$downloads/$2" "$staging/$1"
    record "$1" "$2" "verbatim copy"
}

# Members are concatenated in the order given (or C-locale sorted order).
concat_zip() {
    out=$1 archive=$2 description=$3
    shift 3
    : > "$staging/$out"
    for member in "$@"; do
        unzip -p "$downloads/$archive" "$member" >> "$staging/$out"
    done
    record "$out" "$archive" "$description"
}

sorted_members() {
    unzip -Z1 "$downloads/$1" | grep -E "$2" | LC_ALL=C sort
}

books="austen-emma austen-persuasion austen-sense blake-poems bryant-stories
burgess-busterbrown chesterton-ball chesterton-brown chesterton-thursday
edgeworth-parents melville-moby_dick whitman-leaves"
set --
for book in $books; do
    set -- "$@" "gutenberg/$book.txt"
done
concat_zip text/gutenberg-12-books.txt gutenberg.zip \
    "concatenation of gutenberg/{$(echo $books | tr ' ' ',')}.txt in that order" "$@"

# shellcheck disable=SC2046
concat_zip text/europarl-en.txt europarl_raw.zip \
    "concatenation of every europarl_raw/english/* member, C-locale sorted" \
    $(sorted_members europarl_raw.zip '^europarl_raw/english/.')
# shellcheck disable=SC2046
concat_zip text/europarl-el.txt europarl_raw.zip \
    "concatenation of every europarl_raw/greek/* member, C-locale sorted" \
    $(sorted_members europarl_raw.zip '^europarl_raw/greek/.')
# shellcheck disable=SC2046
concat_zip text/udhr2-all-languages.txt udhr2.zip \
    "concatenation of every udhr2/*.txt member, C-locale sorted" \
    $(sorted_members udhr2.zip '^udhr2/.+\.txt$')

xz -dc "$downloads/curl-8.10.1.tar.xz" > "$staging/source/curl-8.10.1.tar"
record source/curl-8.10.1.tar curl-8.10.1.tar.xz "xz-decompressed release tarball"
gzip -dc "$downloads/pytest-8.3.3.tar.gz" > "$staging/source/pytest-8.3.3.tar"
record source/pytest-8.3.3.tar pytest-8.3.3.tar.gz "gzip-decompressed sdist tarball"

copy structured/twitter.json twitter.json
copy structured/citm_catalog.json citm_catalog.json
copy structured/covid19_confirmed_US.csv time_series_covid19_confirmed_US.csv
copy structured/apache_access.log apache_logs
copy structured/nginx_json.log nginx_json_logs
for log in Android HDFS OpenSSH Windows; do
    copy "structured/loghub-${log}_2k.log" "${log}_2k.log"
done

tar -xzOf "$downloads/ripgrep-14.1.1-x86_64-unknown-linux-musl.tar.gz" \
    ripgrep-14.1.1-x86_64-unknown-linux-musl/rg > "$staging/executables/rg-14.1.1-x86_64-linux-musl"
record executables/rg-14.1.1-x86_64-linux-musl ripgrep-14.1.1-x86_64-unknown-linux-musl.tar.gz \
    "tar member ripgrep-14.1.1-x86_64-unknown-linux-musl/rg"
tar -xzOf "$downloads/ripgrep-14.1.1-aarch64-unknown-linux-gnu.tar.gz" \
    ripgrep-14.1.1-aarch64-unknown-linux-gnu/rg > "$staging/executables/rg-14.1.1-aarch64-linux-gnu"
record executables/rg-14.1.1-aarch64-linux-gnu ripgrep-14.1.1-aarch64-unknown-linux-gnu.tar.gz \
    "tar member ripgrep-14.1.1-aarch64-unknown-linux-gnu/rg"
copy executables/jq-1.7.1-linux-amd64 jq-linux-amd64
copy executables/jq-1.7.1-windows-amd64.exe jq-windows-amd64.exe
copy executables/jq-1.7.1-macos-arm64 jq-macos-arm64
tar -xzOf "$downloads/fzf-0.55.0-linux_amd64.tar.gz" fzf > "$staging/executables/fzf-0.55.0-linux-amd64"
record executables/fzf-0.55.0-linux-amd64 fzf-0.55.0-linux_amd64.tar.gz "tar member fzf"

for name in air_temperature.nc eraint_uvz.nc imerghh_730.hdf5 HorseHead.fits \
    spitzer_example_image.fits l1448_13co.fits; do
    copy "scientific/$name" "$name"
done

for name in fruits.jpg building.jpg HappyFish.jpg home.jpg graf1.png \
    basketball1.png box_in_scene.png guava-33.3.1-jre.jar typescript-5.6.3.tgz \
    jq-1.7.1.tar.gz; do
    copy "compressed/$name" "$name"
done

download_hashes=$(printf '%s\n' "$items" | awk '{print $1 "\t" $2 "\t" $3}')
summary=$(DOWNLOADS="$download_hashes" python3 -I - "$staging" "$records" <<'PY'
import hashlib
import json
import os
import sys
from pathlib import Path

staging = Path(sys.argv[1])
rows = [line.split("\t") for line in Path(sys.argv[2]).read_text().splitlines()]
recorded = {row[0] for row in rows}
present = {p.relative_to(staging).as_posix() for p in staging.rglob("*") if p.is_file()}
if recorded != present or len(recorded) != len(rows):
    sys.exit(f"manifest records do not match staged files: {sorted(recorded ^ present)}")

files = []
for path, download, url, derivation in sorted(rows):
    data = (staging / path).read_bytes()
    files.append({
        "path": path,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "category": path.split("/", 1)[0],
        "source_url": url,
        "download": download,
        "derivation": derivation,
    })
lock = hashlib.sha256("".join(
    f"{f['sha256']}  {f['bytes']}  {f['path']}\n" for f in files).encode()).hexdigest()
downloads = [dict(zip(("name", "sha256", "url"), line.split("\t")))
             for line in os.environ["DOWNLOADS"].splitlines()]
manifest = {
    "corpus": "holdout-2026",
    "experiment": "EXP-0081",
    "lock_sha256": lock,
    "file_count": len(files),
    "total_bytes": sum(f["bytes"] for f in files),
    "files": files,
    "downloads": downloads,
}
(staging / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(lock, len(files), manifest["total_bytes"])
PY
)
rm -f "$records"
set -- $summary
actual_lock=$1 actual_files=$2 actual_bytes=$3
if [ "$actual_lock" != "$expected_lock" ] || [ "$actual_files" != "$expected_files" ] \
    || [ "$actual_bytes" != "$expected_bytes" ]; then
    echo "holdout lock mismatch: $actual_lock ($actual_files files, $actual_bytes bytes)" >&2
    echo "expected $expected_lock ($expected_files files, $expected_bytes bytes)" >&2
    exit 1
fi

if [ "$dry_run" = 1 ]; then
    echo "dry run ok: lock $actual_lock ($actual_files files, $actual_bytes bytes); corpus/holdout-2026 untouched"
    exit 0
fi
rm -rf "$destination"
mv "$staging" "$destination"
echo "holdout-2026 corpus ready: lock $actual_lock ($actual_files files, $actual_bytes bytes)"
