#!/usr/bin/env python3
"""045: Sequencer-Stopp auf einer Slicer-Spur: der Slice blendet in wenigen Audio-Bloecken aus, ohne Sprung (echter Render-Pfad)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from klick_mess import render
from emu_slicer import Rig, syms, load_ppf
def main(ppf):
    nm = syms(); r = Rig(load_ppf(ppf), True, nm)
    fails = 0; n = 0
    L = 441000; sig = 20000
    r.set_sample(L, gen=[sig] * L); r.set_mode(r.track, 1)          # 4 Slices a 2,5 s
    for note in (36, 48, 60, 72, 84):
        for dec in (0, 3000, 8000):
            for pre in (3, 40):
                r.trigger({9: 0, 6: note, 10: 1 * 64, 11: 0x7FFF, 13: 0, 14: dec})
                a = [s for b in render(r, pre) for s in b]
                lvl = max(abs(s) for s in a[-128:]) or 1
                r.e.call(nm["sl_transport"] | 1, 0)
                b = [s for blk in render(r, 12) for s in blk]
                x = a[-128:] + b
                last = max((i for i, s in enumerate(b) if s), default=-1)
                jump = max(abs(x[i + 1] - x[i]) for i in range(len(x) - 1)) / lvl
                ok = last < 6 * 128 and jump < 0.08
                n += 1
                if not ok: fails += 1; print("   FEHLER note", note, "decay", dec, "vorher", pre, "Bloecke, still ab Sample", last, "Sprung", round(jump, 3))
    r.e.call(nm["sl_transport"] | 1, 1)
    print(f"Stop blendet aus: {n - fails}/{n} Faelle (still nach <= 6 Bloecken, Sprung < 8 %)")
    return fails
if __name__ == "__main__":
    sys.exit(1 if main(sys.argv[1]) else 0)
