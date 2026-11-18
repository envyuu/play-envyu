#!/bin/bash
# Alle PC-Tests von Patch 045 (Release-Stand + Slicer, Granular ausgeblendet) (kein Geraetezugriff). UNICORN_PY=/pfad/zu/venv/bin/python fuer die Maschinencode-Tests.
set -eo pipefail
cd "$(dirname "$0")"
# 32-Bit-Host-Tests brauchen gcc -m32 (Linux mit multilib). macOS kann das nicht → dort ausgelassen, auf CachyOS nachholen.
M32=1; echo 'int main(void){return 0;}' | gcc -m32 -x c -o /dev/null - 2>/dev/null || M32=0
SKIP32=""
# macOS (Mach-O) kennt keine ELF-Sektionsnamen und keine fehlenden weak-Symbole → nur fuer Host-Tests neutralisieren.
if [ "$(uname)" = Darwin ]; then gcc() { command gcc '-Dsection(x)=' -Wl,-undefined,dynamic_lookup "$@"; }; fi
g32() { if [ $M32 = 1 ]; then return 0; fi; SKIP32="$SKIP32 $1"; echo "   UEBERSPRUNGEN (kein gcc -m32) – auf CachyOS nachholen"; return 1; }
SAN="-O1 -g -Wall -Wextra -DHOST_TEST -fsanitize=address,undefined -fno-sanitize-recover=all"
echo "== 1. Host-Test Stereo (012-Teil: Ring, Regler, Setup IF4; ASan/UBSan)"
gcc $SAN -DNO_UM -DNO_UI -o ua_test usbaudio.c multitrack.c ua_test.c -lm
./ua_test desc.bin | tail -1
echo "== 2. Host-Test Multitrack (Setup IF5 AC + IF6 AS, Routing, Rampen, Export, Alt-Wechsel, Simulation; ASan/UBSan)"
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
  B=${BASELINE_DIR:-..}   # A/B-Vergleich mit alten Builds (privat: Nachbarordner; Release-Repo: optional)
  if [ -f "$B/012_usb_audio/PolyendPlay_v1.6.0_usbaudio_ENTWURF.ppf" ]; then $PY emu_mt.py PolyendPlay_v1.6.0_mod.ppf "$B/012_usb_audio/PolyendPlay_v1.6.0_usbaudio_ENTWURF.ppf" | tail -1
  else echo "   SKIPPED emu_mt.py (A/B test, needs BASELINE_DIR)"; fi
  $PY emu_menu.py PolyendPlay_v1.6.0_mod.ppf | tail -1
else echo "   uebersprungen (unicorn fehlt: python3 -m venv v && v/bin/pip install unicorn; UNICORN_PY=v/bin/python)"; fi
echo "== 8. Arp (Patch 014, 32 Bit, ASan/UBSan)"
echo "   → test32.sh (Abschnitt 8)"
echo "== 7b. Spurauswahl im Emulator (echter Notenweg triggerStep bis sendNote)"
$PY emu_fxsel.py PolyendPlay_v1.6.0_mod.ppf | tail -1
echo "== 9. FX-Suite inkl. Loop-Fix (Szenarien + Zufallstest)"
gcc -O1 -DHOST_TEST -DNO_ARP -fsanitize=address,undefined -o host_test fx.c host_test.c && ./host_test | tail -1
gcc -O1 -DHOST_TEST -DNO_ARP -fsanitize=address,undefined -o fuzz_test fx.c fuzz_test.c && ./fuzz_test | tail -1
echo "== 9b. Spurauswahl der FX-Suite (Drums + Synth, Szenarien + Zufallstest; ASan/UBSan)"
gcc -O1 -g -DHOST_TEST -DNO_ARP -fsanitize=address,undefined -fno-sanitize-recover=all -o fxsel_test fx.c fxsel_test.c && ./fxsel_test | tail -1
echo "== 10. USB-Upload inkl. GET (Szenarien + Zufallstest; ASan/UBSan)"
gcc $SAN -DNO_UM -o up_test usbup.c up_test.c 2>/dev/null && ./up_test | tail -1
echo "== 11. Menue USB Audio / Config-Datei / Neuanmeldung / Classic-Creative (32 Bit, ASan/UBSan, 5 Startwerte)"
echo "   → test32.sh (Abschnitt 11)"
echo "== 12. Varianten-Deskriptoren (Stereo, Off) gegen UAC-Pruefung"
python3 -I check_variants.py
echo "== 13. Note In Scale (Patch 018): Host-Test (32 Bit, ASan/UBSan, Scale-Masken aus dem echten Image)"
echo "   → test32.sh (Abschnitt 13)"
echo "== 14. Note In Scale im Emulator (echte Randomize-Funktionen) + Patchstellen-Vergleich mit 018"
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_nis.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
  echo "== 14b. 045 (11.11.): neue Randomize-Typen MIDI + Audio (Shuffle, Walk, Accent, Call & Response, Thin Out), Audio-Liste, Einbacken zweistufig"
  $PY emu_rand.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 15. USB-Eingang (Patch 020): Host-Test (Setup IF5, Ring, Rueckmeldung +-300 ppm, Mixer/Send, Zufall; 32 Bit, ASan/UBSan)"
