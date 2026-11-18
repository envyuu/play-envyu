#!/usr/bin/env python3
"""046 (14.11.2026): envyū-Startbild im Emulator (unicorn + FT81x-Modell eve_emu.py).

Ausgefuehrt wird der ECHTE Startweg: Gfx_init 0x3FF8 → b.w 0x405C → Weiche → bs_boot → Original FUN_00003e8c (flag 0)
→ 26 Animationsbilder. Geprueft:
  A) Bildzahl, Endbild (Pads des Schriftzugs hell, uebrige dunkel, ū-Strich hellster Wert), Fusszeile sichtbar,
     Display-Liste je Bild klein genug (RAM_DL 8 KB, Befehls-FIFO 4 KB), Animation laeuft (Zwischenbilder verschieden).
  B) Sicherheitsnetz: Merker 0xA5A5A5A5 → genau das Original-Startbild (pixelgleich mit dem Original-Image).
  C) Fixpunkte der Pad-Funktion (bs_pad_value im Maschinencode): t = 0 alles aus, t = 1000 Wort an, Funken klingen ab.
Bilder: screens/00b_boot_envyu.png (Endbild), 00c_boot_frames.png (Bilder 0, 5, 10, 15, 20, 25), 00d_boot_anim.gif.
usage: emu_boot.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin
"""
import re, sys, os
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
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
MACRON = 0x70000
PAD, PITCH, GX, GY = 11, 14, 21, 46
def center(r, c): return GY + r * PITCH + PAD // 2, GX + c * PITCH + PAD // 2


def run(img, patched, nm, bad=False):
    orig_rig = ED.Rig
    if bad:
        def rig_bad(*a, **k):
            r = orig_rig(*a, **k); r.e.w32(0x1FFF9A38, 0xA5A5A5A5); return r
        ED.Rig = rig_bad
    try:
        frames = []; dls = []
        orig_render = ED.Eve.render
        def rec(self):
            out = orig_render(self); frames.append(out.copy()); dls.append(len(self.last_dl)); return out
        ED.Eve.render = rec
        orig_call6 = ED.Disp.call6; mark = {}
        def call6(self, addr, *a, **k):
            r = orig_call6(self, addr, *a, **k)
            if addr == 0x3FF8: mark["n"] = len(frames)      # Bilder bis zum Ende von Gfx_init (danach baut Disp die Seite)
            return r
        ED.Disp.call6 = call6
        try: d = ED.Disp(img, patched, nm)
        finally: ED.Eve.render = orig_render; ED.Disp.call6 = orig_call6
        n_boot = mark["n"]
        return d, frames[:n_boot], dls[:n_boot]
    finally:
        ED.Rig = orig_rig


