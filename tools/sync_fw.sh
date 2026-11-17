#!/bin/bash
# Copy the publishable firmware sources from the private working tree (single source of truth) into fw/
# and rewrite the private paths. Nothing here is committed automatically.
#   usage: tools/sync_fw.sh [SRC]     (default: ~/Projekte/mein_synth_re/patches/046_usb_iad)
set -e
cd "$(dirname "$0")/.."
SRC=${1:-$HOME/Projekte/mein_synth_re/patches/046_usb_iad}
[ -f "$SRC/build.sh" ] || { echo "no build.sh in $SRC"; exit 1; }
# private only: user recordings, measurement scripts, bootloader marker test, granular tests/fuzzer
EXCL='^(check_018\.py|test_data|screens|analyse_.*\.py|marker\.c|emu_granular\.py|fuzz_granular\.py|.*\.o|.*\.elf|.*\.bin|.*\.ppf|.*\.dSYM|.*_test|fuzz_test|host_test|sd_test|__pycache__|README\.md)$'
mkdir -p fw
for f in $(ls "$SRC"); do
  echo "$f" | grep -Eq "$EXCL" && continue
  [ -f "$SRC/$f" ] || continue
  cp "$SRC/$f" "fw/$f"
done
# private paths → repo paths
for f in fw/*.sh fw/*.py fw/*.c; do
  sed -i.bak \
    -e 's#\$R/work/scripts/#$R/tools/#g' \
    -e 's#\$R/work/firmware\.bin#$R/stock/firmware.bin#g' \
    -e 's#\$R/firmware/mein_synth\.ppf_/mein_synth\.ppf#$R/stock/PolyendPlay_v1.6.0.ppf#g' \
    -e 's#\.\./\.\./work/firmware\.bin#../stock/firmware.bin#g' \
    -e 's#\.\./\.\./work/testbeat_120bpm\.wav#../stock/testbeat_120bpm.wav#g' \
    -e 's#R=\.\./\.\.$#R=..#' \
    -e 's#PolyendPlay_v1\.6\.0_slicer\.ppf#PolyendPlay_v1.6.0_mod.ppf#g' \
    -e 's#\.\./012_usb_audio/desc\.bin#fixtures/desc_stereo.bin#g' \
    -e 's#\.\./012_usb_audio/check_desc\.py#fixtures/check_desc_stereo.py#g' \
    -e 's#os\.path\.join(HERE, "\.\.", "\.\.", "firmware", "mein_synth\.ppf_", "mein_synth\.ppf")#os.path.join(HERE, "..", "stock", "PolyendPlay_v1.6.0.ppf")#g' \
    -e 's#R=\$(cd \.\./\.\. \&\& pwd)#R=$(cd .. \&\& pwd)#g' \
    -e 's#-w /w/patches/\$(basename "\$PWD")#-w /w/fw#g' \
    "$f" && rm -f "$f.bak"
done
cp "$SRC/../../work/scripts/patch_ppf.py" "$SRC/../../work/scripts/thumb_bl.py" tools/   # Build-Werkzeuge (Segmentwechsel > 64 KB)
mkdir -p tools/docker32 && cp "$SRC/../../work/docker32/Dockerfile" tools/docker32/ 2>/dev/null || true
sed -i.bak -e 's#work/docker32#tools/docker32#g' -e 's#work/uc/bin/pip#pip#g' fw/test32.sh fw/test_all.sh && rm -f fw/*.bak
rm -f fw/modinit.c fw/lr_test.c        # replaced by sl_init / wv_test in the new sources
echo "synced from $SRC"; grep -l "work/\|mein_synth" fw/* 2>/dev/null | sed 's/^/WARNING private path left in: /' || true
