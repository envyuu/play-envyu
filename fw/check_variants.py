#!/usr/bin/env python3
"""Patch 017: prueft die Deskriptor-Varianten, die usbmenu.c zur Laufzeit nach 0x1FFFD4C0 kopiert.
  Stereo (248 B): 049: Anfang von ua_cfg_desc, = 012/desc.bin + IAD Audio (046); Stereo + Multi (365 B) = 046-Deskriptor.
  Off (149 B):    ua_cfg_desc[0:149] mit wTotalLength 149 und bNumInterfaces 3 – Kettenpruefung, nur IF0..IF2,
                  Bytes 9..149 identisch mit dem Original-Deskriptor der Firmware (CDC + MIDI)."""
import subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
fx = open(os.path.join(here, "fx.bin"), "rb").read()
st012 = open(os.path.join(here, "fixtures/desc_stereo.bin"), "rb").read()
st = bytearray(st012[:149] + bytes([8, 0x0B, 3, 2, 1, 1, 0, 0]) + st012[149:]); st[2] = len(st) & 0xFF; st[3] = len(st) >> 8   # 046: + IAD Audio
st = bytes(st)
open(os.path.join(here, "desc_st_fw.bin"), "wb").write(st)
full = open(os.path.join(here, "desc_fw.bin"), "rb").read()
img = open(os.path.join(here, "../stock/firmware.bin"), "rb").read()
ok = True
def say(c, t):
    global ok
    print(("  ok   " if c else "  FEHLER ") + t); ok &= bool(c)

# 049: Stereo und Stereo + Multi (kombiniert) entstehen zur Laufzeit aus ua_cfg_desc (getrennt, 390 B, = desc_fw.bin)
sp = open(os.path.join(here, "desc_fw.bin"), "rb").read()
d = bytearray(sp[:248]); d[2], d[3], d[4] = 248, 0, 5
say(bytes(d) == st, "Stereo = ua_cfg_desc[0:248] mit Kopf 248/5 = 012 + IAD Audio (046)")
c = bytearray(365)
for a, b_, n in ((0, 0, 166), (176, 175, 21), (197, 274, 21), (218, 196, 52), (270, 295, 95)): c[a:a + n] = sp[b_:b_ + n]   # = COMB_SEG (usbmenu.c)
c[166:176] = bytes([0x0A, 0x24, 0x01, 0x00, 0x01, 0x34, 0x00, 0x02, 0x04, 0x05]); c[4] = 6; c[152] = 3; c[2], c[3] = 365 & 255, 365 >> 8
i = 270
while i < 365:
    if c[i + 1] == 4: c[i + 2] = 5
    i += c[i]
import hashlib
say(hashlib.md5(bytes(c)).hexdigest() == "bdc96f9abfc5d798fc0367592559c489", "Stereo + Multi (kombiniert) = Deskriptor von 046 (am Mac bewaehrt)")
open(os.path.join(here, "desc_comb_fw.bin"), "wb").write(bytes(c))
for name, fn in (("Stereo", "desc_st_fw.bin"), ("Stereo + Multi", "desc_comb_fw.bin")):
    r = subprocess.run([sys.executable, "-I", os.path.join(here, "check_desc.py"), os.path.join(here, fn),
                    "--orig", os.path.join(here, "../stock/firmware.bin")], capture_output=True, text=True)
    say(r.returncode == 0 and "ERGEBNIS: OK" in r.stdout, f"{name}: UAC-Pruefung (check_desc.py mit IAD-Regel): " + r.stdout.strip().splitlines()[-1])

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
