#!/usr/bin/env python3
"""045 (11.11.2026): neue Randomize-Typen im Emulator mit ECHTEM Firmware-Code (Randomizer_modify 0x38C10,
Randomizer_valueFor, Randomize_applyTriple, echte MIDI-/Audio-Step-vtables 0xAA2D0/0xAA338, Step_initParamOffsets)
und Steps im echten Pattern-Layout (PAT0 0x200025D0 + 0x10 + t*0xBA8 + 0x128 + i*42).

Geprueft:
  1. Patch-Stellen: Literale MIDI/Audio-Liste, Grenzen 13/16, Umleitungen 0x38B60/0x38D5C/0x21B26.
  2. AUD_LIST 0–9 == Original-Audioliste aus Randomize_initTypes (Name, Index, Bereich, Generator-vtable + Modus).
  3. „1 Octave“, „Sample In Folder“ (Audio, aus der Flash-Liste) bitgleich mit dem Original-Image.
  4. Shuffle / Melodic Walk / Accent / Call & Response / Thin Out fuer MIDI und Audio (Vorschau), Humanize/Octave Jump Audio.
  5. Einbacken ueber rz_bake (zweistufig) == Vorschau; leere und nicht gewaehlte Steps bleiben.
  6. Typ wechseln: MIDI max 13, Audio max 16.

usage: emu_rand.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin
"""
import sys, struct, os, subprocess
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_LR

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import veneer as VN  # 13.11.: Weichen (Sicherheitsnetz) aufloesen
from emu_nis import Emu, load_ppf, R, S, SVT, SEL, CODE, SCALE_T, SCALE_R  # noqa: E402

PAT0 = 0x200025D0
SELX = 0x70346954           # 12.11.: wie am Geraet (SDRAM) – vorher RAM, deshalb blieb der Fehler unentdeckt
OFFS = 0x20019DC4
KIND = 0xAA114
PATOBJ = 0x20021C00          # Attrappe: vt[0](pat, t) → Spur, Spur vt[0](spur, i) → Step
fails = 0; checks = 0


def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


def sx(v): return v - 0x10000 if v & 0x8000 else v