echo "   → test32.sh (Abschnitt 15)"
echo "== 16. USB-Eingang im Emulator (echter usbotg_isr, um_poll, Mixer 0x15884 + Hooks) gegen 019"
python3 -I check_desc.py desc_in_fw.bin --orig ../stock/firmware.bin | tail -1
if $PY -c "import unicorn" 2>/dev/null; then
  B=${BASELINE_DIR:-..}
  if [ -f "$B/019_menu_nis/PolyendPlay_v1.6.0_menu_nis.ppf" ]; then $PY emu_ui.py PolyendPlay_v1.6.0_mod.ppf "$B/019_menu_nis/PolyendPlay_v1.6.0_menu_nis.ppf" | tail -1
  else echo "   SKIPPED emu_ui.py (A/B test, needs BASELINE_DIR)"; fi
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 17. Slicer/Granular (Patch 021): Host-Test (32 Bit, ASan/UBSan)"
echo "   → test32.sh (Abschnitt 17)"
echo "== 18. Slicer/Granular im Emulator (echter Trigger-/Render-Pfad: resolveEvent, FUN_000166F4, Sampler_render)"
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_slicer.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 19. Aufnahme USB-Eingang (Patch 024): Host-Test (WAV, Nummern, Verluste, Fehler, Menuezeile, Zufall; 32 Bit, ASan/UBSan)"
echo "   → test32.sh (Abschnitt 19)"
echo "== 20. Aufnahme im Emulator (echter Maschinencode, Aufrufkonvention aller FatFs-Aufrufe, u. a. f_lseek 64 Bit)"
if $PY -c "import unicorn" 2>/dev/null; then
  $PY emu_rec.py PolyendPlay_v1.6.0_mod.ppf | tail -1
  echo "== 32. Sicherheitsnetz (13.11.): Kopf/CRC, Weichen an allen Patchstellen, Notfall-Start mit Original-Init"
  $PY emu_guard.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
  echo "== 31. Perform (045, 12.11.): Spurauswahl bleibt, Anschlag-Blinken im Mute-Pad (triggerStep-Nachbau, pf_poll)"
  $PY emu_pf.py PolyendPlay_v1.6.0_mod.ppf | tail -1
else echo "   uebersprungen (unicorn fehlt)"; fi
echo "== 21. Waveform + Live Rec (Patch 023): Host-Test (Spalten, Min/Max, Bitmap-Zeilen, Layout Slicer/Granular, Tasten-Zustandsmaschine; 32 Bit, ASan/UBSan)"
echo "   → test32.sh (Abschnitt 21)"
echo "== 22. Display im Emulator (echter Grafik-Init, Widget-Manager, Step-Parameter-Seite + FT81x-Modell) → screens/*.png"
if $PY -c "import unicorn, numpy, PIL" 2>/dev/null; then
  $PY emu_display.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
