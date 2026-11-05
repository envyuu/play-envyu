#!/usr/bin/env python3
"""Maschinencode-Test von Patch 018 („Note In Scale“): fuehrt die ECHTEN Firmware-Funktionen des gepatchten
Rev-A-Images in einem Cortex-M4-Emulator (unicorn) aus – mit Attrappen nur fuer Randomizer-Umgebung, Pattern und
Display-Popup. Die MIDI-Steps benutzen die echte Step-vtable 0xAA2D0 (getParam/setParam/Grenzen/hasNote).

Geprueft:
  1. Randomize_initTypes 0x38E74 (Original-Image) baut die MIDI-Liste 0x1FFF2190 → Eintraege 0–3 == NIS_LIST 0–3.
     Im gepatchten Image laeuft der Konstruktor unveraendert und beschreibt nur die alte Liste.
  2. Vorschau FUN_00038c10 → FUN_00038a74 → FUN_000389a0 → nis_gen_entry: Ergebnis == Python-Modell
     (Seed-Hash, minstd, Prozent aus s16, Zustand aus r7), Note in der Scale, Anteil ≈ Prozent.
  3. Einbacken FUN_00038b00 (echte Selektion/Pattern-Schleife, setParam): gebackene Noten == Vorschau, danach 0 %.
  4. „1 Octave“ (Eintrag 1, jetzt aus der Flash-Liste) arbeitet wie im Original-Image (bitgleich).
  5. FUN_0003893c (Typ wechseln) begrenzt auf 0…5; FUN_00038d74 (Anzeige) zeigt „In D Dorian“ und meldet max. 5.
  6. Patch-Stellen enthalten genau die erwarteten Bytes, alles andere im Bereich 0x38698–0x38E74 ist unveraendert.

usage: emu_nis.py PolyendPlay_v1.6.0_noteinscale.ppf ../stock/firmware.bin
"""
import sys, struct, os, subprocess
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UcError, UC_HOOK_CODE
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from check_ppf import images  # noqa: E402

STOP = 0x000FFF00
HEAP = 0x20028000
R = 0x20020000          # Randomizer-Objekt (wie SeqController+0x160)
S = 0x20020100          # Selektions-/Editor-Objekt (+0x84 = MIDI-Spur, +4 = Liste der (Spur, Step)-Paare)
SVT = 0x20020200        # dessen vtable (+8 = „Step gewaehlt?“)
SEL = 0x20020400        # Paarliste, Anzahl bei +0x800
PAT = 0x20021000        # Pattern-Attrappe: vtable[0](pat, spur) → Spur, Spur vtable[0](spur, i) → Step i
CODE = 0x20021800       # kleine Thumb-Stuecke
STEPS = 0x20023000      # Steps zu je 0x40 Byte
OFFS = 0x20019DC4       # Offsets der 22 Parameter im Step (.bss, sonst zur Laufzeit gefuellt)
SCALE_T, SCALE_R = 0x1FFF70AC, 0x1FFF70B0
NIS_RAM = 0x1FFFB900
LIT = [0x38768, 0x38918, 0x38AF8, 0x38C04, 0x38D68, 0x38E64]

fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


def load_ppf(fn):
    im = images(fn)[0]
    img = bytearray(max(im) + 1)
    for a, v in im.items(): img[a] = v
    return img


class Emu:
    def __init__(self, img):
        self.img = img
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x0, 0x100000); uc.mem_write(0, bytes(img[:0x100000]))
        uc.mem_write(STOP, b"\xfe\xe7" * 8)
        uc.mem_map(0x1FFF0000, 0x10000); uc.mem_map(0x20000000, 0x30000)
        uc.mem_map(0xE0000000, 0x100000)
        uc.mem_write(0x1FFF1CA0, bytes(img[0xAEC30:0xAEC30 + 0x1A6C]))           # .data wie ResetHandler
        uc.mem_write(0x1FFFB000, bytes((i * 37 + 11) & 0xFF for i in range(0x1000)))   # SRAM_L-Muell
        self.uc = uc; self.heap = HEAP; self.sp = 0x2002F000
        uc.hook_add(UC_HOOK_CODE, self._heap, begin=0x56F5C, end=0x56F5D)       # Heap_alloc → Bump-Zeiger
        self.stubs = {}
    def _heap(self, uc, addr, size, ud):
        n = uc.reg_read(UC_ARM_REG_R0); p = self.heap; self.heap += (n + 7) & ~7
        uc.mem_write(p, b"\0" * n); uc.reg_write(UC_ARM_REG_R0, p)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    def stub(self, addr, ret=None, cb=None):
        """Firmware-Funktion durch 'return ret' ersetzen (optional Rueckruf mit den Registern)."""
        def h(uc, a, s, ud):
            if cb: cb(self)
            if ret is not None: uc.reg_write(UC_ARM_REG_R0, ret)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        self.uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr + 1)
    def call(self, addr, *args, s0=None, limit=3_000_000):
        uc = self.uc
        for i, a in enumerate(args): uc.reg_write(UC_ARM_REG_R0 + i, a & 0xFFFFFFFF)
        if s0 is not None: uc.reg_write(UC_ARM_REG_S0, s0)
        uc.reg_write(UC_ARM_REG_SP, self.sp); uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        try: uc.emu_start(addr | 1, STOP, count=limit)
        except UcError as e: raise RuntimeError(f"Emulation abgebrochen bei pc=0x{uc.reg_read(UC_ARM_REG_PC):x}: {e}")
        if uc.reg_read(UC_ARM_REG_PC) != STOP: raise RuntimeError("nicht zurueckgekehrt")
        return uc.reg_read(UC_ARM_REG_R0)
    def r32(self, a): return struct.unpack("<I", self.uc.mem_read(a, 4))[0]
    def w32(self, a, v): self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    def wf(self, a, f): self.uc.mem_write(a, struct.pack("<f", f))
    def rf(self, a): return struct.unpack("<f", self.uc.mem_read(a, 4))[0]
    def cstr(self, a): return bytes(self.uc.mem_read(a, 40)).split(b"\0")[0].decode("latin1")


