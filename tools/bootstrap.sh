#!/bin/sh
# Fetch pinned dependency tags without requiring Git repository histories.
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fetch() {
    name=$1 repository=$2 tag=$3
    if [ -d "repos/$name" ]; then
        echo "Keeping existing repos/$name"
        return
    fi
    curl --fail --location --retry 3 "https://codeload.github.com/$repository/tar.gz/refs/tags/$tag" -o "$tmp/source.tar.gz"
    mkdir "$tmp/unpack"
    tar xzf "$tmp/source.tar.gz" --strip-components=1 -C "$tmp/unpack"
    mkdir -p repos
    mv "$tmp/unpack" "repos/$name"
}
fetch apache-mynewt-core apache/mynewt-core mynewt_1_15_0_tag
fetch apache-mynewt-nimble apache/mynewt-nimble nimble_1_10_0_tag
fetch nordic-nrfx NordicSemiconductor/nrfx v3.14.0
fetch arm-CMSIS_5 ARM-software/CMSIS_5 5.9.0
fetch mbedtls Mbed-TLS/mbedtls v3.6.6

python3 tools/patch_mynewt.py
