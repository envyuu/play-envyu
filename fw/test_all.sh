#!/bin/bash
# All PC tests, no device access. Needs ../stock/PolyendPlay_v1.6.0.ppf and ../stock/firmware.bin (see README).
# UNICORN_PY=path/to/venv/bin/python enables the machine-code tests.
# Optional: BASELINE_DIR = folder with old builds (012_usb_audio, 019_menu_nis) for the two A/B tests.
set -eo pipefail
cd "$(dirname "$0")"
SAN="-O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all"
echo "== 1. Host-Test Stereo (012-Teil: Ring, Regler, Setup IF4; ASan/UBSan)"
gcc $SAN -DNO_UM -DNO_UI -o ua_test usbaudio.c multitrack.c ua_test.c -lm
./ua_test desc.bin | tail -1
echo "== 2. Host-Test Multitrack (Setup IF5, Routing, Rampen, Export, Alt-Wechsel, Simulation; ASan/UBSan)"
gcc $SAN -DNO_UM -DNO_UI -o mt_test usbaudio.c multitrack.c mt_test.c -lm
./mt_test | tail -1
echo "== 3. Deskriptor-Pruefung (UAC 1.0, 2 AS-Interfaces) + Gegenprobe"
python3 -I check_desc.py desc.bin --orig ../stock/firmware.bin | tail -1
python3 -I check_desc_selftest.py desc.bin ../stock/firmware.bin | tail -1
echo "== 4. Auswerteskripte (Selbsttest)"
python3 -I analyse_aufnahme.py --selbsttest | tail -1
python3 -I analyse_mt.py --selbsttest | tail -1
echo "== 5. Bauen + PPF-Pruefung"
./build.sh | tail -3
cmp desc.bin desc_fw.bin && echo "   desc.bin (Host) == Deskriptor im gebauten Binary"
echo "== 6./7. Maschinencode im Emulator (echter usbotg_isr, audio_callback, Mixer 0x15884 + Hooks)"
PY=${UNICORN_PY:-python3}
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_isr.py PolyendPlay_v1.6.0_mod.ppf | tail -1
  if [ -n "$BASELINE_DIR" ]; then $PY emu_mt.py PolyendPlay_v1.6.0_mod.ppf "$BASELINE_DIR/012_usb_audio/PolyendPlay_v1.6.0_usbaudio_ENTWURF.ppf" | tail -1
  else echo "   SKIPPED emu_mt.py (A/B test, needs BASELINE_DIR)"; fi
  $PY emu_menu.py PolyendPlay_v1.6.0_mod.ppf | tail -1
else echo "   uebersprungen (unicorn fehlt: python3 -m venv v && v/bin/pip install unicorn; UNICORN_PY=v/bin/python)"; fi
echo "== 7b. Spurauswahl im Emulator (echter Notenweg triggerStep bis sendNote)"
if $PY -c "import unicorn" 2>/dev/null; then $PY emu_fxsel.py PolyendPlay_v1.6.0_mod.ppf | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 8. Arp (Patch 014, 32 Bit, ASan/UBSan)"
gcc -m32 -O1 -DHOST_TEST -fsanitize=address,undefined -o arp_test arp.c arp_test.c && ./arp_test | tail -1
echo "== 9. FX-Suite inkl. Loop-Fix (Szenarien + Zufallstest)"
gcc -O1 -DHOST_TEST -DNO_ARP -fsanitize=address,undefined -o host_test fx.c host_test.c && ./host_test | tail -1
gcc -O1 -DHOST_TEST -DNO_ARP -fsanitize=address,undefined -o fuzz_test fx.c fuzz_test.c && ./fuzz_test | tail -1
echo "== 9b. Spurauswahl der FX-Suite (Drums + Synth, Szenarien + Zufallstest; ASan/UBSan)"
gcc -O1 -g -DHOST_TEST -DNO_ARP -fsanitize=address,undefined -fno-sanitize-recover=all -o fxsel_test fx.c fxsel_test.c && ./fxsel_test | tail -1
echo "== 10. USB-Upload inkl. GET (Szenarien + Zufallstest; ASan/UBSan)"
gcc $SAN -DNO_UM -o up_test usbup.c up_test.c 2>/dev/null && ./up_test | tail -1
echo "== 11. Menue USB Audio / Config-Datei / Neuanmeldung / Classic-Creative (32 Bit, ASan/UBSan, 5 Startwerte)"
gcc -m32 $SAN -DNO_UI -o um_test usbmenu.c um_test.c && for s in 1 2 3 4 5; do ./um_test $s | tail -1; done
echo "== 12. Varianten-Deskriptoren (Stereo, Off) gegen UAC-Pruefung"
python3 -I check_variants.py
echo "== 13. Note In Scale (Patch 018): Host-Test (32 Bit, ASan/UBSan, Scale-Masken aus dem echten Image)"
gcc -m32 -O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all -o nis_test nis.c nis_test.c
./nis_test ../stock/firmware.bin | tail -1
echo "== 14. Note In Scale im Emulator (echte Randomize-Funktionen) + Patchstellen-Vergleich mit 018"
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_nis.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 15. USB-Eingang (Patch 020): Host-Test (Setup IF5, Ring, Rueckmeldung +-300 ppm, Mixer/Send, Zufall; 32 Bit, ASan/UBSan)"
gcc -m32 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -o ui_test ui_test.c -lm
for s in 1 2 3; do ./ui_test $s | tail -1; done
echo "== 16. USB-Eingang im Emulator (echter usbotg_isr, um_poll, Mixer 0x15884 + Hooks) gegen 019"
python3 -I check_desc.py desc_in_fw.bin --orig ../stock/firmware.bin | tail -1
if $PY -c "import unicorn" 2>/dev/null; then
  if [ -n "$BASELINE_DIR" ]; then $PY emu_ui.py PolyendPlay_v1.6.0_mod.ppf "$BASELINE_DIR/019_menu_nis/PolyendPlay_v1.6.0_menu_nis.ppf" | tail -1
  else echo "   SKIPPED emu_ui.py (A/B test, needs BASELINE_DIR)"; fi
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 19. Aufnahme USB-Eingang (Patch 024): Host-Test (WAV, Nummern, Verluste, Fehler, Menuezeile, Zufall; 32 Bit, ASan/UBSan)"
gcc -m32 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -o rec_test rec_test.c
for s in 1 2 3; do ./rec_test $s | tail -1; done
echo "== 20. Aufnahme im Emulator (echter Maschinencode, Aufrufkonvention aller FatFs-Aufrufe, u. a. f_lseek 64 Bit)"
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_rec.py PolyendPlay_v1.6.0_mod.ppf | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 21. Live Rec long press: host test (state machine, status text; 32 bit, ASan/UBSan)"
gcc -m32 -O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all -o lr_test lrec.c lr_test.c
./lr_test | tail -1
echo "== 29. Cold start: start with any RAM content"
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_kaltstart.py PolyendPlay_v1.6.0_mod.ppf | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
