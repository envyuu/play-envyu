#!/usr/bin/env python3
"""Matrix: Sprung am Slice-Ende relativ zum Pegel (konstantes Signal, alles Sichtbare ist Huellkurve). Rueckgabe je Fall."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from klick_mess import render
from emu_slicer import Rig, syms, load_ppf
def run(ppf, verbose=True, sig=20000):
    nm = syms(); r = Rig(load_ppf(ppf), True, nm)
    res = []
    for L, m, name in ((88200, 4, "16 Slices 2 s"), (88200, 8, "64 Slices 2 s"), (20000, 8, "64 Slices 0,45 s")):
        r.set_sample(L, gen=[sig] * L); r.set_mode(r.track, m)
        for note in (36, 48, 60, 67, 72, 84, 96):
            for dec in (0, 1000, 3000, 8000):
                r.trigger({9: 0, 6: note, 10: 3 * 64, 11: 0x7FFF, 13: 0, 14: dec})
                x = [s for b in render(r, 64 if note >= 60 else 200) for s in b]
                full = max(abs(s) for s in x) or 1
                last = max((i for i, s in enumerate(x) if s), default=0)
                # Sprung im Ausklang = Abfall je Sample in der zweiten Haelfte
                tail = [abs(x[i + 1] - x[i]) for i in range(last // 2, min(len(x) - 1, last + 2))]
                j = max(tail) / full if tail else 0
                adv = 128 * 2 ** ((note - 60) / 12); short = (L // {4: 16, 8: 64}[m]) < 2 * adv   # Slice kuerzer als 2 Audio-Bloecke: keine Luft
                res.append((name, note, dec, round(j, 3), full, last, short))
    bad = [q for q in res if q[3] > 0.08 and not q[6]]
    if verbose:
        print("Faelle", len(res), "mit Sprung > 8 %:", len(bad))
        for q in sorted(bad, key=lambda q: -q[3])[:24]: print("  ", q)
    return res, bad
if __name__ == "__main__":
    res, bad = run(sys.argv[1], verbose=len(sys.argv) < 3)
    if len(sys.argv) > 2:
        print(f"Faelle {len(res)}, mit Knacken {len(bad)}"); sys.exit(1 if bad else 0)