else echo "   uebersprungen (unicorn/numpy/pillow fehlen: pip install numpy pillow)"; fi
echo "== 22b. Startbild (046): echter Startweg Gfx_init → bs_boot, 26 Bilder, Sicherheitsnetz → Original → screens/00b–00d"
$PY emu_boot.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
echo "== 22c. Ladebildschirm (17.11.): Endbild der Animation statt P-Logo, Balken Original, Sicherheitsnetz → Original"
$PY emu_ldscreen.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin | tail -1
echo "== 23. Ablauf (Patch 030): echter Knob-Pfad, Step-Seite, Display, 1500 Zufallsschritte"
$PY emu_flow.py PolyendPlay_v1.6.0_mod.ppf | tail -1
echo "== 24. Beat-Erkennung an echtem Material (Testbeat, Live-Aufnahme des Nutzers)"
echo "   → test32.sh (Abschnitt 24)"
echo "== 25. Regler in View mit gewaehlter Spur im echten Firmware-Code (Patch 034)"
$PY emu_vorlage.py PolyendPlay_v1.6.0_mod.ppf | tail -1
echo "== 26. Slice-Ende ohne Knacken (Patch 036): echter Render-Pfad, Matrix Tonhoehe/Slice-Laenge/Decay"
$PY klick_matrix.py PolyendPlay_v1.6.0_mod.ppf | head -1
$PY klick_matrix.py PolyendPlay_v1.6.0_mod.ppf test
echo "== 26c. Color Mode (045): echter Leds_setColor → cb_pixel → Leds_setPixel"
$PY emu_cb.py PolyendPlay_v1.6.0_mod.ppf | tail -1
echo "== 26b. Stop auf Slicer-Spuren (045): Ausblenden in ~9 ms, echter Render-Pfad"
$PY klick_stop.py PolyendPlay_v1.6.0_mod.ppf
echo "== 27./28. Granular: in 045 ausgeblendet (sl_is_granular = 0), Tests nur mit WITH_GRANULAR-Build"
echo "== 30. Tap Tempo im Menue (Patch 045): echte Hooks, echter Tempo-Setter 0x21030 (Hard-Float)"
$PY emu_tap.py PolyendPlay_v1.6.0_mod.ppf | tail -1
echo "== 29. Kaltstart (Patch 043): Start mit beliebigem RAM-Inhalt"
$PY emu_kaltstart.py PolyendPlay_v1.6.0_mod.ppf | tail -1
echo "== 32-Bit-Hosttests (8, 11, 13, 15, 17, 19, 21, 24) → test32.sh"
if [ $M32 = 1 ]; then ./test32.sh
elif command -v docker >/dev/null && DOCKER_HOST=${DOCKER_HOST:-unix://$HOME/.colima/default/docker.sock} docker image inspect play-test32 >/dev/null 2>&1; then
  export DOCKER_HOST=${DOCKER_HOST:-unix://$HOME/.colima/default/docker.sock}
  R=$(cd .. && pwd)
  T32=$(mktemp); docker run --rm -v "$R":/w -w /w/fw play-test32 bash test32.sh 2>&1 | tee "$T32"
  if grep -qE "error:|undefined reference|FEHLER \(|[1-9][0-9]* Fehler" "$T32"; then echo "32-BIT-TESTS: FEHLER (Compile/Link oder Pruefung)"; rm -f "$T32"; exit 1; fi; rm -f "$T32"
else
  echo "!! NICHT VOLLSTAENDIG: kein gcc -m32 und kein Container (colima start, Image aus tools/docker32)."
  echo "!! Vor einem Flash die volle Testrunde auf CachyOS laufen lassen."; exit 1
fi
