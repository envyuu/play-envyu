#!/usr/bin/env python3
"""Patch 017: prueft die Deskriptor-Varianten, die usbmenu.c zur Laufzeit nach 0x1FFFD4C0 kopiert.
  Stereo (240 B): muss bytegleich mit 012/desc.bin im gebauten fx.bin liegen, UAC-Pruefung mit dem Pruefer aus 012.
  Off (149 B):    ua_cfg_desc[0:149] mit wTotalLength 149 und bNumInterfaces 3 – Kettenpruefung, nur IF0..IF2,
                  Bytes 9..149 identisch mit dem Original-Deskriptor der Firmware (CDC + MIDI)."""
import subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
fx = open(os.path.join(here, "fx.bin"), "rb").read()
st = open(os.path.join(here, "fixtures/desc_stereo.bin"), "rb").read()
full = open(os.path.join(here, "desc_fw.bin"), "rb").read()
img = open(os.path.join(here, "../stock/firmware.bin"), "rb").read()
ok = True
def say(c, t):
    global ok
    print(("  ok   " if c else "  FEHLER ") + t); ok &= bool(c)

say(fx.count(st) == 1, "Stereo-Deskriptor (240 B) genau einmal im Binary, bytegleich mit 012")
r = subprocess.run([sys.executable, "-I", os.path.join(here, "fixtures/check_desc_stereo.py"), os.path.join(here, "fixtures/desc_stereo.bin"),
                    "--orig", os.path.join(here, "../stock/firmware.bin")], capture_output=True, text=True)
say(r.returncode == 0 and "ERGEBNIS: OK" in r.stdout, "Stereo: UAC-Pruefung (Pruefer aus 012): " + r.stdout.strip().splitlines()[-1])

off = bytearray(full[:149]); off[2] = 149; off[3] = 0; off[4] = 3
i, ifs, chain = 0, set(), True
while i < len(off):
    L = off[i]
    if L < 2 or i + L > len(off): chain = False; break
    if off[i + 1] == 4: ifs.add(off[i + 2])
    i += L
say(chain and i == 149, "Off: Deskriptorkette endet genau bei 149 Byte")
say(ifs == {0, 1, 2}, f"Off: Interfaces {sorted(ifs)} = CDC (0,1) + MIDI (2)")
orig = img[0xAED68:0xAED68 + 181]
say(bytes(off[9:149]) == orig[9:149], "Off: Bytes 9..149 identisch mit dem Original (IAD, CDC, MIDI)")
say(off[0:2] == b"\x09\x02" and off[5:9] == orig[5:9], "Off: Konfigurationskopf wie Original (bConfigurationValue, Attribute, Strom)")
print("ERGEBNIS:", "OK" if ok else "FEHLER")
sys.exit(0 if ok else 1)