class Env:
    def __init__(self, img, nm):
        self.e = e = Emu(img); self.nm = nm; self.img = img
        e.uc.mem_map(0x70340000, 0x10000)                                 # 12.11.: Auswahl-Paarliste liegt am Geraet im SDRAM (PEEK 0x70346954)
        e.stub(0x8598); e.stub(0x859C); e.stub(0x56F6C)                  # IRQ an/aus, free (Bump-Heap)
        e.call(0x49244)                                                  # Step_initParamOffsets → 0x20019DC4
        self.off = list(e.uc.mem_read(OFFS, 22))
        e.uc.mem_write(CODE, bytes.fromhex("01207047"))                  # Auswahl: „Step gewaehlt?“ = 1
        e.w32(SVT + 8, CODE | 1)
        # Pattern-/Spur-Attrappe per Python-Hook: Spur = PAT0+0x10+t*0xBA8, Step = Spur+0x128+i*42
        e.w32(PATOBJ, PATOBJ + 0x20); e.w32(PATOBJ + 0x20, (CODE + 0x40) | 1)
        e.w32(PATOBJ + 0x40, PATOBJ + 0x60); e.w32(PATOBJ + 0x60, (CODE + 0x50) | 1)
        def trk(uc, a, s, ud):
            t = uc.reg_read(UC_ARM_REG_R1); self.cur_t = t
            uc.reg_write(UC_ARM_REG_R0, PATOBJ + 0x40); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        def stp(uc, a, s, ud):
            i = uc.reg_read(UC_ARM_REG_R1)
            uc.reg_write(UC_ARM_REG_R0, self.addr(self.cur_t, i)); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        from unicorn import UC_HOOK_CODE
        e.uc.hook_add(UC_HOOK_CODE, trk, begin=CODE + 0x40, end=CODE + 0x41)
        e.uc.hook_add(UC_HOOK_CODE, stp, begin=CODE + 0x50, end=CODE + 0x51)
        self.cur_t = 0

    @staticmethod
    def addr(t, i): return PAT0 + 0x10 + t * 0xBA8 + 0x128 + i * 42

    def src(self, t, j):
        """12.11.: Vorschau der Step-Operationen (echtes rz_preview_src, Randomizer gespiegelt nach 0x1FFF4640)"""
        self.e.uc.mem_write(0x1FFF4640, bytes(self.e.uc.mem_read(R, 0x40))); self.e.w32(0x1FFF4640, 0xA59F8)
        r = self.e.call(self.nm["rz_preview_src"] | 1, PAT0 + 0x10 + t * 0xBA8, t, j)
        return r - (1 << 32) if r >= 1 << 31 else r

    def on(self, t, i): return bool(self.e.uc.mem_read(self.addr(t, i) + 0x29, 1)[0] & 0x80)

    def kind(self, idx): return self.img[KIND + 16 * idx]

    def setp(self, st, idx, v):
        e = self.e; o = st + 4 + self.off[idx]; k = self.kind(idx)
        if k == 3: e.uc.mem_write(o, struct.pack("<h", v))
        elif k == 2: e.uc.mem_write(o, bytes([v & 0xFF]))
        else: e.uc.mem_write(o, struct.pack("<b", v))
        f = int.from_bytes(e.uc.mem_read(st + 0x26, 4), "little") | (1 << idx)
        e.uc.mem_write(st + 0x26, f.to_bytes(4, "little"))

    def getp(self, st, idx):
        o = st + 4 + self.off[idx]; k = self.kind(idx); b = bytes(self.e.uc.mem_read(o, 2))
        return struct.unpack("<h", b)[0] if k == 3 else b[0] if k == 2 else struct.unpack("<b", b[:1])[0]

    def track(self, t, midi, steps, sel=None):
        """steps: Liste je Step: None = leer, sonst dict idx → Wert; sel: gewaehlte Step-Indizes (Standard: alle)"""
        e = self.e
        for i in range(64):
            a = self.addr(t, i)
            e.uc.mem_write(a, b"\0" * 42); e.w32(a, 0xAA2D0 if midi else 0xAA338)
            if i < len(steps) and steps[i] is not None:
                e.uc.mem_write(a + 0x29, bytes([0x80]))                    # Step an (Bit 7, vt[3] 0x480ED)
                for idx, v in steps[i].items(): self.setp(a, idx, v)
        e.w32(PAT0 + 0x10 + t * 0xBA8 + 0xC0, 16)                          # 12.11.: Spurlaenge (Spur+0xC0)
        sel = list(range(len(steps))) if sel is None else sel
        e.w32(S, SVT); e.w32(S + 4, SELX); e.uc.mem_write(S + 0x84, bytes([1 if midi else 0]))
        e.uc.mem_write(SELX, b"".join(bytes([t, i]) for i in sel)); e.w32(SELX + 0x800, len(sel))
        e.w32(R + 4, S); e.w32(R + 8, 0); e.uc.mem_write(R + 0x38, b"\0")

    def rnd(self, typ, pct, salt=0):
        e = self.e
        e.w32(R + 0xC, typ); e.wf(R + 0x30, pct)
        for i in range(8): e.w32(R + 0x10 + 4 * i, (0x1234567 * (i + 1) + typ * 7919 + salt * 31) & 0x7FFFFFFF)

    def prev(self, t, i, idx):
        a = self.addr(t, i)
        return sx(self.e.call(0x38C10, R, a, idx, self.getp(a, idx), 0) & 0xFFFF)

    def bake(self):
        self.e.call(self.nm["rz_bake"], R, PATOBJ)


def scale_ok(n, mask, root): return (mask >> ((n - root) % 12)) & 1


