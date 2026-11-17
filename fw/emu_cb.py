#!/usr/bin/env python3
"""045: Color Mode – echter Leds_setColor 0x4DC4 → bl (gepatcht) cb_pixel → Leds_setPixel 0x6CD4 (Puffer-Modus obj[0] == 2)."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_slicer import Rig, syms, load_ppf
fails = 0; n = 0
def check(c, m):
    global fails, n
    n += 1
    if not c: fails += 1
    print(("  ok    " if c else "  FEHLER ") + m)
def main(ppf):
    nm = syms(); r = Rig(load_ppf(ppf), True, nm); e = r.e
    OBJ = 0x1FFF0100; RGB = 0x1FFF0F00; CB = 0x1FFFB860
    e.uc.mem_write(OBJ, bytes(0xC00)); e.wf(OBJ + 0xB84, 1.0); e.uc.mem_write(OBJ + 0x2B0, b"\x02")
    def led(rgb, mode, i=5):
        e.w32(CB, mode & 0xFFFFFFFF); e.uc.mem_write(RGB, bytes(rgb))
        e.call(0x4DC5, OBJ, i, RGB, 0, 0)
        return list(e.uc.mem_read(OBJ + 0x2B0 + i * 4 + 0x19, 3))
    def model(c, m):
        R, G, B = c
        if m == 1: return [min(255, R + B), R, (G + B) >> 1]
        if m == 2: return [R, (R + G) >> 1, B]
        return [R, G, B]
    cols = [(255, 24, 0), (0, 204, 102), (25, 9, 255), (255, 255, 96), (145, 30, 255), (255, 255, 255), (0, 0, 0), (56, 59, 15)]
    for m in (0, 1, 2):
        got = [led(c, m) for c in cols]; want = [model(c, m) for c in cols]
        check(got == want, f"Modus {m}: {len(cols)} Pad-Farben wie berechnet" + ("" if got == want else f" {got} statt {want}"))
    check(led((255, 24, 0), 0x55AA77) == [255, 24, 0], "undefinierter Wert (Kaltstart-SRAM) = Normal")
    print(f"{n - fails}/{n} Pruefungen" + ("" if not fails else f", {fails} FEHLER"))
    return fails
if __name__ == "__main__": sys.exit(1 if main(sys.argv[1]) else 0)
