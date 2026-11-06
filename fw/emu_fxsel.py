#!/usr/bin/env python3
"""Spurauswahl der FX-Suite im Cortex-M4-Emulator auf dem gebauten Image.

Echter Code: Pattern_ctor, Step-Setter, MidiTrack_triggerStep, resolveEvent, getParam, playChord, sendNote,
Original-Parametertabelle mit arp_init, die gepatchte vtable 0xAA324 und 0x9AD40, arp_chord_hook, usb_send_hook, tick_hook.
Attrappen: USB-Ausgabe 0x5D34 (zeichnet auf), Sequencer-Tick 0x4B21C, Note-Off-Warteschlange 0x4B6E4 und 0x4B0C0.
usage: emu_fxsel.py PolyendPlay_v1.6.0_mod.ppf
"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unicorn.arm_const import UC_ARM_REG_R1  # noqa: E402
from emu_nis import Emu, load_ppf  # noqa: E402

fails = 0
def check(c, m):
    global fails
    print(("  ok    " if c else "  FEHLER ") + m)
    if not c: fails += 1

def syms():
    out = subprocess.run(["arm-none-eabi-nm", os.path.join(HERE, "fx.elf")], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}

A, P = 0x200025D0, 0x2000E060
SEL, PAT = 0x1FFF4518, 0x1FFF4568
X = 0x2001F000
DELAY, LOOP, CCM = 0x000A86B4, 0x000A8434, 0x000A8E34
OUT = 0x2001F400           # Ausgabeobjekt, vtable = echte 0x9AD40 (gepatcht auf usb_send_hook)
EV = 0x2001F500

e = Emu(load_ppf(sys.argv[1])); S = syms()
sent = []
def usb(em):
    p = em.uc.reg_read(UC_ARM_REG_R1)
    t = em.r32(p); b = bytes(em.uc.mem_read(p + 4, 4))
    sent.append((t, b[0], b[2], b[3]))
e.stub(0x5D34, cb=usb); e.stub(0x4B21C); e.stub(0x4B6E4); e.stub(0x4B0C0); e.stub(0x4B330)

# g_stepParamOffset: wird beim Booten im RAM aufgebaut (FUN_00048138), Werte per PEEK vom laufenden Geraet gelesen
e.uc.mem_write(0x20019DC4, bytes([0x00, 0x02, 0x04, 0x06, 0x1A, 0x08, 0x1B, 0x0A, 0x0C, 0x18, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x1C, 0x1D, 0x19, 0x1E, 0x1F, 0x20, 0x21]))
e.call(0x497F4, P); e.call(0x497F4, A)
check(e.r32(P) == 0xAA500 and e.r32(P + 0x5D50) == 0xA9D04, "echter Pattern_ctor: vtables Pattern und MIDI-Spur")
e.w32(PAT, P); e.w32(SEL, 0)
e.w32(0x1FFF69B8, X)
for k in range(16): e.w32(X + 0x64 + 4 * k, 0)
e.w32(OUT, 0x9AD40)
e.call(S["arp_init"]) if "arp_init" in S else None

def track(t, base=P): return base + 0x5D50 + 0xBA8 * t
def step(t, i, base=P): return track(t, base) + 0x128 + 42 * i
def setup(t, ch, base=P):
    tr = track(t, base); e.w32(tr + 0x88 + 0x10, OUT)
    s = step(t, 0, base); vt = e.r32(s)
    e.call(e.r32(vt + 8), s, 1); e.call(e.r32(vt + 0x10), s, 6, 60); e.call(e.r32(vt + 0x10), s, 9, 16 + ch); e.call(e.r32(vt + 0x10), s, 0, 100)
    return s
def fire(t, base=P):
    e.uc.mem_write(EV, b"\0" * 0x40)
    e.call(0x4904C, step(t, 0, base), track(t, base) + 0x88, EV)
def ticks(n):
    for _ in range(n): e.call(S["tick_hook"], 0, 1)
def ons(ch): return sum(1 for t, c, a, b in sent if t == 4 and b and c == ch)
def ccs(ch): return sum(1 for t, c, a, b in sent if t == 6 and c == ch)
def clear():
    for k in range(16): e.w32(X + 0x64 + 4 * k, 0)
    e.w32(SEL, 0); ticks(600); sent.clear()

setup(0, 0); setup(3, 3)
ticks(1)
print("Notenweg ohne Effekt")
fire(0); fire(3)
check(ons(0) == 1 and ons(3) == 1, f"echte Kette sendet je eine Note ({ons(0)}/{ons(3)})")
check(any(a == 60 for t, c, a, b in sent if t == 4), "Note 60 aus dem Step")
ticks(1); sent.clear()

print("Echo")
e.w32(X + 0x64 + 4 * 13, DELAY + 20)
fire(0); fire(3); ticks(300)
check(ons(0) == 4 and ons(3) == 4, f"nichts gewaehlt: Echos auf beiden Spuren ({ons(0)}/{ons(3)})")
clear()
e.w32(X + 0x64 + 4 * 13, DELAY + 20); e.w32(SEL, 1 << (8 + 3))
fire(0); fire(3); ticks(300)
check(ons(0) == 1 and ons(3) == 4, f"Spur 3 gewaehlt: Spur 0 ohne, Spur 3 mit Echos ({ons(0)}/{ons(3)})")
clear()
e.w32(X + 0x64 + 4 * 13, DELAY + 20); e.w32(SEL, 1 << 8)
fire(0); fire(3); ticks(300)
check(ons(0) == 4 and ons(3) == 1, f"Spur 0 gewaehlt: umgekehrt ({ons(0)}/{ons(3)})")
clear()
e.w32(X + 0x64 + 4 * 13, DELAY + 20); e.w32(SEL, 0x00FF)
fire(0); ticks(300)
check(ons(0) == 4, f"nur Audio-Bits gesetzt: MIDI wie ohne Auswahl ({ons(0)})")
clear()

print("Zweites Pattern-Objekt")
setup(3, 3, A)
e.w32(X + 0x64 + 4 * 13, DELAY + 20); e.w32(SEL, 1 << (8 + 3))
fire(3, A); ticks(300)
check(ons(3) == 4, f"Spur in Pattern A wird erkannt ({ons(3)})")
clear()

print("Loop")
e.w32(SEL, 1 << (8 + 3))
fire(0); fire(3); ticks(20); sent.clear()
e.w32(X + 0x64 + 4 * 7, LOOP + 20 * 3); ticks(4); sent.clear()
fire(0); fire(3); 
check(ons(0) == 1 and ons(3) == 0, f"gewaehlte Spur stumm, andere spielt ({ons(0)}/{ons(3)})")
sent.clear(); ticks(96)
check(ons(3) == 4 and ons(0) == 0, f"Loop 1 Step wiederholt nur die gewaehlte Spur ({ons(0)}/{ons(3)})")
clear()

print("CC Motion")
e.w32(SEL, 1 << (8 + 3)); fire(0); fire(3); ticks(2); sent.clear()
e.w32(X + 0x64 + 4 * 14, CCM + 20); ticks(96)
check(ccs(3) > 10 and ccs(0) == 0, f"nur Kanal der gewaehlten Spur ({ccs(0)}/{ccs(3)})")
clear()

print("Arp")
arp_step = step(0, 0); arp_vt = e.r32(arp_step)
e.call(e.r32(arp_vt + 0x10), arp_step, 11, 1)          # Up 1/16
ar3 = step(3, 0); e.call(e.r32(e.r32(ar3) + 0x10), ar3, 11, 1)
got = []
fire(0); fire(3)
check(ons(0) == 1 and ons(3) == 1, f"Arp startet ueber den echten Weg ({ons(0)}/{ons(3)})")
ticks(1); clear()
e.w32(X + 0x64 + 4 * 13, DELAY + 20); e.w32(SEL, 1 << (8 + 3))
fire(0); fire(3); ticks(100)
n0, n3 = ons(0), ons(3)
check(n3 > n0, f"Arp-Noten: nur die gewaehlte Spur bekommt Echos ({n0}/{n3})")
del got
clear()
e.call(e.r32(arp_vt + 0x10), arp_step, 11, 0); e.call(e.r32(e.r32(ar3) + 0x10), ar3, 11, 0)

arp_ok = e.r32(0xAA324) & ~1 == S["arp_chord_hook"]
check(arp_ok, "vtable 0xAA324 zeigt auf arp_chord_hook")
print(f"GESAMT: {'FEHLER' if fails else 'alles OK'} ({fails} Fehler)")
sys.exit(1 if fails else 0)