def main():
    ppf, fwfile = sys.argv[1], sys.argv[2]
    img = load_ppf(ppf); orig = bytearray(open(fwfile, "rb").read())
    nm = {}
    for l in subprocess.check_output(["arm-none-eabi-nm", os.path.join(HERE, "fx.elf")], text=True).splitlines():
        p = l.split()
        if len(p) == 3: nm[p[2]] = int(p[0], 16)
    AL, NL = nm["AUD_LIST"], nm["NIS_LIST"]

    print("== 1. Patch-Stellen")
    check(all(struct.unpack_from("<I", img, a)[0] == AL for a in (0x3891C, 0x38AFC, 0x38C0C, 0x38D64, 0x38E70)), "5 Literale → AUD_LIST")
    check(struct.unpack_from("<I", img, 0x391F0)[0] == 0x1FFF28FC, "Konstruktor-Literal unveraendert")
    check(img[0x3894E] == 14 and img[0x38DE6] == 14 and img[0x38950] == 17 and img[0x38DE8] == 17, "Grenzen MIDI 14, Audio 17")
    def bl_target(a):
        h1, h2 = struct.unpack_from("<HH", img, a)
        s_ = (h1 >> 10) & 1; j1 = (h2 >> 13) & 1; j2 = (h2 >> 11) & 1
        i1 = 1 - (j1 ^ s_); i2 = 1 - (j2 ^ s_)
        off = (s_ << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
        if s_: off -= 1 << 25
        return a + 4 + off
    check(VN.follow(img, bl_target(0x38B60)) == nm["rz_value_bake"] and VN.follow(img, bl_target(0x38D5C)) == nm["rz_value_prev"] and VN.follow(img, bl_target(0x21B26)) == nm["rz_bake"], "bl → rz_value_bake / rz_value_prev / rz_bake")
    diff = [a for a in range(0x38698, 0x38E74) if img[a] != orig[a]]
    want = set()
    for a in (0x38768, 0x38918, 0x38AF8, 0x38C04, 0x38D68, 0x38E64, 0x3891C, 0x38AFC, 0x38C0C, 0x38D64, 0x38E70, 0x38B60, 0x38D5C, 0x38DC0):
        want |= set(range(a, a + 4))
    want |= {0x3894E, 0x38DE6, 0x38950, 0x38DE8}
    check(set(diff) <= want, f"im Randomizer nur erwartete Bytes geaendert ({len(diff)})")

    print("== 2. AUD_LIST 0–9 == Original-Audioliste")
    eo = Emu(orig); eo.call(0x38E74, 0x20020000)
    ok = True
    for t in range(10):
        ra, fa = 0x1FFF28FC + 100 * t, AL + 100 * t
        rw = struct.unpack("<25I", eo.uc.mem_read(ra, 100)); fw = struct.unpack_from("<25I", img, fa)
        if rw[0] != fw[0]: ok = False; print("    Name", t)
        for k in range(8):
            ri, rr, rg = rw[1 + 3 * k:4 + 3 * k]; fi, fr, fg = fw[1 + 3 * k:4 + 3 * k]
            if (ri, rr) != (fi, fr): ok = False; print("    Tripel", t, k, hex(ri), hex(fi))
            if (rg == 0) != (fg == 0): ok = False; print("    Generator", t, k)
            elif rg and (eo.r32(rg), eo.r32(rg + 4)) != struct.unpack_from("<II", img, fg) and eo.r32(rg) != 0xA59BC:
                ok = False; print("    Generator-Objekt", t, k)
            elif rg and eo.r32(rg) != struct.unpack_from("<I", img, fg)[0]: ok = False; print("    vtable", t, k)
            if ri == 0xFFFFFFFF: break
    check(ok, "Name, Index, Bereich, Generator (vtable, Ordner/Pool) aller Tripel gleich")
    names = [bytes(img[struct.unpack_from("<I", img, AL + 100 * t)[0]:][:20]).split(b"\0")[0].decode() for t in range(18)]
    check(names[10:] == ["Humanize", "Octave Jump", "Swap Steps", "Scatter Steps", "Melodic Walk", "Accent", "Call & Response", "Thin Out"], f"neue Audio-Namen {names[10:]}")
    mn = [bytes(img[struct.unpack_from("<I", img, NL + 100 * t)[0]:][:20]).split(b"\0")[0].decode() if struct.unpack_from("<I", img, NL + 100 * t)[0] < 0x100000 else "(RAM)" for t in range(15)]
    check(mn[9:] == ["Swap Steps", "Scatter Steps", "Melodic Walk", "Accent", "Call & Response", "Thin Out"], f"neue MIDI-Namen {mn[9:]}")
    check(all(len(n) <= 16 for n in names + mn), "Namen hoechstens 16 Zeichen")

    print("== 3. Original-Audiotypen aus der Flash-Liste == Original-Image (Vorschau)")
    res = []
    for im in (orig, img):
        v = Env(im, nm)
        v.e.w32(SCALE_T, 3); v.e.w32(SCALE_R, 2)
        if im is orig:   # Original: Liste im RAM per Konstruktor
            v.e.call(0x38E74, 0x20020000)
        v.track(0, False, [{0: 5000, 6: 40 + i, 9: 10 + i, 10: 100 * i} for i in range(16)])
        out = []
        for typ in (0, 2, 4, 6, 9):
            v.rnd(typ, 0.6)
            out.append([v.prev(0, i, idx) for i in range(16) for idx in (0, 6, 9, 10)])
        res.append(out)
    check(res[0] == res[1], "Sample In Folder, Sample Length, 1 Octave, Volume, Duke Nuke bitgleich")

    v = Env(img, nm)
    v.e.w32(SCALE_T, 3); v.e.w32(SCALE_R, 2)                                  # D Dorian
    mask = struct.unpack_from("<H", orig, 0xAC838 + 6)[0] & 0xFFF; root = 2
    for midi in (True, False):
        tag = "MIDI " if midi else "Audio"
        T = 9 if midi else 12                                               # Index Shuffle
        print(f"== 4. {tag}: neue Typen (Vorschau)")
        def mk(n=16):
            if midi: return [{6: 50 + (i * 5) % 14, 0: 60 + i, 1: 100 + 10 * i, 18: 0, 20: 0} for i in range(n)]
            return [{6: 50 + (i * 5) % 14, 0: 6000 + 100 * i, 9: 3, 10: 2048 * i, 11: 2048 * i + 2047, 18: 0, 20: 0} for i in range(n)]
        TT = 10 if midi else 2                                             # 12.11.: echte MIDI-Spur (8–15)
        sh_idx = (6, 0, 1) if midi else (9, 10, 11, 6, 0)
        def four():                                                       # 4 Steps auf 1/5/9/13, alle verschieden
            st = [None] * 16
            for k, pos in enumerate((0, 4, 8, 12)): st[pos] = {6: 50 + 3 * k, 0: (70 + k) if midi else 5000 + 900 * k, 1: 200 + 50 * k} if midi else {6: 50 + 3 * k, 0: 5000 + 900 * k, 9: 3 + k, 10: 1000 * k, 11: 1000 * k + 999}
            return st
        def sound(t):                                                     # was an Position j klingt: Quell-Position oder None
            out = []
            for j in range(16):
                r = v.src(t, j)
                if r == -2: out.append(None)
                elif r >= 0: out.append(r)
                else: out.append(j if v.on(t, j) else None)
            return out
        def baked(t): return [tuple(v.getp(v.addr(t, j), idx) for idx in sh_idx) if v.on(t, j) else None for j in range(16)]
        st = four(); v.track(TT, midi, st, sel=list(range(16))); v.rnd(T, 1.0)
        snd = sound(TT); pos = [j for j in range(16) if snd[j] is not None]
        check(pos == [0, 4, 8, 12] and all(snd[j] != j for j in pos) and sorted(snd[j] for j in pos) == pos,
              f"Swap Steps 100 %: Rhythmus bleibt, alle 4 Steps tauschen {snd}")
        want = [None if snd[j] is None else tuple(st[snd[j]][idx] for idx in sh_idx) for j in range(16)]
        v.bake(); check(baked(TT) == want, "Swap Steps eingebacken == Vorschau (ganze Steps)")
        st = four(); v.track(TT, midi, st, sel=list(range(16))); v.rnd(T, 0.5)
        snd = sound(TT); moved = sum(1 for j in range(16) if snd[j] is not None and snd[j] != j)
        check(moved == 2, f"Swap Steps 50 %: genau 2 von 4 tauschen ({moved}) {snd}")
        st = four(); v.track(TT, midi, st, sel=list(range(16))); v.rnd(T + 1, 1.0)
        snd = sound(TT); pos = [j for j in range(16) if snd[j] is not None]
        check(len(pos) == 4 and sorted(snd[j] for j in pos) == [0, 4, 8, 12] and not set(pos) & {0, 4, 8, 12},
              f"Scatter Steps 100 %: 4 Steps auf neue freie Plaetze {pos}")
        want = [None if snd[j] is None else tuple(st[snd[j]][idx] for idx in sh_idx) for j in range(16)]
        v.bake(); check(baked(TT) == want and sum(v.on(TT, j) for j in range(64)) == 4, "Scatter Steps eingebacken == Vorschau, Anzahl gleich")
        st = four(); v.track(TT, midi, st, sel=list(range(16))); v.rnd(T + 1, 0.25)
        snd = sound(TT); moved = sum(1 for j in range(16) if snd[j] is not None and snd[j] != j)
        check(moved == 1 and sum(1 for x in snd if x is not None) == 4, f"Scatter Steps 25 %: 1 Step wandert {snd}")
        st = four(); v.track(TT, midi, st, sel=[0, 1, 2, 3, 4, 5, 6, 7]); v.rnd(T + 1, 1.0)
        snd = sound(TT)
        check(all(snd[j] == j for j in (8, 12)) and all(snd[j] is None for j in range(16) if j not in range(8) and j not in (8, 12)),
              f"Scatter Steps: nur innerhalb der Auswahl (1–8), Rest unberuehrt {snd}")
        print(f"   12.11.: sichtbare Vorschau ({tag}): Step-Pads zeigen den Vorschau-Zustand, Bit danach unveraendert")
        st = four(); v.track(TT, midi, st, sel=list(range(16))); v.rnd(T + 1, 1.0)
        CTL, PG = 0x20022000, 0x20022800
        v.e.uc.mem_write(CTL, bytes(0x2500)); v.e.uc.mem_write(PG, bytes(0x120))
        v.e.w32(PG + 4, CTL); v.e.w32(CTL + 4, PATOBJ); v.e.uc.mem_write(PG + 0x112, bytes([1 if midi else 0]))
        drawn = {}
        if not hasattr(v, "_pd"):
            def pd(em):
                r_, c_ = em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2)
                drawn[(r_, c_)] = v.on(TT, c_)
            v.e.stub(0x3A3D4, cb=pd); v._pd = drawn
        drawn = v._pd; drawn.clear()
        v.e.uc.mem_write(0x1FFF4640, bytes(v.e.uc.mem_read(R, 0x40))); v.e.w32(0x1FFF4640, 0xA59F8)
        before = [v.on(TT, j) for j in range(16)]
        for c_ in range(16): v.e.call(v.nm["rz_pad_draw"] | 1, PG, TT - (8 if midi else 0), c_)
        prevvis = [drawn.get((TT - (8 if midi else 0), j)) for j in range(16)]
        snd = sound(TT); want = [x is not None for x in snd]
        check(prevvis == want and [v.on(TT, j) for j in range(16)] == before,
              f"Pads zeigen Scatter-Vorschau {''.join('#' if x else '.' for x in prevvis)} (vorher {''.join('#' if x else '.' for x in before)}), Steps unveraendert")
        calls = []
        if not hasattr(v, "_rd"):
            v.e.stub(0x213B4, cb=lambda em: v._rd.append(em.uc.reg_read(UC_ARM_REG_R0))); v._rd = calls
        calls = v._rd; calls.clear()
        v.e.w32(0x1FFF44E0 + 0x24B4, 0x20022C00); v.e.w32(0x20022C00, 0xA5A54)
        v.e.call(v.nm["rz_poll"] | 1); n1 = len(calls); v.e.call(v.nm["rz_poll"] | 1); n2 = len(calls)
        v.rnd(T + 1, 0.5); v.e.uc.mem_write(0x1FFF4640, bytes(v.e.uc.mem_read(R, 0x40))); v.e.w32(0x1FFF4640, 0xA59F8)
        v.e.call(v.nm["rz_poll"] | 1); n3 = len(calls)
        v.e.w32(0x1FFF4640 + 0x30, 0); v.e.call(v.nm["rz_poll"] | 1); n4 = len(calls)
        check(n1 >= 1 and n2 == n1 and n3 == n2 + 1 and n4 == n3 + 1, f"Neuzeichnen nur bei Aenderung (Anteil, aus): {n1}/{n2}/{n3}/{n4}")
        full = [{6: 50 + i % 7, 0: (60 + i) if midi else 6000 + 100 * i} for i in range(16)]
        v.track(TT, midi, full); v.rnd(T + 5, 0.5)
        snd = sound(TT); off = [j for j in range(16) if snd[j] is None]
        check(len(off) == 6 and not any(j % 4 == 0 for j in off), f"Thin Out 50 %: 6 von 12 Nicht-Vierteln aus {off}")
        v.bake(); check([j for j in range(16) if not v.on(TT, j)] == off, "Thin Out eingebacken: genau diese Steps aus")
        v.track(TT, midi, full); v.rnd(T + 5, 1.0)
        check(sum(1 for x in sound(TT) if x is not None) == 1, "Thin Out 100 %: ein Step bleibt")

        st = mk(); v.track(TT, midi, st); v.rnd(T + 2, 1.0)
        w = [v.prev(TT, i, 6) for i in range(16)]
        okw = w[0] == st[0][6] and all(scale_ok(n, mask, root) for n in w[1:]) and all(abs(n - st[0][6]) <= 7 + 4 for n in w)
        steps_ok = all(sum(1 for m in range(min(a, b) + 1, max(a, b) + 1) if scale_ok(m, mask, root)) <= 2 for a, b in zip(w, w[1:]) if scale_ok(a, mask, root))
        check(okw and steps_ok and len(set(w)) >= 3, f"Melodic Walk 100 %: Linie in der Scale, Schritte <= 2 Stufen {w}")
        v.rnd(T + 2, 0.3); w3 = [v.prev(TT, i, 6) for i in range(16)]
        kept = sum(1 for a, s_ in zip(w3, st) if a == s_[6])
        check(kept >= 6, f"Melodic Walk 30 %: {kept}/16 Originalnoten behalten")

        st = mk(); v.track(TT, midi, st); v.rnd(T + 3, 1.0)
        a = [v.prev(TT, i, 0) for i in range(16)]
        top = 127 if midi else 10000
        check(all(a[i] == top for i in range(0, 16, 4)) and all(a[i] <= st[i][0] for i in range(16) if a[i] != top),
              f"Accent 100 %: Viertel = {top}, Rest leiser {a[:8]}")

        st = mk(); v.track(TT, midi, st); v.rnd(T + 4, 1.0)
        c = [v.prev(TT, i, 6) for i in range(16)]
        check(c[:8] == [s_[6] for s_ in st[:8]] and c[8:15] == [s_[6] for s_ in st[:7]] and c[15] != st[7][6] and scale_ok(c[15], mask, root),
              f"Call & Response: 2. Haelfte = 1. Haelfte, letzte Note anders {c}")

        if not midi:
            st = mk(); v.track(2, False, st); v.rnd(10, 1.0)
            hu = [v.prev(2, i, 20) for i in range(16)]
            v.rnd(11, 1.0); oc = [v.prev(2, i, 6) for i in range(16)]
            check(all(-4 <= x <= 4 for x in hu) and 0 not in hu, f"Audio Humanize: {hu}")
            check(all(abs(o - s_[6]) == 12 for o, s_ in zip(oc, st)), f"Audio Octave Jump: ±12 {oc[:6]}")

        print(f"== 5. {tag}: Einbacken (rz_bake, zweistufig) == Vorschau")
        for typ, idxs in ((T + 2, (6,)), (T + 3, (0,)), (T + 4, (6,))):
            st = mk(); st[6] = None; v.track(TT, midi, st, sel=[i for i in range(16) if i != 9]); v.rnd(typ, 1.0, salt=typ)
            pv = [[v.prev(TT, i, idx) for idx in idxs] for i in range(16)]
            b0 = v.e.r32(0x1FFF9A60 + 0x1C)
            v.bake()
            bk = [[v.getp(v.addr(TT, i), idx) for idx in idxs] for i in range(16)]
            twophase = v.e.r32(0x1FFF9A60 + 0x1C) == b0 + (1 if typ in (T + 2, T + 4) else 0)
            empty = all(v.getp(v.addr(TT, 6), idx) == 0 for idx in idxs)
            check(bk == pv and twophase and v.e.rf(R + 0x30) == 0.0 and empty,
                  f"Typ {typ}: gebacken == Vorschau, Zweistufig {twophase}, leerer Step unberuehrt" + ("" if bk == pv else f" {bk[:4]} vs {pv[:4]}"))

    print("== 6. Typ wechseln")
    v.track(0, True, [{6: 60}]); v.e.w32(R + 0xC, 0); v.e.call(0x3893C, R, 40); m1 = v.e.r32(R + 0xC)
    v.track(0, False, [{6: 60}]); v.e.w32(R + 0xC, 0); v.e.call(0x3893C, R, 40); m2 = v.e.r32(R + 0xC)
    check(m1 == 14 and m2 == 17, f"MIDI max {m1}, Audio max {m2}")

    print(f"{'OK' if not fails else 'FEHLER'}: {checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
