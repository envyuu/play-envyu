#!/usr/bin/env python3
"""Messung: Ende eines Slices im echten Render-Pfad. Spitze des letzten Tons, Sprung am Ende, Stufen der Huellkurve."""
import sys, struct, os, math
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from emu_slicer import Rig, syms, load_ppf, DATA
def render(rig, blocks):
    e = rig.e; out = []
    def tx(uc, a, s, ud):
        blk = uc.reg_read(UC_ARM_REG_R1)
        ch = struct.unpack("<i", uc.mem_read(blk + 4, 4))[0]   # gestoppte Stimme: leerer Block (0 Kanaele), Inhalt ist alter Speicher
        out.append(struct.unpack("<128h", uc.mem_read(blk + 8, 256)) if ch > 0 else (0,) * 128)
    h1 = e.uc.hook_add(UC_HOOK_CODE, tx, begin=0x17294, end=0x17295)
    fn = e.r32(e.r32(rig.stream)) | 1
    for b in range(blocks): e.call(fn & ~1, rig.stream)
    e.uc.hook_del(h1); return out
def run(ppf, verbose=True):
    nm = syms(); img = load_ppf(ppf); r = Rig(img, True, nm); e = r.e
    L = 88200
    r.set_sample(L, gen=[int(20000 * math.sin(i * 0.06)) for i in range(L)])
    r.set_mode(r.track, 4)                                  # 16 Slices: je 5512 Frames = 43 Bloecke
    res = {}
    for dec in (0, 500, 1000, 3000, 6000, 10000):
        r.trigger({9: 0, 6: 60, 10: 3 * 64, 11: 0x7FFF, 13: 0, 14: dec})
        out = render(r, 48)
        x = [s for b in out for s in b]
        last = max((i for i, s in enumerate(x) if s), default=0)
        d = [abs(x[i + 1] - x[i]) for i in range(len(x) - 1)]
        base = 20000 * 0.06 * 1.1                            # groesste Steigung des Sinus bei vollem Pegel
        endjump = max(abs(x[i]) for i in range(max(0, last - 2), min(len(x), last + 1)))
        pk = [max(abs(s) for s in b) for b in out]
        pkm = max(pk) or 1
        steps = [abs(pk[i + 1] - pk[i]) / pkm for i in range(len(pk) - 1)]
        res[dec] = (endjump, max(d), max(steps))
        if verbose:
            print(f"Decay {dec / 100:5.1f} %: letztes Sample {last:5d}, Wert am Ende {endjump:6d}, groesster Sprung {max(d):6d}, Pegel je Block (Ende): {pk[-14:]}")
    return res
if __name__ == "__main__": run(sys.argv[1])