def fbits(f): return struct.unpack("<I", struct.pack("<f", f))[0]

def minstd(x):
    return x * 48271 % 0x7FFFFFFF

def uniform_int(state, lo, hi):
    """FUN_0003876c (std::uniform_int_distribution mit minstd_rand), Fall Bereich < 0x7FFFFFFD."""
    n = (hi - lo) & 0xFFFFFFFF
    assert n < 0x7FFFFFFD
    scale = 0x7FFFFFFD // (n + 1); lim = scale * (n + 1)
    while True:
        state = minstd(state); r = state - 1
        if r < lim: return state, r // scale + lo

def seed_of(seedk, step):
    b = step & 0xFF
    v = ((b << 16 | b << 8 | b | (step << 24)) & 0xFFFFFFFF) ^ seedk
    v %= 0x7FFFFFFF
    return v or 1

def model(seedk, step, orig, pct, mask, root, mode):
    """Python-Modell: FUN_00038a74 + FUN_000389a0 (Bereich ±12, MIDI-Minimum 12, Maximum 127) + nis_gen."""
    st = seed_of(seedk, step)
    lo, hi = -12, 12
    if int(orig + lo * pct) < 12: lo = 12 - orig
    if 127 < int(orig + hi * pct): hi = 127 - orig
    st, _ = uniform_int(st, lo, hi)
    p = min(65536, int(pct * 65536)) if pct < 1 else 65536
    x = st % 0x7FFFFFFF or 1
    x = minstd(x); u1 = ((x - 1) >> 15) & 0xFFFF
    x = minstd(x); u2 = x
    x = minstd(x); u3 = ((x - 1) >> 15) & 0xFFFF
    x = minstd(x); u4 = x
    if p == 0 or u1 >= p: return orig
    wl, wh = max(12, orig - 12), min(119, orig + 12)
    ins = [n for n in range(wl, wh + 1) if n != orig and (mask >> ((n - root) % 12)) & 1]
    outs = [n for n in range(wl, wh + 1) if n != orig and not (mask >> ((n - root) % 12)) & 1]
    if mode == 1 and p > 32768 and outs and u3 < (p - 32768) // 2: return outs[u4 % len(outs)]
    return ins[u2 % len(ins)] if ins else orig


def setup_env(e, nsteps, notes):
    e.uc.mem_write(CODE, bytes.fromhex("01207047"))                       # +0: movs r0,#1; bx lr
    e.w32(SVT + 8, CODE | 1)
    e.w32(S, SVT); e.w32(S + 4, SEL); e.uc.mem_write(S + 0x84, b"\x01")   # MIDI-Spur
    pairs = b"".join(bytes([0, i]) for i in range(nsteps))
    e.uc.mem_write(SEL, pairs); e.w32(SEL + 0x800, nsteps)
    # Pattern: vtable[0] → Spur-Objekt; Spur vtable[0](spur, i) → STEPS + i*0x40
    e.uc.mem_write(CODE + 0x10, bytes.fromhex("00487047") + struct.pack("<I", PAT + 0x40))   # ldr r0,[pc]; bx lr
    e.uc.mem_write(CODE + 0x20, bytes.fromhex("0148890108447047") + struct.pack("<I", STEPS))
    e.w32(PAT, PAT + 0x20); e.w32(PAT + 0x20, CODE + 0x10 | 1)
    e.w32(PAT + 0x40, PAT + 0x60); e.w32(PAT + 0x60, CODE + 0x20 | 1)
    e.uc.mem_write(OFFS, bytes(22)); e.uc.mem_write(OFFS + 6, bytes([12]))   # Note bei Step+4+12
    for i in range(nsteps):
        a = STEPS + 0x40 * i
        e.uc.mem_write(a, b"\0" * 0x40); e.w32(a, 0xAA2D0)                    # echte MIDI-Step-vtable
        e.uc.mem_write(a + 0x26, bytes([1 << 6, 0, 0, 0]))                    # Note gesetzt
        e.uc.mem_write(a + 4 + 12, bytes([notes[i]]))
    e.w32(R + 4, S); e.w32(R + 8, 0); e.uc.mem_write(R + 0x38, b"\0")