def main():
    ppf, orig = sys.argv[1], sys.argv[2]
    nm = syms(); img = load_ppf(ppf); oimg = bytearray(open(orig, "rb").read())
    os.makedirs(ED.SCREENS, exist_ok=True)

    do, fo, _ = run(oimg, False, {})
    n0 = len(fo)                                   # Original: Bilder in Gfx_init (EVE-Init + Startbild)
    print("A) Startweg im gepatchten Image")
    d, fr, dls = run(img, True, nm)
    fr = fr[n0 - 1:]                               # ab dem Bild, das im Original das Startbild waere
    print(f"   {len(fr)} Bilder bis Ende Gfx_init, Display-Liste je Bild {min(dls)}…{max(dls)} Worte")
    check(len(fr) == 27, f"Original-Bilder bis auf das Startbild + leeres Bild (flag 0) + 26 Animationsbilder ({len(fr)} ab Startbild-Platz)")
    check(max(dls) * 4 < 4000, f"groesste Display-Liste {max(dls) * 4} Byte < 4 KB (FIFO) bzw. 8 KB (RAM_DL)")
    last = fr[-1].astype(int).max(axis=2)
    save_png(fr[-1], os.path.join(ED.SCREENS, "00b_boot_envyu.png"))
    word = [(r, c) for r in range(8) for c in range(20) if (ROWS[r] >> c) & 1]
    mac = [(0, c) for c in range(20) if (MACRON >> c) & 1]
    rest = [(r, c) for r in range(8) for c in range(20) if (r, c) not in word and (r, c) not in mac]
    vw = [last[center(r, c)] for r, c in word]; vm = [last[center(r, c)] for r, c in mac]; vr = [last[center(r, c)] for r, c in rest]
    check(len(word) == 52 and min(vw) >= 200 and max(vw) <= 230, f"Endbild: 52 Wort-Pads hell ({min(vw)}…{max(vw)})")
    check(min(vm) >= 250, f"Endbild: ū-Strich (3 Pads) am hellsten ({vm})")
    check(all(20 <= v <= 32 for v in vr), f"Endbild: 96 uebrige Pads dunkel, keine Funken mehr ({min(vr)}…{max(vr)})")
    check(int(last[0:40].max()) == 0 and int(last[162:190].max()) == 0, "Endbild: ueber und unter dem Raster schwarz")
    l1 = last[196:218, 10:200]; l2 = last[222:238, 10:200]; hs = last[222:238, 250:312]
    check(int((l1 > 150).sum()) > 150, f"Fusszeile 1 (envyu Version, Font 8) sichtbar ({int((l1 > 150).sum())} Px > 150)")
    check(int((l2 > 30).sum()) > 60 and int(l2.max()) <= 100, f"Fusszeile 2 (based on Play 1.6.0) gedimmt (max {int(l2.max())})")
    check(int((hs > 20).sum()) > 20 and int(hs.max()) <= 75, f"Build-Kennung rechts, sehr dezent (max {int(hs.max())})")
    txt = [c for c in d.eve.cmd_log if c[0] == 0x0C]
    t_last = [c[5] for c in txt[-3:]]
    build = "%08x" % int.from_bytes(bytes(img[nm["SN_HDR"] + 8:nm["SN_HDR"] + 12]), "little")
    VER = re.search(r'ENVYU_VERSION "([^"]+)"', open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "version.h")).read()).group(1)   # 049: aus version.h
    check(t_last == ["envyu " + VER, "based on Play 1.6.0", "#" + build[:4]], f"Texte {t_last} (Build {build})")
    mac_px = last[204:206, 47:54]; gap = last[206, 45:56]
    check(int((mac_px > 150).sum()) == 14 and int(gap.max()) < 60 and int(last[196:204, 40:60].max()) < 60,
          "ū-Strich in der Fusszeile: 7×2 Px direkt ueber dem u (Zeile 204/205, Abstand 1 Zeile)")
    mid = [f.astype(int).max(axis=2) for f in fr[1:]]
    lit = [sum(1 for r, c in word if m[center(r, c)] > 120) for m in mid]
    sparks = [sum(1 for r, c in rest if m[center(r, c)] > 40) for m in mid]
    print(f"   helle Wort-Pads je Bild: {lit}")
    print(f"   Funken je Bild:          {sparks}")
    check(lit[0] == 0 and all(a <= b + 2 for a, b in zip(lit, lit[1:])) and 10 < lit[8] < 45 and lit[-1] == 52,
          "Wort-Pads tauchen nach und nach auf (zufaellige Reihenfolge, am Ende alle)")
    check(max(sparks) >= 4 and sparks[-1] == 0, "Funken auf leeren Pads, am Ende verschwunden")
    check(len({f.tobytes() for f in fr[1:]}) >= 24, "Animation: Zwischenbilder verschieden")
    strip = np.concatenate([fr[1 + k] for k in (0, 5, 10, 15, 20, 25)], axis=1)
    save_png(strip, os.path.join(ED.SCREENS, "00c_boot_frames.png"))
    try:
        from PIL import Image
        ims = [Image.fromarray(f).resize((640, 480), Image.NEAREST) for f in fr[1:]]
        ims[0].save(os.path.join(ED.SCREENS, "00d_boot_anim.gif"), save_all=True, append_images=ims[1:],
                    duration=[30] * 25 + [1500], loop=0)
    except Exception as ex: print("   (GIF nicht geschrieben:", ex, ")")

    print("B) Sicherheitsnetz: Merker BAD → Original-Startbild")
    db, fb, _ = run(img, True, nm, bad=True)
    check(len(fb) == n0 and np.array_equal(fb[-1], fo[-1]), f"gepatchtes Image mit Merker BAD: {len(fb)} Bilder wie Original ({n0}), Startbild pixelgleich mit Original")

    print("C) Pad-Funktion bs_pad_value im Maschinencode")
    e = d.e; f = nm["bs_pad_value"] | 1
    v = lambda r, c, t: e.call(f, r, c, t & 0xFFFFFFFF) - (1 << 32 if e.call(f, r, c, t & 0xFFFFFFFF) >= 1 << 31 else 0)
    check(all(v(r, c, 0) <= 40 for r in range(8) for c in range(20)) and all(v(r, c, 0) == 26 for r, c in word + mac),
          "t = 0: Wort und Strich aus")
    check(all(v(r, c, 1000) == 215 for r, c in word) and all(v(r, c, 1000) == 255 for r, c in mac)
          and all(v(r, c, 1000) == 26 for r, c in rest), "t = 1000: Wort 215, Strich 255, Rest 26")
    check(all(26 <= v(r, c, t) <= 255 for r in range(8) for c in range(20) for t in range(0, 1001, 40)), "Werte immer 26…255")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
