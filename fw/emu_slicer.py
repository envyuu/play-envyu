#!/usr/bin/env python3
"""Maschinencode-Test von Patch 021 (Slicer/Granular): fuehrt den ECHTEN Firmware-Wiedergabepfad im Emulator aus.

Aufgebaut wird eine echte Spur-Stimme (Hal_createTrackNode 0x14AC0, Slots wie AudioEngine_createTracks), ein Audio-Step
mit der echten Step-vtable 0xAA338 im Pattern-Speicher des Engine-Objekts, und ein Kontext {Knoten, Mixer, Kanal}.
Attrappen nur fuer: Sample-Pool (liefert {Daten, Frames, Rate}), Heap (Bump-Zeiger).

Ausgeloest wird mit MidiTrack_triggerStep 0x4904C → MidiTrack_resolveEvent → vtable +0x4C (sl_begin) → +0x50
(FUN_00048910 → sl_param_hook → MidiTrack_getParam) → +0x54 → FUN_000166F4 (Frames ausrechnen, Stimme starten).

Geprueft:
  1. Patchstellen: genau die erwarteten Bytes, Rest des Bereichs 0x14000–0x4A000 und 0x9C000–0xAB000 unveraendert.
  2. Normal: Start/Ende-Frames der Stimme bitgleich mit dem Original-Image (Zufallswerte, mit/ohne gesetztes Ende).
  3. Slicer N = 4…64: Start/Ende == Slice-Grenzen (Python-Modell inkl. Rundung der Firmware), Gate, Zyklus,
     nur Start gesetzt, nur Ende gesetzt, kein Einfluss auf andere Spuren.
  4. Anzeige: Formatter-Eintraege 10/11 → "Slice k/N", "Gate x%", "Grain n ms"; Fallback auf den Original-Formatter.
  5. Wiedergabe: Normal-Spur ueber gr_update bitgleich mit dem Original (TrackVoice_update/Sampler_render, 40 Bloecke);
     Granular: Pegel/Fenster plausibel, kein Lesen ausserhalb des Samples, Befehle je Block gezaehlt.

usage: emu_slicer.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin
"""
import sys, struct, os, subprocess, random
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import veneer as VN  # 13.11.: Weichen (Sicherheitsnetz) aufloesen
from emu_nis import Emu, load_ppf  # noqa: E402

OFFS = 0x20019DC4
PAT0 = 0x200025D0
SL = 0x1FFF9800
POOLOBJ = 0x20020000     # Sample-Pool-Attrappe {vptr}
POOLVT = 0x20020010
POOLFN = 0x20020020      # Thumb: bx lr (Inhalt per Hook)
CTX = 0x20020100
EV = 0x20020200
MIXER = 0x20020300
VEC = 0x20020400         # Vektor fuer Hal_createTrackNode {x, end, cap} + Platz
DATA = 0x20030000        # Sampledaten (eigene Abbildung)
DATA_MAX = 0x100000
BLOCKS = 0x2001C000      # Audio-Block-Pool (32 × 0x108)
GTAB = 0x20010000        # Pegeltabelle 10001 × s16

fails = 0; checks = 0
GRAN_OFF = False   # 045: Release ohne Granular → Granular-Pruefungen nur anzeigen, nicht zaehlen

def check(c, m, quiet=False):
    global fails, checks
    if GRAN_OFF and (m.startswith("Granular") or m.startswith("Stop: Block ausgeblendet")):
        print("  AUS   ", m, "(Granular ausgeblendet)"); return
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    elif not quiet: print("  ok   ", m)


def syms():
    nm = {}
    for l in subprocess.check_output(["arm-none-eabi-nm", os.path.join(HERE, "fx.elf")], text=True).splitlines():
        p = l.split()
        if len(p) == 3: nm[p[2]] = int(p[0], 16)
    return nm


