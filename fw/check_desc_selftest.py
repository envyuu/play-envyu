#!/usr/bin/env python3
"""Gegenprobe fuer check_desc.py: gezielt kaputte Varianten von desc.bin muessen abgelehnt werden."""
import subprocess, sys, os, tempfile
d = bytearray(open(sys.argv[1], "rb").read())
def find(seq):
    i = bytes(d).find(bytes(seq)); assert i >= 0, seq; return i
ac_hdr = find([0x0A, 0x24, 0x01, 0x00, 0x01]); ep = find([0x09, 0x05, 0x86]); fmt = find([0x0B, 0x24, 0x02, 0x01])
as1 = find([0x09, 0x04, 0x04, 0x01]); gen = find([0x07, 0x24, 0x01, 0x02])
# 015: Multitrack
it3 = find([0x0C, 0x24, 0x02, 0x03]); ot4 = find([0x09, 0x24, 0x03, 0x04])
m1 = find([0x09, 0x04, 0x05, 0x01]); m2 = find([0x09, 0x04, 0x05, 0x02])
f10 = find([0x0B, 0x24, 0x02, 0x01, 0x0A]); f11 = find([0x0B, 0x24, 0x02, 0x01, 0x0B])
e10 = find([0x09, 0x05, 0x87, 0x05, 0x84]); e11 = find([0x09, 0x05, 0x87, 0x05, 0xDE])
cases = {
    "wTotalLength falsch":            [(2, 0xEF)],
    "bNumInterfaces 4":               [(4, 4)],
    "AC wTotalLength falsch":         [(ac_hdr + 5, 0x1F)],
    "baInterfaceNr zeigt auf CDC":    [(ac_hdr + 8, 1)],
    "wMaxPacketSize 176 zu klein":    [(ep + 4, 176)],
    "Endpunkt Bulk statt isochron":   [(ep + 3, 0x02)],
    "bInterval 4":                    [(ep + 6, 4)],
    "3 Kanaele im Format":            [(fmt + 4, 3)],
    "bTerminalLink auf IT":           [(gen + 3, 1)],
    "Alt 1 als Alt 2":                [(as1 + 3, 2)],
    "EP-Adresse kollidiert mit MIDI": [(ep + 2, 0x84)],
    "CDC-Byte veraendert":            [(20, 0x03)],
    "IF5 fehlt in baInterfaceNr":     [(ac_hdr + 9, 4)],
    "IT3 nur 9 Kanaele":              [(it3 + 7, 9)],
    "Alt 2 mit 12 Kanaelen":          [(f11 + 4, 12)],
    "OT4 Quelle unbekannt":           [(ot4 + 7, 9)],
    "EP 0x87 900 → 880 (zu klein)":   [(e10 + 4, 0x70)],
    "EP 0x87 990 → 1102 (> 1023)":    [(e11 + 4, 0x4E), (e11 + 5, 0x04)],
    "EP 0x87 kollidiert mit IF4":     [(e10 + 2, 0x86)],
    "IF5 Alt 2 als Alt 3":            [(m2 + 3, 3)],
    "IF5 Alt 1 ohne Endpunkt":        [(m1 + 4, 0)],
    "Format 10 Kan. mit 24 Bit/2 B":  [(f10 + 6, 24)],
}
bad = 0
for name, muts in cases.items():
    x = bytearray(d)
    for o, v in muts: x[o] = v
    with tempfile.NamedTemporaryFile(delete=False) as f: f.write(x); fn = f.name
    r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_desc.py"), fn, "--orig", sys.argv[2]], capture_output=True, text=True)
    os.unlink(fn)
    hit = r.returncode != 0
    print(("  erkannt   " if hit else "  NICHT erkannt ") + name)
    bad += not hit
print("Gegenprobe:", "OK" if not bad else f"{bad} nicht erkannt"); sys.exit(1 if bad else 0)
