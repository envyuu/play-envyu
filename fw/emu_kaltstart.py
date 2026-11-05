#!/usr/bin/env python3
"""Cold start: SRAM is undefined after power-on. mod_init must run cleanly with any RAM content."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from emu_nis import Emu, load_ppf
LR = 0x1FFF9EB0
LR_MAGIC = 0x11BEC023
def syms():
    return {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(HERE, "fx.elf")) if len(l.split()) == 3}
def run(img, nm, fill, lo=0x1FFF8000, hi=0x20000000):
    e = Emu(img)
    e.uc.mem_write(lo, bytes([fill]) * (hi - lo))
    try:
        e.call(nm['mod_init'] | 1, limit=20000000)
        e.call(nm['lr_poll'] | 1, limit=2000000)
    except Exception as ex:
        return False, str(ex)[:90]
    magic = e.r32(LR)
    return magic == LR_MAGIC, f'mod_init + lr_poll, LR magic {magic:#x}'
def main():
    nm = syms(); img = load_ppf(sys.argv[1]); bad = 0
    for fill in (0x00, 0xFF, 0xA5, 0x5A, 0x20, 0x1F):
        ok, msg = run(img, nm, fill)
        print(f"  {'ok    ' if ok else 'FEHLER'} RAM mit {fill:#04x} gefuellt: {msg}")
        bad += not ok
    print(f'{6 - bad}/6 Startzustaende ok'); sys.exit(1 if bad else 0)
if __name__ == '__main__': main()
