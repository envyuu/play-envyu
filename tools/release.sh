#!/bin/bash
# Build, write manifests/latest.json, then prove the patcher reproduces the build byte for byte.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: tools/release.sh VERSION}
python3 -I tools/hex2bin.py stock/PolyendPlay_v1.6.0.ppf stock/firmware.bin > /dev/null
(cd fw && ./build.sh > /dev/null)
python3 -I tools/make_manifest.py stock/PolyendPlay_v1.6.0.ppf fw/PolyendPlay_v1.6.0_mod.ppf fw/fx.bin manifests/latest.json --version "$V"
cp manifests/latest.json docs/manifest.json
T=$(mktemp -d)
python3 -I patcher/play_patch.py stock/PolyendPlay_v1.6.0.ppf -o "$T/out.ppf"
cmp "$T/out.ppf" fw/PolyendPlay_v1.6.0_mod.ppf && echo "patcher output == build"
rm -r "$T"
