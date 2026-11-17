#!/usr/bin/env python3
import sys, struct, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from klick_mess import render
from emu_slicer import Rig, syms, load_ppf
def run(ppf):
    nm = syms(); r = Rig(load_ppf(ppf), True, nm); e = r.e
    L = 88200
    r.set_sample(L, gen=[20000] * L); r.set_mode(r.track, 4)
    out_all = {}
    for dec in (0, 500, 1000, 3000, 10000):
        r.trigger({9: 0, 6: 60, 10: 3 * 64, 11: 0x7FFF, 13: 0, 14: dec})
        x = [s for b in render(r, 48) for s in b]
        d = [(abs(x[i + 1] - x[i]), i) for i in range(len(x) - 1)]
        top = sorted(d, reverse=True)[:6]
        nz = max((i for i, s in enumerate(x) if s), default=0)
        print(f"Decay {dec/100:5.1f} %: Pegel voll {max(abs(s) for s in x)}, Ende bei {nz}, Sample davor {x[max(0,nz-3):nz+2]}; groesste Spruenge (Hoehe, Stelle): {top}")
        out_all[dec] = (x, top)
    # Start des Slices
    r.trigger({9: 0, 6: 60, 10: 3 * 64, 11: 0x7FFF, 13: 0, 14: 0})
    x = [s for b in render(r, 3) for s in b]; print("Start (Attack 0): ", x[:12])
    r.trigger({9: 0, 6: 60, 10: 3 * 64, 11: 0x7FFF, 13: 300, 14: 0})
    x = [s for b in render(r, 3) for s in b]; print("Start (Attack 3 %):", x[:12])
    # Neuer Step, waehrend der alte klingt
    r.trigger({9: 0, 6: 60, 10: 3 * 64, 11: 0x7FFF, 13: 0, 14: 0}); render(r, 10)
    r.trigger({9: 0, 6: 60, 10: 5 * 64, 11: 0x7FFF, 13: 0, 14: 0}); x = [s for b in render(r, 2) for s in b]
    print("Neustart mitten im Ton:", x[:8], "... groesster Sprung", max(abs(x[i+1]-x[i]) for i in range(len(x)-1)))
    return out_all
if __name__ == "__main__": run(sys.argv[1])
