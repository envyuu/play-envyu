#!/usr/bin/env python3
"""17.11.2026: Ladebildschirm (Projekt/Samples laden) im Emulator – echter Widget-Manager, echte Klasse 0xA05F8.

A) Original-Image: P-Logo (100×100 bei x 110…209, y 60…159) + Balken.
B) Gepatcht: kein P, stattdessen das Endbild der Animation (Wort-Pads hell, ū-Strich), Fusszeile; Balken unveraendert.
C) Gepatcht + Sicherheitsnetz (Merker 0xA5A5A5A5): pixelgleich mit A.
Bilder: screens/00e_load_original.png, 00f_load_envyu.png
usage: emu_ldscreen.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin
"""
import sys, os, struct
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import emu_display as ED  # noqa: E402
from emu_nis import load_ppf  # noqa: E402
from emu_slicer import syms  # noqa: E402
from eve_emu import save_png  # noqa: E402

fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)

ROWS = [0x00000, 0x00000, 0x55537, 0x55555, 0x56557, 0x54551, 0x74257, 0x03000]
PAD, PITCH, GX, GY = 11, 14, 21, 46
def center(r, c): return GY + r * PITCH + PAD // 2, GX + c * PITCH + PAD // 2

def render(img, patched, nm, bad=False):
    orig_rig = ED.Rig
    if bad:
        def rig_bad(*a, **k):
            r = orig_rig(*a, **k); r.e.w32(0x1FFF9A38, 0xA5A5A5A5); return r
        ED.Rig = rig_bad
    try: d = ED.Disp(img, patched, nm)
    finally: ED.Rig = orig_rig
    e = d.e; M = ED.MGR
    def mgr(off, *a): return e.call(e.r32(0x9AA1C + off), M, *a)
    arr = e.r32(M + 8)
    for k in range(50):                                   # Seite der Test-Umgebung ausblenden
        if e.r32(arr + 4 * k): mgr(0x28, k)
    lv = e.call(0x30C0, 0x60); e.uc.mem_write(lv, bytes(0x60))
    e.w32(lv, 0xA05F8); e.w32(lv + 4, M); e.w32(lv + 0x24, ED.THEME)
    e.call(struct.unpack_from("<I", img, 0xA05FC)[0], lv)   # vt[1] – im gepatchten Image ueber die Weiche
    logo, bar = e.r32(lv + 0x2C), e.r32(lv + 0x30)
    mgr(0x24, logo); mgr(0x24, bar); mgr(0x38, bar, 500)
    mgr(0x08, logo); mgr(0x08, bar); mgr(0x0C, 0)
    d.pump()
    obj = e.r32(arr + 4 * logo)
    return d.eve.frames[-1].copy(), e.r32(obj), logo, bar

def main():
    ppf, orig = sys.argv[1], sys.argv[2]
    nm = syms(); img = load_ppf(ppf); oimg = bytearray(open(orig, "rb").read())
    fo, vto, _, _ = render(oimg, False, {})
    save_png(fo, os.path.join(ED.SCREENS, "00e_load_original.png"))
    g = fo.astype(int).max(axis=2)
    print("A) Original")
    p_px = int((g[60:160, 110:210] > 100).sum()); bar_px = int((g[185:190, 83:237] > 60).sum())
    check(vto == 0x9B510 and p_px > 500, f"P-Logo sichtbar ({p_px} helle Pixel), Textklasse {vto:#x}")
    check(bar_px > 50, f"Balken sichtbar ({bar_px} Pixel)")
    print("B) Gepatcht: Endbild der Animation statt P")
    fp, vtp, _, _ = render(img, True, nm)
    save_png(fp, os.path.join(ED.SCREENS, "00f_load_envyu.png"))
    g2 = fp.astype(int).max(axis=2)
    word = [g2[center(r, c)] for r in range(8) for c in range(20) if (ROWS[r] >> c) & 1]
    mac = [g2[center(0, c)] for c in (16, 17, 18)]
    rest = [g2[center(r, c)] for r in range(8) for c in range(20) if not (ROWS[r] >> c) & 1 and not (r == 0 and c in (16, 17, 18))]
    check(vtp == nm["LD_VT"], f"Logo-Widget hat unsere vtable ({vtp:#x})")
    check(len(word) == 52 and min(word) >= 200 and max(mac) >= 240 and max(rest) < 60, f"Wort-Pads hell ({min(word)}…{max(word)}), ū-Strich {max(mac)}, Rest dunkel (max {max(rest)})")
    check(int((g2[196:240, 10:120] > 80).sum()) > 100, "Fusszeile sichtbar")
    bar2 = int((g2[185:190, 83:237] > 60).sum())
    check(abs(bar2 - bar_px) <= 2 and np.array_equal(fp[180:195], fo[180:195]), f"Balken unveraendert ({bar2} Pixel, Zeilen 180–194 pixelgleich mit Original)")
    print("C) Sicherheitsnetz")
    fb, vtb, _, _ = render(img, True, nm, bad=True)
    check(vtb == 0x9B510 and np.array_equal(fb, fo), "Merker BAD → Original-Ladebildschirm, pixelgleich")
    print(f"ERGEBNIS: {checks - fails}/{checks} Pruefungen" + (f", {fails} FEHLER" if fails else ""))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