def main():
    ppf, fwfile = sys.argv[1], sys.argv[2]
    img = load_ppf(ppf); orig = bytearray(open(fwfile, "rb").read())
    nm = {}
    elf = os.path.join(HERE, "fx.elf")
    for l in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = l.split()
        if len(p) == 3: nm[p[2]] = int(p[0], 16)
    NL = nm["NIS_LIST"]

    print("== 6. Patch-Stellen im Bereich 0x38698–0x38E74")
    diff = [a for a in range(0x38698, 0x38E74) if img[a] != orig[a]]
    want = set()
    for a in LIT: want |= set(range(a, a + 4))
    want |= {0x3894E, 0x38DE6} | set(range(0x38DC0, 0x38DC4))
    check(set(diff) <= want, f"nur erwartete Bytes geaendert ({len(diff)} Byte)")
    check(all(struct.unpack_from("<I", img, a)[0] == NL for a in LIT), "6 Literale → NIS_LIST")
    check(img[0x3894E] == 5 and img[0x38DE6] == 5, "Grenzen 3 → 5")
    check(struct.unpack_from("<I", img, 0x3921C)[0] == 0x1FFF2190, "Literal im Konstruktor unveraendert")

    print("== 1. Randomize_initTypes: Original-Liste == NIS_LIST 0–3")
    eo = Emu(orig); eo.call(0x38E74)
    ep = Emu(img); ep.call(0x38E74)
    same = bytes(eo.uc.mem_read(0x1FFF1CA0, 0x1A6C)) == bytes(ep.uc.mem_read(0x1FFF1CA0, 0x1A6C))
    check(same, "Konstruktor schreibt im gepatchten Image dieselben .data-Inhalte")
    ok = True
    for e_ in range(4):
        ra, fa = 0x1FFF2190 + 100 * e_, NL + 100 * e_
        rw = struct.unpack("<25I", eo.uc.mem_read(ra, 100)); fw = struct.unpack_from("<25I", img, fa)
        if rw[0] != fw[0]: ok = False; print("    Name", e_, hex(rw[0]), hex(fw[0]))
        for k in range(8):
            ri, rr, rg = rw[1 + 3 * k:4 + 3 * k]; fi, fr, fg = fw[1 + 3 * k:4 + 3 * k]
            if (ri, rr) != (fi, fr): ok = False; print("    Eintrag", e_, k, (hex(ri), hex(rr)), (hex(fi), hex(fr)))
            if (rg == 0) != (fg == 0): ok = False; print("    Generator", e_, k)
            elif rg:
                if eo.r32(rg) != struct.unpack_from("<I", img, fg)[0]: ok = False; print("    vtable", e_, k)
    check(ok, "Name, Index, Bereich, Generator-vtable aller 4×8 Tripel gleich")
    names = [eo.cstr(eo.r32(0x1FFF2190 + 100 * i)) for i in range(4)]
    check(names == ["Filter CC", "1 Octave", "2 Octaves", "Velocity"], f"Namen {names}")

    print("== 2./3. Vorschau und Einbacken mit echtem Firmware-Code")
    e = ep
    e.stub(0x8598); e.stub(0x859C)                                         # IRQ an/aus (Teensy) – egal
    N = 64
    for (st, rt, label) in [(3, 2, "D Dorian"), (17, 9, "A Penta Min"), (0, 0, "Chromatic → C Major")]:
        e.w32(SCALE_T, st); e.w32(SCALE_R, rt)
        mask = struct.unpack_from("<H", orig, 0xAC838 + 2 * st)[0] & 0xFFF
        if st == 0: mask = 0xAB5
        for typ, mode in [(4, 0), (5, 1)]:
            for pct in [0.0, 0.3, 0.75, 1.0]:
                notes = [36 + (i * 7) % 48 for i in range(N)]
                setup_env(e, N, notes)
                e.w32(R + 0xC, typ); e.wf(R + 0x30, pct)
                seeds = [(0x13579BDF * (i + 1) + typ * 977 + int(pct * 100)) & 0x7FFFFFFF for i in range(8)]
                for i, s in enumerate(seeds): e.w32(R + 0x10 + 4 * i, s)
                pv, mv = [], []
                for i in range(N):
                    step = STEPS + 0x40 * i
                    pv.append(e.call(0x38C10, R, step, 6, notes[i], 0) & 0xFFFF)
                    mv.append(model(seeds[0], step, notes[i], e.rf(R + 0x30), mask, rt, mode))
                pv = [v - 0x10000 if v & 0x8000 else v for v in pv]
                ch = sum(1 for a, b in zip(pv, notes) if a != b)
                ins = all((mask >> ((v - rt) % 12)) & 1 for v, o in zip(pv, notes) if v != o)
                okm = pv == mv
                tag = f"{label:20s} Typ {typ} {int(pct * 100):3d} %:"
                check(okm, f"{tag} Vorschau == Modell, {ch}/{N} geaendert" + ("" if okm else f" {pv[:8]} vs {mv[:8]}"))
                if mode == 0 or pct <= 0.5: check(ins, f"{tag} alle Aenderungen in der Scale")
                if pct in (0.0, 1.0): check(ch == (0 if pct == 0 else N), f"{tag} Anteil {ch}/{N}")
                # Einbacken
                e.call(0x38B00, R, PAT)
                baked = [struct.unpack("<b", e.uc.mem_read(STEPS + 0x40 * i + 16, 1))[0] for i in range(N)]
                check(baked == pv, f"{tag} Einbacken == Vorschau")
                check(e.rf(R + 0x30) == 0.0 and e.r32(R + 0x10) != seeds[0], f"{tag} danach 0 % und neuer Seed")
    calls = e.r32(NIS_RAM + 4)
    check(e.r32(NIS_RAM) == 0x4E495331 and calls > 0, f"Diagnose: {calls} Aufrufe, {e.r32(NIS_RAM + 8)} geaendert, {e.r32(NIS_RAM + 12)} ausserhalb")

    print("== 4. „1 Octave“ aus der Flash-Liste == Original-Image")
    eo.stub(0x8598); eo.stub(0x859C)
    for e_ in (eo, ep): e_.w32(SCALE_T, 3); e_.w32(SCALE_R, 2)
    res = []
    for e_ in (eo, ep):
        notes = [40 + i for i in range(32)]
        setup_env(e_, 32, notes); e_.w32(R + 0xC, 1); e_.wf(R + 0x30, 0.6)
        for i in range(8): e_.w32(R + 0x10 + 4 * i, 0x2468ACE + 77 * i)
        res.append([e_.call(0x38C10, R, STEPS + 0x40 * i, 6, notes[i], 0) for i in range(32)])
    check(res[0] == res[1], "Vorschau „1 Octave“ bitgleich (D Dorian, 60 %)")

    print("== 5. Typ wechseln und Anzeige")
    setup_env(e, 4, [60] * 4)
    e.w32(R + 0xC, 3); e.call(0x3893C, R, 5); check(e.r32(R + 0xC) == 5, f"3 + 5 → {e.r32(R + 0xC)} (max 5)")
    e.call(0x3893C, R, 1); check(e.r32(R + 0xC) == 5, "5 + 1 → 5")
    e.call(0x3893C, R, -9); check(e.r32(R + 0xC) == 0, "→ 0")
    eo_ = Emu(orig); eo_.stub(0x8598); eo_.stub(0x859C); setup_env(eo_, 4, [60] * 4)
    eo_.w32(R + 0xC, 3); eo_.call(0x3893C, R, 5); check(eo_.r32(R + 0xC) == 3, "Original: max 3 (Gegenprobe)")
    got = {}
    def cap(em):
        sp = em.uc.reg_read(UC_ARM_REG_SP)
        p = em.r32(sp); got["max"] = (em.r32(p), em.r32(p + 4))
    for a in (0x2F46C,): e.stub(a)
    e.stub(0x2F464, ret=1); e.stub(0x2F474, ret=0); e.stub(0x2F36C, cb=cap)
    e.stub(0x57DB4)                                                        # snprintf("%d%%") – nicht noetig
    for typ, st, rt, want in [(4, 3, 2, "In D Dorian"), (5, 3, 2, "In D Dorian+"), (4, 0, 7, "In G Major"),
                              (5, 8, 1, "In C# Phryg Dom+"), (5, 9, 6, "F# Mixolydian+"), (1, 3, 2, "1 Octave")]:
        e.w32(SCALE_T, st); e.w32(SCALE_R, rt); e.w32(R + 0xC, typ)
        e.call(0x38D74, R, 0x20020800)
        shown = e.cstr(0x20001F51)
        check(shown == want and got.get("max") == (100, 5), f"Typ {typ}: Anzeige '{shown}', Popup-Grenze {got.get('max')}")

    print(f"{'OK' if not fails else 'FEHLER'}: {checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