class Rig:
    """Eine Spur-Stimme + Step im echten Firmware-Code."""
    def __init__(self, img, patched, nm, track=2):
        self.e = e = Emu(img)
        self.patched = patched; self.nm = nm; self.track = track
        e.uc.mem_map(DATA, DATA_MAX)
        e.uc.mem_write(POOLFN, bytes.fromhex("7047"))
        e.w32(POOLOBJ, POOLVT); e.w32(POOLVT, POOLFN | 1)
        self.sample = (0, 0, 44100)   # (adresse, frames, rate)
        e.uc.hook_add(UC_HOOK_CODE, self._pool, begin=POOLFN, end=POOLFN + 1)
        if patched: e.call(nm["sl_init"])           # arp_init → ParamDefs_ctor, sl_reset, Formatter
        if patched: e.w32(0x1FFF9A04, 1)            # 044: Sequencer laeuft (sonst klingt Granular nur gehalten)
        else: e.call(0x271D0)
        e.call(0x49244)                              # Offsets der 22 Parameter → 0x20019DC4
        e.call(0x18DFC, GTAB)                        # Pegeltabelle, *0x20001C28 = GTAB
        e.call(0x17128, BLOCKS, 32)                  # AudioBlockPool_init
        e.w32(VEC, 0); e.w32(VEC + 4, VEC + 0x10); e.w32(VEC + 8, VEC + 0x40)
        self.node = e.call(0x14AC0, VEC)             # Hal_createTrackNode
        n = self.node
        vt = e.r32(n)
        slot = lambda i: e.call(e.r32(vt + 0x18), n, i)
        e.call(0x14600, slot(0), 1, 0x1DB0)          # wie AudioEngine_createTracks
        for i in (8, 9, 7): e.call(0x14600, slot(i), 0, 0)
        e.call(e.r32(vt + 0x20), n, POOLOBJ)          # Sample-Pool setzen (FUN_000166C8)
        self.stream = n + 0xD8
        self.offs = bytes(e.uc.mem_read(OFFS, 22))
        e.uc.mem_write(CTX, bytes(0x40)); e.w32(CTX, n); e.w32(CTX + 4, MIXER); e.w32(CTX + 8, track)
        self.step = PAT0 + 0x10 + track * 0xBA8 + 0x128 + 42 * 5
        self.vt = vt

    def _pool(self, uc, a, s, ud):
        e = self.e
        r2 = uc.reg_read(UC_ARM_REG_R2)
        adr, frames, rate = self.sample
        e.w32(r2 + 0x20, adr)                         # Vektor-Inhalt hinter dem Datensatz
        e.w32(r2, r2 + 0x20); e.w32(r2 + 4, r2 + 0x24); e.w32(r2 + 8, r2 + 0x24)
        e.w32(r2 + 0xC, frames); e.wf(r2 + 0x10, float(rate))          # Rate als float (vldr in 0x19598)

    def set_mode(self, t, m):
        self.e.w32(SL + 4 + 4 * t, m)

    def set_sample(self, frames, rate=44100, gen=None):
        e = self.e
        assert frames * 2 <= DATA_MAX
        if gen is not None:
            e.uc.mem_write(DATA, struct.pack("<%dh" % frames, *gen))
        self.sample = (DATA, frames, rate)

    def write_step(self, vals):
        """vals: {idx: wert}; setzt Werte und Maskenbits, Step aktiv (Bit 31)."""
        e, st = self.e, self.step
        e.uc.mem_write(st, bytes(42)); e.w32(st, 0xAA338)
        mask = 0
        for idx, v in vals.items():
            kind = e.uc.mem_read(0xAA114 + 16 * idx, 1)[0]
            a = st + 4 + self.offs[idx]
            if kind == 3: e.uc.mem_write(a, struct.pack("<h", v))
            else: e.uc.mem_write(a, bytes([v & 0xFF]))
            mask |= 1 << idx
        mask |= 1 << 31
        e.uc.mem_write(st + 0x26, struct.pack("<I", mask))

    def trigger(self, vals):
        self.write_step(vals)
        self.e.uc.mem_write(EV, bytes(0x40))
        self.e.call(0x4904C, self.step, CTX, EV)
        s = self.stream
        return self.e.r32(s + 0x28), self.e.r32(s + 0x2C), self.e.uc.mem_read(s + 0x39, 1)[0], self.e.uc.mem_read(s + 0x18, 1)[0]


def e_cfg(rig, win, spray):
    """039: Window/Spray liegen jetzt je Spur (gr_cfg_t): +1 win, +2 spray"""
    p = rig.e.call(rig.nm["gr_cfg"] | 1, rig.track)
    rig.e.uc.mem_write(p + 1, bytes([win, spray]))


def fw_frames(length, u):
    """FUN_000166F4: Start = len*u/0x7FFF (Ende: 0x7FFF = len)."""
    return length * u // 0x7FFF


def model_slice(length, n, v10, gate):
    k = (max(v10, 0) >> 6) % n
    s = k * 0x7FFF // n; e = (k + 1) * 0x7FFF // n
    g = min(max(gate, 0), 0x7FFF)
    en = s + (e - s) * g // 0x7FFF
    if g > 0 and en <= s: en = s + 1
    fs = fw_frames(length, s)
    fe = length if en == 0x7FFF else fw_frames(length, en)
    return k, fs, fe


SLICES = [0, 4, 8, 12, 16, 24, 32, 48, 64]


