#!/bin/bash
# Build, write manifests/latest.json, then prove the patcher reproduces the build byte for byte.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: tools/release.sh VERSION}
# boot screen and About page show fw/version.h – it must match the release version
FWV=$(sed -n 's/^#define ENVYU_VERSION "\(.*\)"/\1/p' fw/version.h 2>/dev/null)
[ "$FWV" = "$V" ] || { echo "version mismatch: fw/version.h says '$FWV', release is '$V' (edit version.h in the source, then tools/sync_fw.sh)"; exit 1; }
python3 -I tools/hex2bin.py stock/PolyendPlay_v1.6.0.ppf stock/firmware.bin > /dev/null
(cd fw && ./build.sh > /dev/null)
python3 -I tools/make_manifest.py stock/PolyendPlay_v1.6.0.ppf fw/PolyendPlay_v1.6.0_mod.ppf fw/fx.bin manifests/latest.json --version "$V"
cp manifests/latest.json docs/manifest.json
T=$(mktemp -d)
python3 -I patcher/play_patch.py stock/PolyendPlay_v1.6.0.ppf -o "$T/out.ppf"
cmp "$T/out.ppf" fw/PolyendPlay_v1.6.0_mod.ppf && echo "patcher output == build"
rm -r "$T"
