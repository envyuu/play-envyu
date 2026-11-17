#!/usr/bin/env python3
"""Patch 043: Kaltstart: SRAM ist nach dem Einschalten nicht definiert. sl_init muss mit beliebigem RAM-Inhalt sauber laufen."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from emu_slicer import syms, load_ppf
from emu_nis import Emu
def run(img, nm, fill, lo=0x1FFF8000, hi=0x20000000):
    e = Emu(img)
    e.uc.mem_write(lo, bytes([fill]) * (hi - lo))
    try:
        e.call(nm['sl_init'] | 1, limit=20000000)
    except Exception as ex:
        return False, str(ex)[:90]
    g = e.call(nm['gr_state'] | 1, 1); ok = 0x1FFF0000 <= g < 0x20040000
    c = e.call(nm['gr_cfg'] | 1, 1)
    pl = e.r32(0x1FFF9A04)
    return ok and e.uc.mem_read(c, 1)[0] == 4 and pl == 0, f'gr_state {g:#x}, Sequencer-Merker {pl}'
def main():
    nm = syms(); img = load_ppf(sys.argv[1]); bad = 0
    for fill in (0x00, 0xFF, 0xA5, 0x5A, 0x20, 0x1F):
        ok, msg = run(img, nm, fill)
        print(f"  {'ok    ' if ok else 'FEHLER'} RAM mit {fill:#04x} gefuellt: {msg}")
        bad += not ok
    print(f'{6 - bad}/6 Startzustaende ok'); sys.exit(1 if bad else 0)
if __name__ == '__main__': main()