def main():
    ppf, fwfile = sys.argv[1], sys.argv[2]
    img = load_ppf(ppf); orig = bytearray(open(fwfile, "rb").read())
    nm = syms()

    print("== 1. Patchstellen")
    want = {0x489A4: bytes.fromhex("fff734ff"), 0xAA384: struct.pack("<I", 0x48105), 0x9CED0: struct.pack("<I", 0x18DD1)}
    for a, ob in want.items():
        check(bytes(orig[a:a + 4]) == ob, f"Original bei 0x{a:X} = {ob.hex()}")
    bl = struct.unpack_from("<HH", img, 0x489A4)
    off = ((bl[0] & 0x7FF) << 12 | (bl[1] & 0x7FF) << 1)
    s_ = (bl[0] >> 10) & 1; j1 = (bl[1] >> 13) & 1; j2 = (bl[1] >> 11) & 1
    i1 = 1 - (j1 ^ s_); i2 = 1 - (j2 ^ s_)
    imm = (s_ << 24) | (i1 << 23) | (i2 << 22) | off
    if s_: imm -= 1 << 25
    check(VN.follow(img, 0x489A4 + 4 + imm) == nm["sl_param_hook"], "0x489A4: bl sl_param_hook")
    check(VN.follow(img, struct.unpack_from("<I", img, 0xAA384)[0]) == nm["sl_begin"] | 1, "vtable 0xAA384 → sl_begin")
    check(VN.follow(img, struct.unpack_from("<I", img, 0x9CED0)[0]) == nm["gr_update"] | 1, "vtable 0x9CED0 → gr_update")
    check(struct.unpack_from("<I", img, 0xAE80C)[0] == nm["sn_guard"] | 1, "init_array 0xAE80C → sn_guard (13.11.: Sicherheitsnetz, ruft sl_init)")
    p019 = os.path.join(os.environ.get("BASELINE_DIR", os.path.join(HERE, "..")), "019_menu_nis", "PolyendPlay_v1.6.0_menu_nis.ppf")
    old019 = load_ppf(p019) if os.path.exists(p019) else None   # A/B-Vergleich nur mit altem Build (Release-Repo: BASELINE_DIR)
    d019 = {a for a in range(0, 0xB069C) if old019[a] != orig[a]} if old019 else set()
    d021 = {a for a in range(0, 0xB069C) if img[a] != orig[a]}
    new4 = set(range(0x489A4, 0x489A8)) | set(range(0xAA384, 0xAA388)) | set(range(0x9CED0, 0x9CED4))
    new4 |= set(range(0x15A3E, 0x15A42)) | set(range(0x15CB6, 0x15CBA))   # 022: Patch 020 (ui_cb_hook, ui_send_hook)
    for a in (0x1E0D2, 0x1E206, 0x1DFF4, 0x41A92, 0xA99D4): new4 |= set(range(a, a + 4))   # 024
    for a in (0x3B77E, 0xA5A70, 0xA0160, 0xA0164, 0xA5A68, 0x3A75A, 0x3A784): new4 |= set(range(a, a + 4))   # 026: View-Slicer
    new4 |= set(range(0xA97EC, 0xA97F0)) | set(range(0x30840, 0x30844)) | set(range(0xA0168, 0xA016C))                                                                       # 045: Tap Tempo (Menue vt+0x0C)
    new4 |= set(range(0x2FEC8, 0x2FECC)) | set(range(0xA015C, 0xA0160))   # 023: wv_view_hook, lr_button_hook
    for a in (0x52C4, 0x6E20, 0x6F28, 0x6FAE, 0x711E, 0x7176, 0x722E, 0x4EBC): new4 |= set(range(a, a + 4))   # 045: Color Mode (Leds_setPixel)
    for a in (0x3891C, 0x38AFC, 0x38C0C, 0x38D64, 0x38E70, 0x38B60, 0x38D5C, 0x21B26): new4 |= set(range(a, a + 4))   # 045 (11.11.): Audio-Typenliste, rz_*
    new4 |= {0x38950, 0x38DE8}                                                                                     # 045 (11.11.): Grenze Audio-Typen
    for a in (0x223B6, 0x222D4, 0x49D64, 0x49EAE, 0x49FBE, 0xA5A80): new4 |= set(range(a, a + 4))                                                    # 045 (12.11.): Perform behaelt die Spurauswahl (pf_reset_keep)
    new4 |= set(range(0x405C, 0x4060))                                                                             # 046 (14.11.): Startbild (Gfx_init-Endsprung → bs_boot)
    new4 |= set(range(0xA05FC, 0xA0600))                                                                           # 17.11.: Ladebildschirm (vtable 0xA05F8 [1] → ld_create)
    if old019 is not None and d021 - new4 != d019: print('   Abweichung:', sorted(hex(x) for x in (d021 - new4) ^ d019)[:12])
    if old019 is None: print("   SKIPPED Vergleich mit 019 (needs BASELINE_DIR)")
    else: check(d021 - new4 == d019 and not (d019 & new4),
          f"geaenderte Bytes im Original-Image = Stellen von 019 ({len(d019)}) + neue Stellen 021 + 020 + 024 ({len(d021 - d019)} von 40 Byte weichen ab)")
    from check_ppf import images
    rb = images(ppf)[1]; rbo = images(os.path.join(HERE, "..", "stock", "PolyendPlay_v1.6.0.ppf"))[1]
    check(rb == rbo, f"Rev-B-Image unveraendert ({len(rb)} Byte)")
    print("== 2. Normal-Spur: Frames bitgleich mit dem Original-Image")
    ro = Rig(orig, False, nm); rp = Rig(img, True, nm)
    global GRAN_OFF
    GRAN_OFF = "sl_is_granular" not in nm or rp.e.call(nm["sl_is_granular"] | 1, 9) == 0   # 045: ohne Granular als Konstante 0 entfernt
    if GRAN_OFF: print("  Hinweis: Granular ist in diesem Build ausgeblendet (sl_is_granular(9) = 0)")
    rnd = random.Random(21)
    for r in (ro, rp): r.set_sample(441000)
    bad = 0
    for it in range(150):
        vals = {9: 0, 6: rnd.randrange(30, 90), 10: rnd.randrange(0, 0x8000)}
        if it % 3: vals[11] = rnd.randrange(0, 0x8000)
        if it % 7 == 0: vals[13] = rnd.randrange(0, 10001); vals[14] = rnd.randrange(0, 10001)
        a = ro.trigger(vals); b = rp.trigger(vals)
        if a != b: bad += 1; print("    ", vals, a, b)
    check(bad == 0, "150 Steps: Start/Ende/Richtung/Spielen identisch")
    check(ro.trigger({9: 0, 6: 60, 10: 0})[3] == 1, "Stimme spielt nach dem Ausloesen")

    print("== 3. Slicer")
    r = rp
    for L in (441000, 123457, 4410):
        r.set_sample(L)
        for m in range(1, 9):
            n = SLICES[m]; r.set_mode(r.track, m)
            okc = 0; tot = 0
            for k in list(range(n)) + [n, n + 1, 2 * n + 3, 511]:
                for gate in (0x7FFF, 0x4000, 1, 0):
                    v10 = k * 64 + (32 if k % 2 else 0)       # 045: untere 6 Bit = Feinversatz; 0 und 32 = keiner
                    vals = {9: 0, 6: 60, 10: v10, 11: gate}
                    st, en, rev, _ = r.trigger(vals)
                    kk, fs, fe = model_slice(L, n, v10, gate)
                    tot += 1
                    if (st, en) == (fs, fe) and rev == 0: okc += 1
                    elif gate == 0 and (st, en) == (fs, fs): okc += 1      # Gate 0: Laenge 0
                    else: print("    ", L, n, k, gate, (st, en, rev), (fs, fe))
            check(okc == tot, f"Laenge {L}, {n} Slices: {tot} Steps auf den Slice-Grenzen", quiet=(L != 441000))
    r.set_sample(441000); n = 16; r.set_mode(r.track, 4)
    st, en, _, _ = r.trigger({9: 0, 6: 60, 10: 3 * 64})
    k, fs, fe = model_slice(441000, n, 3 * 64, 0x7FFF)
    check((st, en) == (fs, fe), f"nur Start gesetzt → ganzer Slice 4 ({st}…{en})")
    r.trigger({9: 0, 6: 60, 10: 9 * 64, 11: 0x7FFF})
    st, en, _, _ = r.trigger({9: 0, 6: 60, 11: 0x4000})
    k, fs, fe = model_slice(441000, n, 0, 0x4000)
    check((st, en) == (fs, fe), f"nur Ende gesetzt → Slice 1 (Standard-Start), halbes Gate ({st}…{en})")
    # 045: Vorhoeren der Firmware (Vorlage = Standard-Step) auf einer Slicer-Spur ohne offene Slicer-Seite: Original-Werte
    r.e.w32(0x1FFF450C + 0x7C, r.step)
    st, en, _, _ = r.trigger({9: 0, 6: 60, 10: 0x2000, 11: 0x6000})
    check((st, en) == (fw_frames(441000, 0x2000), fw_frames(441000, 0x6000)), f"Vorlage vorhoeren: Start/Ende wie Original ({st}…{en})")
    r.e.w32(0x1FFF450C + 0x7C, 0)
    st, en, _, _ = r.trigger({9: 0, 6: 60, 10: 3 * 64})
    check((st, en) == model_slice(441000, n, 3 * 64, 0x7FFF)[1:], "normaler Step danach wieder als Slice")
    # 045: Feinversatz des Anfangs (untere 6 Bit), Ende bleibt
    _, fs4, fe4 = model_slice(441000, n, 5 * 64, 0x7FFF)
    for low, fo in ((16, 16), (31, 31), (48, -16), (33, -31)):
        st, en, _, _ = r.trigger({9: 0, 6: 60, 10: 5 * 64 + low})
        want = fs4 + fo * (fe4 - fs4) // 64
        check(abs(st - want) <= 1 and en == fe4, f"Feinversatz {fo:+d}/64: Start {st} (erwartet {want}), Ende {en} = Slice-Ende")
    st, en, _, _ = r.trigger({9: 0, 6: 60, 10: 16 * 64})
    check(st == 0 and en == 441000 // 16 * 1 or (st, en) == model_slice(441000, 16, 16 * 64, 0x7FFF)[1:], "Raste 16 bei 16 Slices → wieder Slice 1")
    # andere Spur unbeeinflusst: Spur 2 auf Slicer, Rig-Spur 3 normal
    r3 = Rig(img, True, nm, track=3); r3.set_sample(441000); r3.set_mode(2, 4)
    o3 = Rig(orig, False, nm, track=3); o3.set_sample(441000)
    vals = {9: 0, 6: 60, 10: 12345, 11: 23456}
    check(r3.trigger(vals) == o3.trigger(vals), "Spur 4 (Normal) unveraendert, waehrend Spur 3 Slicer ist")
    sl = r.e.uc.mem_read(SL, 0x100)
    check(struct.unpack_from("<I", sl, 0xAC + 4 * r.track)[0] == r.node, "sl_begin merkt den Knoten der Spur")

    print("== 4. Anzeige (Formatter-Eintraege 10/11 der Audio-Tabelle)")
    e = r.e
    def fmt(idx, value, rel=0, step=None):
        obj = e.r32(0x1FFF7918 + 0x20 * idx + 0x10)
        buf = 0x20020800; e.uc.mem_write(buf, bytes(40))
        fn = e.r32(e.r32(obj))
        e.sp = 0x2002F000
        e.uc.mem_write(e.sp - 0x100, bytes(0x100))
        # 5./6. Argument auf dem Stack: size, step
        sp = 0x2002EF00
        e.uc.mem_write(sp, struct.pack("<II", 32, step if step is not None else r.step))
        uc = e.uc
        for i, a in enumerate((obj, value & 0xFFFFFFFF, rel, buf)): uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.reg_write(UC_ARM_REG_SP, sp); uc.reg_write(UC_ARM_REG_LR, 0xFFF00 | 1)
        uc.emu_start(fn, 0xFFF00, count=2_000_000)
        return e.cstr(buf)
    check(e.r32(SL + 0x80) == 1, "Formatter installiert (fmt_ok)")
    r.set_mode(r.track, 4)
    check(fmt(10, 3 * 64 + 32) == "Slice 4/16" and fmt(10, 3 * 64) == "Slice 4/16", "Slicer 16: Wert 224/192 → „Slice 4/16“ (kein Versatz)")
    check(fmt(10, 3 * 64 + 5) == "Slice 4 +7%" and fmt(10, 3 * 64 + 60) == "Slice 4 -6%", f"045 Feinversatz: {fmt(10, 3 * 64 + 5)}, {fmt(10, 3 * 64 + 60)}")
    check(fmt(10, 0) == "Slice 1/16", "Wert 0 → „Slice 1/16“")
    check(fmt(11, 0x7FFF) == "Gate 100%", "Ende 0x7FFF → „Gate 100%“")
    check(fmt(11, 0x4000) == "Gate 50%", "Ende 0x4000 → „Gate 50%“")
    r.set_mode(r.track, 9)
    check(fmt(11, 0x7FFF) == "Grain 1000 ms", "Granular: Ende 0x7FFF → „Grain 1000 ms“")
    check(fmt(11, 0) == "Grain 5 ms", "Granular: Ende 0 → „Grain 5 ms“")
    t10 = fmt(10, 0x4000, step=0)
    check(t10 == "50.0 %", f"ohne Step → Original-Formatter („{t10}“)")
    t10r = fmt(10, 0x4000, rel=1)
    check(t10r == "+50.0 %", f"relativ → Original („{t10r}“)")
    # Fallback mit Step (Original liest dann den Sample-Pool der App – hier abgefangen und gezaehlt)
    hits = []
    def orig_fmt(uc, a, s, ud):
        hits.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
        uc.mem_write(uc.reg_read(UC_ARM_REG_R3), b"ORIG\0")
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    h = e.uc.hook_add(UC_HOOK_CODE, orig_fmt, begin=0x2AD38, end=0x2AD39)
    r.set_mode(r.track, 4)
    check(fmt(10, 0x4000, step=r.step + 1) == "ORIG", "Step-Adresse nicht auf dem Step-Raster → Original-Formatter")
    check(fmt(10, 0x4000, step=0x20020000) == "ORIG", "Step ausserhalb der Patterns → Original-Formatter")
    r.set_mode(r.track, 0)
    check(fmt(10, 0x4000) == "ORIG" and fmt(11, 0x4000) == "ORIG", "Normal-Spur → Original-Formatter (Start und Ende)")
    o10 = e.r32(SL + 0xA4); o11 = e.r32(SL + 0xA8)
    check(hits[-2][0] == o10 and hits[-1][0] == o11 and hits[-1][1] == 0x4000, "Original bekommt sein eigenes Objekt und den Wert")
    check(e.r32(o10) == 0xA0338 and e.r32(o11) == 0xA0338, "Original-Objekte haben die vtable 0xA0338")
    r.set_mode(r.track, 4)
    p1 = PAT0 + 0xBA9C - 0xC + 0x10 + r.track * 0xBA8 + 0x128 + 42 * 63
    check(fmt(10, 64, step=p1) == "Slice 2/16", "Step 64 im zweiten Pattern (0x2000E060) → Spur erkannt")
    e.uc.hook_del(h)

    print("== 5. Wiedergabe")
    # Sample: Saegezahn mit Kennung, damit falsche Leseposition auffaellt
    L = 88200
    smp = [((i * 37) % 2000) - 1000 for i in range(L)]
    reads = []
    def render(rig, blocks):
        e = rig.e
        out = []; cnt = [0]
        def tx(uc, a, s, ud):
            blk = uc.reg_read(UC_ARM_REG_R1)
            out.append(struct.unpack("<128h", uc.mem_read(blk + 8, 256)))
        h1 = e.uc.hook_add(UC_HOOK_CODE, tx, begin=0x17294, end=0x17295)
        def cc(uc, a, s, ud): cnt[0] += 1
        h2 = e.uc.hook_add(UC_HOOK_CODE, cc)
        fn = e.r32(e.r32(rig.stream)) | 1
        per = []
        for b in range(blocks):
            c0 = cnt[0]
            e.call(fn & ~1, rig.stream)
            per.append(cnt[0] - c0)
        e.uc.hook_del(h1); e.uc.hook_del(h2)
        return out, per
    ro = Rig(orig, False, nm); rp = Rig(img, True, nm)
    for rig in (ro, rp):
        rig.set_sample(L, gen=smp)
        rig.trigger({9: 0, 6: 64, 10: 1000, 11: 30000, 12: 37, 13: 500, 14: 800})
    a, pa = render(ro, 40); b, pb = render(rp, 40)
    check(len(a) == 40 and a == b, f"Normal-Spur: 40 Bloecke bitgleich mit dem Original ({len(a)}/{len(b)})")
    print(f"     Befehle je Block (Normal): Original {sum(pa)//len(pa)}, mit gr_update {sum(pb)//len(pb)}")
    # Granular
    for mode, name in ((9, "vorwaerts"), (10, "rueckwaerts"), (11, "Ping-Pong")):
        rg = Rig(img, True, nm); rg.set_sample(L, gen=[int(8000 * __import__('math').sin(i * 0.05)) for i in range(L)])
        rg.set_mode(rg.track, mode)
        lo_hi = []
        def mr(uc, acc, addr, size, val, ud):
            lo_hi.append(addr)
        h = rg.e.uc.hook_add(UC_HOOK_MEM_READ, mr, begin=DATA - 0x1000, end=DATA + DATA_MAX)
        e_cfg(rg, 100, 20); rg.trigger({9: 0, 6: 60, 10: 0x4000, 11: 6000, 13: 0, 14: 0})
        out, per = render(rg, 60)
        rg.e.uc.hook_del(h)
        flat = [x for blk in out for x in blk]
        pk = max(abs(x) for x in flat[1280:])
        inside = all(DATA <= x < DATA + 2 * L for x in lo_hi)
        check(len(out) == 60 and pk > 1000, f"Granular {name}: 60 Bloecke, Pegel {pk}")
        check(inside, f"Granular {name}: {len(lo_hi)} Lesezugriffe alle im Sample")
        gl = struct.unpack_from("<I", rg.e.uc.mem_read(SL + 0xF4 + 8, 4))[0]
        check(gl == 60, f"Granular {name}: n_gr = {gl}")
        print(f"     Befehle je Block (Granular {name}, Grain {__import__('math').floor(5 + 995 * (6000/32767)**2)} ms, Fenster 50 %): "
              f"min {min(per)}, mittel {sum(per)//len(per)}, max {max(per)}")
        # Sprungfreiheit: keine Differenz zwischen Nachbarsamples groesser als bei der Sinusquelle moeglich
        jumps = max(abs(flat[i + 1] - flat[i]) for i in range(1280, len(flat) - 1))
        check(jumps < 2000, f"Granular {name}: groesster Sprung zwischen Samples {jumps} (Quelle Sinus, max. Steigung ~800)")
    # Neustart einer Granular-Spur mit langem Grain und Spray, CPU-Spitze
    rg = Rig(img, True, nm); rg.set_sample(L, gen=smp); rg.set_mode(rg.track, 11)
    e_cfg(rg, 100, 100); rg.trigger({9: 0, 6: 96, 12: 100, 10: 0x7000, 11: 0x7FFF, 13: 0, 14: 0})
    out, per = render(rg, 30)
    check(len(out) == 30, "Granular P-P, Grain 1000 ms, Spray 100 %, Note +3 Okt.: 30 Bloecke")
    print(f"     Befehle je Block (schlimmster Fall, 2 Grains): max {max(per)}")
    # 024: Sequencer-Stopp → Granular blendet im naechsten Block aus und beendet die Stimme
    rg.e.call(nm["sl_transport"], 0)
    out2, _ = render(rg, 4)
    st18 = rg.e.uc.mem_read(rg.stream + 0x18, 1)[0]
    gr_ptr = rg.e.call(nm["gr_state"] | 1, rg.track)
    nrel = rg.e.r32(gr_ptr + 24 * 8 + 16 + 4 + 4 + 8 + 4)   # n_rel in gr_t
    fade_ok = len(out2) >= 1 and abs(out2[0][127]) <= max(1, max(abs(x) for x in out2[0][:8]) // 64 + 1) and \
              all(abs(out2[0][i]) <= abs(out[-1][i]) + 2000 for i in range(128))
    silent_after = all(all(x == 0 for x in blk) for blk in out2[1:])
    check(fade_ok and st18 == 0 and nrel == 1 and silent_after,
          f"Stop: Block ausgeblendet (letztes Sample {out2[0][127] if out2 else '-'}), Stimme aus (+0x18 = {st18}), n_rel {nrel}, danach still ({len(out2) - 1} Bloecke)")
    rg.e.call(nm["gr_set_playing"] | 1, 1); e_cfg(rg, 0, 0); rg.trigger({9: 0, 6: 96, 12: 100, 10: 0x7000, 11: 0x7FFF, 13: 0, 14: 0})   # gleicher Step, kurzes Fenster: Ton sofort
    out3, _ = render(rg, 40)                                   # Fenster 50 % von 1000 ms: Einblenden ~0,5 s
    pk3 = [max(abs(x) for x in blk) for blk in out3]
    if os.environ.get("DBG"):
        g = rg.e.uc.mem_read(gr_ptr, 0x40); st = rg.e.uc.mem_read(rg.stream, 0x68)
        print("DBG gr", g.hex(), "\nDBG st", st.hex())  # Diagnose bei Bedarf: DBG=1
    ref30 = max(max(abs(x) for x in blk) for blk in out)
    check(len(out3) == 40 and max(pk3[:30]) >= ref30 // 2 and rg.e.uc.mem_read(rg.e.call(nm["gr_state"] | 1, rg.track) + 211, 1)[0] == 0, f"neue Note nach dem Stopp klingt wieder (Spitze {max(pk3[:30])} in 30 Bloecken, erster Lauf {ref30}, +0x18 = {rg.e.uc.mem_read(rg.stream + 0x18, 1)[0]})")
    print("== 6. Patch 029: Spurwerte im echten Trigger-Pfad, Regler-Hooks")
    SV = 0x1FFFB940
    r = Rig(img, True, nm); e = r.e; t = r.track
    r.set_sample(441000); r.set_mode(t, 4)                     # Slicer 16
    n = 16; W = 0x7FFF // n
    e.uc.mem_write(SV + 0x44 + t, bytes([32]))                 # Versatz +32/64
    st, en, rev, _ = r.trigger({9: 0, 6: 60, 10: 2 * 64, 11: 0x7FFF})
    a = 2 * 0x7FFF // n + 32 * W // 64; b = 3 * 0x7FFF // n + 32 * W // 64
    check((st, en, rev) == (fw_frames(441000, a), fw_frames(441000, b), 0), f"Versatz: Slice 3 verschoben ({st}, {en})")
    st, en, rev, _ = r.trigger({9: 0, 6: 60, 10: 15 * 64, 11: 0x7FFF})
    check(en == 441000 and st < en and rev == 0, "Versatz: letzter Slice endet am Sample-Ende, nicht leer")
    e.uc.mem_write(SV + 0x44 + t, bytes([0]))
    vt = e.r32(r.node); slot = lambda i: e.call(e.r32(vt + 0x18), r.node, i)
    src1 = lambda i: struct.unpack("<h", e.uc.mem_read(slot(i) + 2, 2))[0]
    e.uc.mem_write(SV + 0x34 + 2 * t, struct.pack("<h", 6))    # Sample-Index 5
    r.trigger({9: 2, 6: 60, 10: 0}); a1 = src1(7)
    r.trigger({6: 60, 10: 0}); a2 = src1(7)
    check(a1 == 5 and a2 == 5, f"Sample der Spur schlaegt Step-Sample, auch ohne gesetztes Bit ({a1}, {a2})")
    e.uc.mem_write(SV + 0x34 + 2 * t, bytes(2))
    # 031: Grid-Breite im echten Trigger
    e.call(nm["sv_cfg_set"] | 1, t, 6, 512)                     # Faktor 1,5
    st, en, rev, _ = r.trigger({9: 0, 6: 60, 10: 3 * 64, 11: 0x7FFF})
    a = 3 * 0x7FFF * 1536 // 1024 // 16; b = 4 * 0x7FFF * 1536 // 1024 // 16
    check((st, en, rev) == (fw_frames(441000, a), fw_frames(441000, b), 0), f"Breite 1,5: Slice 4 ({st}, {en})")
    e.call(nm["sv_cfg_set"] | 1, t, 6, 0)
    # Regler-Hooks: Original-Funktionen durch bx lr + Zaehler ersetzt
    cnt = {}
    for ad in (0x3B088, 0x2A71C, 0x2141C, 0x2D0D8):
        e.uc.mem_write(ad, bytes.fromhex("7047")); cnt[ad] = 0
        e.uc.hook_add(UC_HOOK_CODE, (lambda ad: (lambda uc, a, sz, ud: cnt.__setitem__(ad, cnt[ad] + 1)))(ad), begin=ad, end=ad + 1)
    ED = 0x20020000; UI = 0x20020400; SO = 0x20020800
    e.w32(0x1FFF6994, ED); e.w32(ED, 0xA5A54); e.w32(ED + 4, 0x20020C00)
    e.uc.mem_write(ED + 0x18, bytes([1])); e.w32(UI + 8, SO); e.w32(SO + 0x2FC, 0)
    e.w32(0x1FFF4518, 1 << t)
    chg = lambda i, d: e.call(nm["sv_chg_hook"] | 1, ED, i, d & 0xFFFFFFFF)
    knob = lambda k, d: e.call(nm["sv_knob_hook"] | 1, UI, k, d & 0xFFFFFFFF)
    off = lambda: struct.unpack("<b", e.uc.mem_read(SV + 0x44 + t, 1))[0]
    view = lambda on: e.uc.mem_write(ED + 0x18, bytes([1 if on else 0]))
    dfl = lambda i: struct.unpack("<h", e.uc.mem_read(0x1FFF7918 + i * 0x20, 2))[0]
    check(dfl(0) == 7600, f"Standard Volume aus der RAM-Tabelle ({dfl(0)})")
    # Spurauswahl beim Aufruf von FUN_0002d0d8 mitschreiben
    seen = []
    e.uc.hook_add(UC_HOOK_CODE, lambda uc, a, sz, ud: seen.append(e.r32(0x1FFF4518)), begin=0x2D0D8, end=0x2D0D9)
    c0 = cnt[0x3B088]; d0 = cnt[0x2D0D8]
    for i, d in ((11, -64), (13, 10), (14, 10), (2, 100), (3, 100), (0, 200), (1, 100), (4, 1), (5, 100), (6, 1), (7, 100), (8, 100), (12, 100)):
        chg(i, d)
    check(cnt[0x2D0D8] == d0 + 13 and cnt[0x3B088] == c0, "View ohne Step: alle Klangregler direkt an den Standard-Step (FUN_0002d0d8)")
    check(seen[-13:] == [0] * 13, f"View: ohne Spurauswahl ({set(seen[-13:])})")
    check(e.r32(0x1FFF4518) == 1 << t, "Spurauswahl danach wieder da")
    seen.clear(); e.hooks = None
    seen38 = []
    e.uc.hook_add(UC_HOOK_CODE, lambda uc, a, sz, ud: seen38.append(e.r32(0x1FFF4518)), begin=0x3B088, end=0x3B089)
    chg(21, 1); chg(21, 1); chg(21, -1); check(off() == 1 and cnt[0x2D0D8] == d0 + 13, "View: Move bleibt Vorbereitung des Samples")
    chg(21, -1)
    e.uc.mem_write(ED + 0x113, bytes([1])); chg(2, 100); e.uc.mem_write(ED + 0x113, bytes([0]))
    check(seen38[-1] == 1 << t, "Live Rec: Original sieht die Auswahl")
    e.w32(ED + 0x88, 1); chg(2, 100); e.w32(ED + 0x88, 0); check(seen38[-1] == 1 << t, "gehaltener Step: Auswahl unveraendert")
    # Bulk = Firmware: ausserhalb von View kein Eingriff
    view(False); c0 = cnt[0x3B088]
    for i, d in ((2, 500), (13, 10), (6, 1), (11, -64), (9, 1), (21, 1), (20, 1), (10, 64)):
        chg(i, d)
    check(cnt[0x3B088] == c0 + 8 and seen38[-1] == 1 << t, "ausserhalb von View: alles Original, Auswahl sichtbar (Bulk der Firmware)")
    # 032: Huellkurve im Bild = Firmware (Anteile der Slice-Dauer)
    r.trigger({9: 0, 6: 60, 10: 0, 11: 0x7FFF, 13: 2500, 14: 4000})
    bs, be = e.r32(r.stream + 0x30), e.r32(r.stream + 0x34); ba, bd = e.r32(r.stream + 0x58), e.r32(r.stream + 0x5C)
    print(f"     Bloecke Start {bs} Ende {be} AttackEnde {ba} DecayBeginn {bd}")
    check(abs((ba - bs) - (be - bs) * 0.25) <= 2, f"Firmware: Attack 25 % der Dauer ({ba - bs}/{be - bs})")
    check(abs((be - bd) - (be - bs) * 0.40) <= 2, f"Firmware: Decay 40 % der Dauer ({be - bd}/{be - bs})")
    view(True)
    e.w32(ED + 0x88, 1); c0 = cnt[0x3B088]; chg(11, -64); chg(21, 1); chg(6, 1); chg(2, 1); check(cnt[0x3B088] == c0 + 4, "gehaltener Step: Gate/Move/Note/Cutoff Original")
    knob(10, 1); check(e.r32(SL + 4 + 4 * t) == 4 and cnt[0x2A71C] == 1, "gehaltener Step: Track Length Original")
    e.w32(ED + 0x88, 0)
    ref0 = cnt[0x2141C]; knob(10, 1); check(e.r32(SL + 4 + 4 * t) == 5 and cnt[0x2A71C] == 1 and cnt[0x2141C] == ref0 + 1, "Track Length → Slicer 24, Pad-LEDs neu")
    knob(10, -1); knob(10, -1); check(e.r32(SL + 4 + 4 * t) == 3, "Slice-Zahl runter")
    e.uc.mem_write(SV + 0x4C + t, bytes([20])); e.uc.mem_write(SV + 0x54 + t, bytes([1])); knob(10, -1); knob(10, -1)
    check(e.uc.mem_read(SV + 0x4C + t, 1)[0] == 0 and e.uc.mem_read(SV + 0x54 + t, 1)[0] == 0, "gewaehlter Slice/Seite ausserhalb → zurueckgesetzt")
    for _ in range(12): knob(10, -1)
    check(e.r32(SL + 4 + 4 * t) == 1, "klemmt bei Slicer 4")
    c1 = cnt[0x2A71C]; knob(3, 1); check(cnt[0x2A71C] == c1 + 1, "anderer Knob: Original")
    e.uc.mem_write(UI + 0x18, bytes([1])); knob(10, 1); check(cnt[0x2A71C] == c1 + 1 and e.r32(SL + 4 + 4 * t) == 2, "030: Shift waehlt nicht den Fokus, weiter Slice-Zahl")
    knob(10, -1); e.uc.mem_write(UI + 0x18, bytes([0])); e.uc.mem_write(ED + 0x18, bytes([0])); knob(10, 1)
    check(cnt[0x2A71C] == c1 + 2, "ausserhalb von View: Original")
    e.uc.mem_write(ED + 0x18, bytes([1])); r.set_mode(t, 0); knob(10, 1); check(cnt[0x2A71C] == c1 + 3 and e.r32(SL + 4 + 4 * t) == 0, "Normal-Spur: Original")
    c2 = cnt[0x3B088]; chg(11, 1); chg(9, 1); chg(21, 1); check(cnt[0x3B088] == c2 + 3, "Normal-Spur: Regler Original")
    r.set_mode(t, 4)
    # Sample-Regler: Live Recordings zuerst
    BASE = 0x20021000; META = 0x20025000
    e.w32(0x1FFF69BC + 4, BASE); e.w32(0x1FFF69BC + 8, META)
    for i, fo in ((1, 0), (3, 1), (6, 1)):
        rc = BASE + 0x38 * i; e.w32(rc, 0x20020F00); e.w32(rc + 4, 0x20020F04); e.w32(rc + 0xC, 100); e.w32(META + 4 * i, fo)
    e.w32(META + 0x400, 2); e.uc.mem_write(META + 0x404, b"Foo\0"); e.uc.mem_write(META + 0x404 + 17, b"Live Recordings\0")
    smp = lambda: struct.unpack("<h", e.uc.mem_read(SV + 0x34 + 2 * t, 2))[0]
    seq = []
    for _ in range(4): chg(9, 1); seq.append(smp() - 1)
    check(seq == [6, 3, 1, 6], f"Sample-Regler: neueste Aufnahme, dann rund ({seq})")
    chg(9, -1); check(smp() - 1 == 1, "Sample-Regler rueckwaerts")
    print(f"ERGEBNIS: {checks - fails}/{checks} Pruefungen ok" + ("" if fails == 0 else f", {fails} FEHLER"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
