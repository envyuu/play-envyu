#!/usr/bin/env python3
"""Patch 030: ABLAUF-Tests – echter Knob-Pfad (Ui_onKnobTurn → SeqCtl_knobTurn → KnobActions → SeqKnob_turn → Editor-vtable → Hooks),
echte Step-Seite (StepParamView_show), echter Widget-Manager + FT81x-Modell. Attrappen: Editor-Original 0x3B088, Knob-Funktionen,
UI-Refresh. Aufruf: emu_flow.py PPF  (fx.elf liegt daneben)"""
import os, random, struct, sys
import numpy as np
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_LR
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import emu_display as D
from emu_display import Disp, syms, load_ppf, SDRAM, WV, VIEW, RECS, SCREENS, ARGS
from eve_emu import save_png

checks = fails = 0
def check(c, m):
    global checks, fails
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)

SV = 0x1FFFB940; SL = 0x1FFF9800; ED = 0x20024000; UI = 0x20024400; S8 = 0x20024800; SEQ = 0x20025000; SCR = 0x20028000
T = 1; MILLIS = 0x20000D7C
SLICERMODE = 4   # Slicer 16

class Flow:
    def __init__(self, img, nm):
        self.nm = nm; self.d = d = Disp(img, True, nm); self.e = e = d.e
        frames = 88200
        sig = np.zeros(frames)
        for k in range(16):
            s0 = k * frames // 16; n = frames // 16
            sig[s0:s0 + n] = np.exp(-np.arange(n) / 1200) * np.sin(np.arange(n) * 0.05)
        e.uc.mem_write(SDRAM, np.clip(sig * 30000, -32767, 32767).astype("<i2").tobytes())
        d.set_sample(7, SDRAM, frames); e.uc.mem_write(RECS + 0x38 * 7 + 0x14, b"Rec 003\0")
        FOLD = SDRAM + 0x300000                              # 045: Ordnertabelle der Sample-Liste (+8): Index je Sample, Namen ab +0x404 (17 B)
        e.uc.mem_write(FOLD, bytes(0x404 + 17 * 4)); e.uc.mem_write(FOLD + 0x404, b"Breaks\0"); e.w32(D.SLIST + 8, FOLD)
        d.rig.track = T
        from emu_slicer import PAT0
        self.step = PAT0 + 0x10 + T * 0xBA8 + 0x128 + 42 * 5
        d.rig.step = self.step
        d.rig.set_mode(T, SLICERMODE)
        d.rig.write_step({9: 7, 10: 2 << 6})
        # Attrappen mit Zaehlern
        self.cnt = {}
        for name, a in (("chg", 0x3B088), ("vorl", 0x2D0D8), ("len", 0x2073C), ("pm", 0x208C0), ("ref", 0x2141C), ("master", 0x20AC0), ("tempo", 0x210E8), ("rnd", 0x1F180), ("touch", 0x2A74C), ("lim", 0x20B34), ("swing", 0x20F54)):
            self.cnt[name] = 0
            e.stub(a, cb=(lambda n: (lambda _: self.cnt.__setitem__(n, self.cnt[n] + 1)))(name))
        e.call(0x22035)                                     # KnobActions_init (echt)
        e.w32(0x1FFF6994, ED); e.w32(ED, 0xA5A54); e.w32(ED + 4, 0x20024C00); e.w32(ED + 8, 0x1FFF450C)
        self.view(True)
        e.w32(UI + 8, S8); e.w32(S8 + 0x2FC, 0); e.w32(UI + 0xC, SEQ)
        e.w32(SEQ + 0x24B4, ED); e.w32(SEQ + 0x24B8, 0x1FFF7918); e.w32(SEQ + 0xC, SCR)
        e.w32(SCR, VIEW); e.w32(SCR + 4, VIEW + 4); e.w32(SCR + 0xBF * 4, VIEW); e.w32(SCR + 0xC0 * 4, VIEW)
        e.w32(0x1FFF4518, 1 << T)                            # Spur gewaehlt
        e.w32(0x1FFF450C + 0x7C, self.step); e.w32(0x1FFF450C + 0x5C, 0)
        e.w32(MILLIS, 100000)
        e.w32(SL + 0xAC + 4 * T, d.rig.node)                 # Spurknoten bekannt: Vorhoeren direkt
        from emu_slicer import PAT0 as _P
        tr = _P + 0x10 + T * 0xBA8; e.w32(tr + 0x88, d.rig.node); e.w32(tr + 0x8C, 0x20020300); e.w32(tr + 0x90, T)   # Kontext im Spurobjekt
        e.w32(0x20020300, 0x20020380); e.w32(0x20020380 + 0x10, 0xFFE81); e.stub(0xFFE80, ret=0x200203C0)
        self.tick()
    # ---- Bedienung
    def view(self, on): self.e.uc.mem_write(ED + 0x18, bytes([1 if on else 0]))
    def alt(self, a): self.e.w32(VIEW + 200, a)              # linke Taste: Fokus oben/unten
    def shift(self, s): self.e.uc.mem_write(UI + 0x18, bytes([s]))
    def hold(self, on): self.e.w32(ED + 0x88, 1 if on else 0)
    def knob(self, k, d=1): self.e.call(self.nm["sv_knob_hook"] | 1, UI, k, d & 0xFFFFFFFF)
    def touch(self, k): self.e.call(self.nm["sv_touch_hook"] | 1, UI, k, 0)
    def pad(self, k):
        row, col = 1 + k // 4, k % 4
        for ev in (0, 1): self.e.call(self.nm["sv_kb_hook"] | 1, ED, row, col, ev)
    def padev(self, k, ev):                                  # 17.11.: Pad nur druecken (0) oder loslassen (1)
        self.e.call(self.nm["sv_kb_hook"] | 1, ED, 1 + k // 4, k % 4, ev)
    def wait(self, ms): self.e.w32(MILLIS, self.e.r32(MILLIS) + ms)
    def step_page(self, i0, i1, v0, v1):                     # Firmware zeichnet ihre Step-Seite (echt)
        self.d.rig.write_step({9: 7, i0: v0, i1: v1})
        return self.d.show(i0, i1, v0, v1, step=False) if False else self._show(i0, i1, v0, v1)
    def _show(self, i0, i1, v0, v1, rel=0):
        d = self.d; e = self.e; a = ARGS + 0x40
        e.uc.mem_write(a, bytes([i0, i1]) + struct.pack("<hh", v0, v1) + bytes([1, 1, rel, rel]))
        d.call6(0x2FD55, VIEW, a, a + 2, a + 6, a + 8, self.step)
    def tick(self):
        e = self.e
        e.call(self.nm["sv_poll"] | 1)
        for _ in range(300):
            e.call(self.nm["wv_poll"] | 1)
            if e.r32(WV + 0x0C) == 0: break
        self.d.pump()
        return self.d.eve.frames[-1]
    # ---- Zustand
    def r16(self, a): return struct.unpack("<h", self.e.uc.mem_read(a, 2))[0]
    def hl0(self): return struct.unpack("<H", self.e.uc.mem_read(WV + 0x60, 2))[0]
    def wview(self): return self.e.r32(WV + 0x14)
    def won(self): return self.e.uc.mem_read(WV + 8, 1)[0]
    def txt(self, off): return bytes(self.e.uc.mem_read(WV + off, 24)).split(b"\0")[0].decode(errors="replace")
    def t0(self): return self.txt(0xF0)
    def t1(self): return self.txt(0x108)
    def i0(self): return self.txt(0x120)
    def i1(self): return self.txt(0x138)
    def off(self): return struct.unpack("<b", self.e.uc.mem_read(SV + 0x44 + T, 1))[0]
    def mode(self): return self.e.r32(SL + 4 + 4 * T)
    def foc(self): return struct.unpack("<i", self.e.uc.mem_read(WV + 0x38 + 4 * 1 + 0, 4))[0]

def orange(fr, y0, y1, x0, x1):
    p = fr[y0:y1, x0:x1]
    return int(((p[:, :, 0] == 0xFF) & (p[:, :, 1] == 0x55) & (p[:, :, 2] == 0x40)).sum())
def expect_box(fr, where):
    """Fokus: 'L' = oberer Parameter (ueber der Waveform), 'R' = unterer (darunter), None = kein Kasten"""
    o = orange(fr, 25, 63, 0, 320); u = orange(fr, 186, 210, 0, 320)
    if where == "L": return o > 100 and u == 0 and orange(fr, 25, 63, 200, 320) == 0
    if where == "R": return u > 100 and o == 0 and orange(fr, 186, 210, 200, 320) == 0
    return o == 0 and u == 0
def hl_x(k, n=16): return (k * 0x7FFF // n) * 304 // 0x7FFF

def main():
    ppf = sys.argv[1]
    nm = syms(); img = load_ppf(ppf)
    f = Flow(img, nm); e = f.e
    print("A) Pads ohne Step-Beruehrung: Anzeige folgt")
    for k in (0, 5, 11, 3, 15):
        f.pad(k); fr = f.tick()
        check(f.t1() == f"Slice {k+1}/16", f"Pad {k}: Text {f.t1()}")
        check(abs(f.hl0() - hl_x(k)) <= 1, f"Pad {k}: Markierung x={f.hl0()} erwartet {hl_x(k)}")
        px = fr[100, 8 + hl_x(k) + 3]
        check(int(px[0]) >= 0x30, f"Pad {k}: Slice im Bild hell hinterlegt ({px})")

    print("B) GEMELDETER BUG: Step beruehren, loslassen, dann Pads → Anzeige muss weiter folgen")
    f.hold(True); f._show(10, 11, 9 << 6, 0x7FFF); f.d.rig.write_step({9: 7, 10: 9 << 6}); fr = f.tick()
    check(f.t1() == "Slice 10/16", f"gehaltener Step zeigt seinen Slice: {f.t1()}")
    f.hold(False); f.pad(2); fr = f.tick()
    check(f.wview() == 1, f"Slicer-Seite aktiv (view={f.wview():#x})")
    check(f.t1() == "Slice 3/16" and abs(f.hl0() - hl_x(2)) <= 1, f"nach Step: Pad 2 → {f.t1()} x={f.hl0()}")
    for k in (7, 1, 14):
        f.pad(k); fr = f.tick()
        check(f.t1() == f"Slice {k+1}/16" and abs(f.hl0() - hl_x(k)) <= 1, f"nach Step: Pad {k} → {f.t1()} x={f.hl0()}")
    if len(sys.argv) > 2:
        print(f"ERGEBNIS nur A/B: {checks - fails}/{checks}"); sys.exit(1 if fails else 0)
    f.wait(4000); f.tick()

    print("C) Zwei Werte auf einem Knob, Fokus sichtbar; in View bleiben vorhandene Steps unberuehrt")
    f.alt(0); c = dict(f.cnt)
    f.knob(12, 1); f.knob(12, 1)
    check(f.cnt["vorl"] == c["vorl"] + 2 and f.cnt["chg"] == c["chg"], "View ohne Step: Attack geht an den Standard-Step")
    f.d.rig.write_step({9: 7, 13: 2000, 14: 500}); f._show(13, 14, 2000, 500); fr = f.tick()
    check("Attack" in f.i0() and "Decay" in f.i1() and expect_box(fr, "L"), f"Beide Werte, Kasten links: {f.i0()} | {f.i1()}")
    save_png(fr, os.path.join(SCREENS, "07_fokus_attack.png"))
    # GEMELDET (035): Firmware-Seite im Mehrfach-Modus (Spur gewaehlt, Anzeige "~"/"+x") darf in View nicht durchscheinen
    txt0 = f.i0(), f.i1()
    f.wait(100); f._show(13, 14, 2000, 500, rel=1); fr = f.tick()
    check(f.i0() == txt0[0] and f.i1() == txt0[1] and "~" not in f.i0() + f.i1() and "+" not in f.i0() + f.i1(), f"keine Tilde, Wert des Standard-Steps: {f.i0()} | {f.i1()}")
    f.hold(True); f._show(13, 14, 2000, 500, rel=1); f.tick(); held_txt = f.i0(); f.hold(False)
    check("~" in held_txt or "+" in held_txt, f"gehaltener Step: Text der Firmware bleibt ({held_txt})")
    f._show(13, 14, 2000, 500); fr = f.tick()
    f.alt(1); fr = f.tick()
    check(expect_box(fr, "R"), "linke Taste: Kasten wandert OHNE Drehen zu Decay")
    c = dict(f.cnt); f.knob(12, 1); check(f.cnt["vorl"] == c["vorl"] + 1, "Decay-Regler")
    save_png(f.tick(), os.path.join(SCREENS, "08_fokus_decay.png"))
    f.wait(3100); fr = f.tick()
    check(expect_box(fr, "R") and "Slices" not in f.i0(), f"045: nach 3 s bleibt der Fokus (dauerhaft): {f.i0()}")
    f.wait(600000); fr = f.tick()
    check(expect_box(fr, "R"), "045: auch nach 10 min noch Fokus")
    f.alt(0)

    print("D) Track Length, Move / Grid-Breite, Sample Start / End")
    m0 = f.mode(); c = dict(f.cnt)
    f.knob(10, 1); fr = f.tick()
    check(f.mode() == m0 + 1 and f.cnt["len"] == c["len"] and expect_box(fr, "L") and "24 Slices" in f.i0(), f"Track Length → Slices: {f.mode()} {f.i0()}")
    f.shift(1); f.knob(10, -1); f.shift(0); f.tick()
    check(f.mode() == m0, "Shift aendert den Fokus NICHT (weiter Slices)")
    f.alt(0); o0 = f.off(); f.knob(14, 1); f.knob(14, 1); fr = f.tick()
    check(f.off() == o0 + 2 and expect_box(fr, "L") and f.i0().startswith("Offset +"), f"Move → Offset: {f.off()} {f.i0()}")
    f.knob(14, 1); f.tick(); check(f.off() == o0 + 3, "Offset 3")
    f.alt(1); c = dict(f.cnt); f.tick()
    check(f.i1() == "Grid Fit", f"045: Grid-Breite 0 = \"Grid Fit\": {f.i1()}")
    f.knob(14, 1); fr = f.tick()
    check(f.i1() == "Grid Fit" and f.off() == o0 + 3 and f.cnt["chg"] == c["chg"] and expect_box(fr, "R"), "045: erste Raste rastet an Fit (Micromove = Grid-Breite, Kasten rechts)")
    f.wait(100); f.knob(14, 1); f.tick()
    check(f.i1().endswith(" BPM"), f"zweite Raste verlaesst Fit: {f.i1()}")
    bpm1 = float(f.i1().split()[0])
    f.wait(100); f.knob(14, 1); f.tick(); bpm2 = float(f.i1().split()[0]); check(bpm2 < bpm1, f"breiteres Grid = weniger BPM ({bpm1} → {bpm2})")
    f.wait(100); f.knob(14, -1); f.tick(); f.wait(100); f.knob(14, -1); f.tick(); f.wait(100); f.knob(14, -1); f.tick()
    check(f.i1() == "Grid Fit", f"zurueckdrehen haelt an Fit an: {f.i1()}")
    f.alt(0)
    for _ in range(3): f.knob(14, -1)
    f.pad(0); f.tick(); f.wait(100); f.knob(7, 1); fr = f.tick()
    check(e.call(nm["sv_cut_any"] | 1, T, 16) == 0, "17.11.: Pad nur angetippt (losgelassen) → Sample Start schneidet NICHT")
    tv_ = struct.unpack("<h", bytes(e.uc.mem_read(e.r32(0x1FFF450C + 0x7C) + 4 + f.d.rig.offs[10], 2)))[0]
    check(f.i0().startswith("Start +") and (tv_ & 63) != 32, f"17.11.: ohne Halten = Feinversatz der Vorlage: Fokus {f.i0()!r}, Vorlage {tv_}")
    f.wait(100); f.knob(7, -1); f.tick()
    f.padev(0, 0); f.tick(); f.wait(100); f.knob(7, 1); fr = f.tick()
    check(f.t1() == "Slice 1/16" and f.i0().startswith("Cut 1 +") and expect_box(fr, "L"), f"17.11.: Pad gehalten → Sample Start verschiebt den Anfang: {f.t1()} {f.i0()}")
    f.wait(100); f.knob(7, -1); f.padev(0, 1); f.tick(); e.call(nm["sv_cut_set"] | 1, T, 0, 0, 0)
    # Slices waehlt der Encoder: Abschnitt M
    f.alt(1); c = dict(f.cnt); f.knob(7, -1); fr = f.tick()
    check(f.cnt["vorl"] == c["vorl"] + 1 and expect_box(fr, "R") and f.i1().startswith("Gate"), f"Sample End: Standard-Step, Anzeige {f.i1()}")
    f.alt(0); f.wait(3100); f.tick()

    print("D1) 12.11.: Handschnitte – gruenes Pad 3 = Cut, Sample Start verschiebt den Anfang des gewaehlten Slices")
    BA = 0x2002F800
    def bnd(k, n=16):
        e.w32(e.sp, BA); e.w32(e.sp + 4, BA + 4)          # Argument 5/6 auf dem Stack (AAPCS)
        e.call(nm["sl_bounds"] | 1, T, k, n, 0x7FFF)
        return struct.unpack("<ii", bytes(e.uc.mem_read(BA, 8)))
    def gpad(col):
        for ev in (0, 1): e.call(nm["sv_kb_hook"] | 1, ED, 0, col, ev)
    a2, b2 = bnd(2); a1, b1 = bnd(1)
    gpad(2); f.tick()
    check(e.uc.mem_read(SV + 0xB8 + T, 1)[0] == 0, "13.11.: gruenes Pad 3 ist kein Modus mehr (Sample Start schneidet immer)")
    f.padev(2, 0); f.tick()
    for _ in range(5): f.wait(100); f.knob(7, 1)
    fr = f.tick(); c2, d2_ = bnd(2); c1, d1 = bnd(1)
    from PIL import Image as _I; _I.fromarray(fr).save(os.path.join(SCREENS, "06c_cut.png"))
    check(c2 > a2 and d1 == c2 and c1 == a1 and d2_ == b2, f"Anfang Slice 3 nach rechts ({a2} → {c2}), Ende Slice 2 folgt ({b1} → {d1}), Rest gleich")
    check(f.i0().startswith("Cut 3 +5") and f.t1() == "Slice 3/16", f"Fokuszeile {f.i0()!r}, oben {f.t1()!r}")
    for _ in range(40): f.knob(7, -1)                     # schnell zurueck: 10-ms-Rasten, Halt vor Slice 2
    f.padev(2, 1); f.tick(); e2_, _ = bnd(2); a1n, _ = bnd(1)
    f.wait(100); f.knob(7, 1); f.tick()
    check(bnd(2)[0] == e2_, "17.11.: Pad losgelassen → weiteres Drehen schneidet nicht mehr")
    check(a1n < e2_ < a2 and e2_ - a1n >= 2 * 16, f"schnell nach links, bleibt hinter dem Anfang von Slice 2 ({a1n} < {e2_})")
    lr = f.e.r32(SV + 0x68)
    check(lr != 0, "Aenderung markiert (engines.cfg wird geschrieben)")
    key = e.call(nm["sv_cut_key"] | 1, T, 16)
    cut = e.call(nm["sv_cut"] | 1, T, 2, 16); cut = cut - (1 << 32) if cut >= 1 << 31 else cut
    check(cut != 0 and key & 0x80, f"Schnitt gespeichert: {cut}, Schluessel {key:#x}")
    e.call(nm["sv_clear_track"] | 1, T); check(e.call(nm["sv_cut_any"] | 1, T, 16) == 0, "Spur geleert → keine Schnitte")
    e.call(nm["sv_cut_set"] | 1, T, key, 2, cut & 0xFFFFFFFF); e.uc.mem_write(SV + 0xB8 + T, bytes([2]))
    check(bnd(2)[0] == e2_, "aus engines.cfg (sv_cut_set) wiederhergestellt")
    check(e.call(nm["sv_cut"] | 1, T, 2, 8) == 0, "andere Slice-Zahl: Schnitte gelten nicht")
    f.padev(0, 0); f.tick(); f.wait(100); f.knob(7, 1); f.tick()
    check(f.t1() == "Slice 1/16" and f.i0().startswith("Cut 1"), f"Sample Start bleibt beim Slice ({f.t1()}, {f.i0()})")
    f.padev(0, 1); f.tick()
    e.call(nm["sv_cut_set"] | 1, T, 0, 0, 0)
    e.call(nm["sv_cut_set"] | 1, T, 0, 2, 0)              # Schnitte fuer die folgenden Tests aus
    f.wait(3100); f.tick()

    print("D2) Vorhoeren mit Vorlage, Huellkurve, Grid-Breite, Beat an echtem Material")
    d = f.d; d.rig.write_step({9: 7, 6: 72, 10: 0}); f.pad(0); f.tick()
    r72 = struct.unpack("<f", e.uc.mem_read(d.rig.stream + 0x3C, 4))[0]
    d.rig.write_step({9: 7, 6: 60, 10: 0}); f.pad(0); f.tick()
    r60 = struct.unpack("<f", e.uc.mem_read(d.rig.stream + 0x3C, 4))[0]
    check(abs(r72 / r60 - 2.0) < 0.01, f"Pad spielt die Note der Vorlage ({r60} / {r72})")
    # Huellkurve
    def envp(fr):
        p = fr[66:182, 8:312]; return int(((p[:, :, 0] == 0x9A) & (p[:, :, 1] == 0x9A) & (p[:, :, 2] == 0x9A)).sum())
    d.rig.write_step({9: 7, 10: 0, 13: 0, 14: 0}); fr = f.tick(); check(envp(fr) == 0, "Attack/Decay 0: keine Huellkurve")
    d.rig.write_step({9: 7, 10: 0, 13: 3000, 14: 4000}); fr = f.tick(); e1 = envp(fr)
    check(e1 > 40, f"Huellkurve am gewaehlten Slice sichtbar (13.11.: Trapez-Umriss, {e1} Pixel)")
    save_png(fr, os.path.join(SCREENS, "13_huellkurve.png"))
    col = fr[:, 8 + 19 * 1 + 2];
    # GEMELDET: Standard-Step aendert sich ohne Step-Beruehrung → Bild muss folgen (Regler ist im Test Attrappe, Wert direkt gesetzt)
    d.rig.write_step({9: 7, 10: 0, 13: 8000, 14: 0}); fr2 = f.tick(); check(not np.array_equal(fr, fr2), "Attack geaendert: Bild ohne Step-Beruehrung neu")
    d.rig.write_step({9: 7, 10: 0, 13: 0, 14: 0}); f.tick()
    def lines(fr):
        row = fr[66].astype(int); return [x for x in range(8, 312) if row[x][2] - row[x][0] >= 0x18 and row[x][2] >= 0x28]   # 13.11.: feine Linien (geglaettet) am Blauanteil
    f.pad(0); fr = f.tick(); l0 = lines(fr)
    check(len(l0) >= 13, f"Rasterlinien sichtbar ({len(l0)})")
    f.alt(1)
    for _ in range(40): f.wait(100); f.knob(14, 1)
    fr = f.tick(); l1 = lines(fr)
    check(l1 and l0 and l1[0] > l0[0], f"Breite: Linien ruecken auseinander ({l0[:2]} → {l1[:2]})")
    save_png(fr, os.path.join(SCREENS, "10_grid_breite_bpm.png"))
    for _ in range(40): f.wait(100); f.knob(14, -1)
    f.alt(0); f.tick()
    # Beat: echte Live-Aufnahme des Nutzers
    import wave as _w
    recp = os.path.join(HERE, "test_data", "Rec_001.wav")
    if os.path.exists(recp) and not os.environ.get("FLOW_SYNTH"):
        wf = _w.open(recp); frames = wf.getnframes(); raw = wf.readframes(frames)
    else:                                                  # Release-Repo: Aufnahme ist privat → Ersatz mit 10 Anschlaegen (6. bei 1,266 s)
        print("   (synthetischer Ersatz fuer Rec_001.wav)")
        frames = int(2.6 * 44100); sigb = np.zeros(frames); rs_ = np.random.RandomState(3)
        for k_, t_ in enumerate((0.005, 0.31, 0.55, 0.79, 1.03, 1.266, 1.52, 1.77, 2.02, 2.28)):
            p_ = int(t_ * 44100); n_ = 7000
            sigb[p_:p_ + n_] += (0.45 + 0.5 * rs_.rand()) * np.exp(-np.arange(n_) / 1200) * np.sin(np.arange(n_) * (0.05 + 0.02 * k_))
        raw = np.clip(sigb * 30000, -32767, 32767).astype("<i2").tobytes()
    e.uc.mem_write(SDRAM, raw); d.set_sample(8, SDRAM, frames); e.uc.mem_write(RECS + 0x38 * 8 + 0x14, b"Rec_001\0")
    d.rig.write_step({9: 8, 10: 0})
    m_ = f.mode(); e.w32(SL + 4 + 4 * T, 1)                # Slicer 4: wie am Geraet beim Befund
    f.alt(1); f.knob(10, 1); fr = f.tick()
    check(f.i1().startswith("Beat") and f.mode() == 6, f"Beat an, nutzt alle Slice-Plaetze (045: 32): {f.i1()} mode {f.mode()}")
    from unicorn import UC_HOOK_CODE
    cnt_ = [0]; hk = e.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s_, u: cnt_.__setitem__(0, cnt_[0] + 1))
    e.call(nm["sv_poll"] | 1); e.uc.hook_del(hk)
    print(f"   sv_poll waehrend der Analyse: {cnt_[0]} Befehle")
    check(cnt_[0] < 90000, f"Analyse blockiert die Hauptschleife nicht ({cnt_[0]} Befehle je Durchlauf)")
    n_ = 0
    while not (f.i1().startswith("Beat ") and f.i1() != "Beat ...") and n_ < 400: fr = f.tick(); n_ += 1
    check(f.i1() != "Beat ...", f"Analyse fertig nach {n_} Durchlaeufen: {f.i1()}")
    f.alt(0); nc = []
    for _ in range(16): f.knob(10, -1); f.tick()
    for lv in range(1, 17):
        fr = f.tick(); nc.append(int(f.i1().split()[1]))
        check(f.i0() == f"Sensitivity {lv}/16", f"Sensitivity: {f.i0()}")
        if lv < 16: f.knob(10, 1); f.tick()
    print(f"   Slices je Sensitivity: {nc}")
    fr = f.tick(); save_png(fr, os.path.join(SCREENS, "14_sensitivity.png"))
    f.alt(0); f.knob(10, -1); f.knob(10, -1); f.knob(10, -1); f.knob(10, -1); fr = f.tick()
    box = fr[66:182, 8:312]; check(int(((box[:, :, 0] == 0xFF) & (box[:, :, 1] == 0x55)).sum(axis=1).max()) > 250, "Schwelle als Linie im Bild")
    save_png(fr, os.path.join(SCREENS, "15_sensitivity_schwelle.png"))
    for _ in range(4): f.knob(10, 1)
    f.tick()
    check(nc == sorted(nc) and nc[0] < nc[-1], f"grob → fein mehr Slices: {nc}")
    check(f.mode() == 6, "Genauigkeit aendert die Pad-Plaetze nicht")
    check(nc[-1] == 10, f"Rec_001: alle 10 Anschlaege auf Stufe 16 ({nc[-1]})")
    f.wait(3100); f.tick(); fr = f.tick(); save_png(fr, os.path.join(SCREENS, "11_beat_rec_001.png"))
    f.pad(5); fr = f.tick()
    hp = f.hl0() * frames // 304
    check(abs(hp - int(1.266 * 44100)) < 500, f"Pad 6 = Anschlag bei 1,266 s ({hp / 44100:.3f} s)")
    f.pad(20); fr2 = f.tick(); check(f.t1() == "Slice 6/10", f"Pad ohne Slice bleibt stumm: {f.t1()}")
    check(f.t1().endswith("/10"), "Anzeige zaehlt echte Slices")
    f.alt(1); f.knob(10, -1); f.tick()
    check(f.mode() == 1 and f.i1() == "Grid", f"zurueck: Slice-Zahl wie vorher ({f.mode()}, {f.i1()})")
    f.alt(0); f.wait(3100); f.tick()
    # Sample tauschen und wieder Beat: rechnet neu
    frames2 = 88200; sig2 = np.zeros(frames2)
    for h in (0, 30000, 60000):
        n = 6000; sig2[h:h + n] = np.exp(-np.arange(n) / 1400) * np.sin(np.arange(n) * 0.09)
    e.uc.mem_write(SDRAM + 0x100000, np.clip(sig2 * 30000, -32767, 32767).astype("<i2").tobytes())
    d.set_sample(8, SDRAM + 0x100000, frames2); f.alt(1); f.knob(10, 1); n_ = 0; f.tick()
    while f.i1() == "Beat ..." and n_ < 400: f.tick(); n_ += 1
    check(f.i1() == "Beat 3", f"getauschtes Sample neu berechnet: {f.i1()}")
    f.knob(10, -1); f.alt(0); f.wait(3100)
    frames = 88200; e.w32(SL + 4 + 4 * T, m_); d.set_sample(7, SDRAM, frames)
    sig = np.zeros(frames)
    for k in range(16):
        s0 = k * frames // 16; n = frames // 16
        sig[s0:s0 + n] = np.exp(-np.arange(n) / 1200) * np.sin(np.arange(n) * 0.05)
    e.uc.mem_write(SDRAM, np.clip(sig * 30000, -32767, 32767).astype("<i2").tobytes())
    d.rig.write_step({9: 7, 10: 2 << 6}); f.tick()

    print("E) Fremder Knob und gehaltener Step: Werte der Firmware-Seite in der Fokuszeile")
    c = dict(f.cnt); f.knob(11, 1); check(f.cnt["vorl"] == c["vorl"] + 1, "View: Cutoff geht an die Vorlage")
    f._show(2, 3, 12000, 4000); fr = f.tick()
    check(("Cutoff" in f.i0() or "Pass" in f.i0()) and "Resonance" in f.i1() and expect_box(fr, "L") and f.wview() == 1, f"Cutoff sichtbar: {f.i0()} | {f.i1()}")
    f.alt(1); fr = f.tick(); check(expect_box(fr, "R"), "Resonanz im Fokus: Kasten rechts")
    f.alt(0); f.wait(3100); f.tick()
    f.hold(True); c = dict(f.cnt); f.knob(12, 1)
    check(f.cnt["chg"] == c["chg"] + 1, "gehaltener Step: Attack geht an den Step")
    f._show(13, 14, 2500, 500); fr = f.tick()
    check("Attack" in f.i0() and "Decay" in f.i1() and expect_box(fr, "L"), f"Step-Werte sichtbar: {f.i0()} | {f.i1()}")
    f.hold(False); f.wait(3100); f.tick()
    f.knob(0, 1); f.tick(); check(not f.won() and f.cnt["master"] >= 1, "Master: Firmware-Anzeige")
    f.wait(1600); f.tick(); check(not f.won(), "Master: bleibt stehen (kein Zeitablauf)")
    f.knob(11, 1); f.tick(); check(f.won(), "Master: anderer Knob -> Slicer-Seite")

    print("F) Ausserhalb von View: Waveform-Step-Ansicht wie gehabt, mit Fokus")
    f.view(False); f.tick()
    check(not f.won() or f.wview() != 1, "View aus: Slicer-Seite weg")
    # Bulk = Firmware, ausserhalb von View ohne Eingriff
    c = dict(f.cnt); f.knob(11, 1); f.knob(12, 1); check(f.cnt["chg"] == c["chg"] + 2, "Klangregler ausserhalb von View: Original")
    c = dict(f.cnt); m0 = f.mode(); f.knob(10, 1)
    check(f.cnt["len"] == c["len"] + 1 and f.mode() == m0, "Track Length Original")
    f.alt(0); f.d.eve.frames.clear() if False else None
    f._show(10, 11, 3 << 6, 0x4000)
    for _ in range(300):
        e.call(nm["wv_poll"] | 1)
        if e.r32(WV + 0x0C) == 0: break
    f.d.pump(); fr = f.d.eve.frames[-1]
    check(f.won() and f.wview() == VIEW, f"Step-Ansicht an (view={f.wview():#x})")
    check(expect_box(fr, "L"), "Fokus Sample Start: Kasten oben")
    save_png(fr, os.path.join(SCREENS, "09_step_fokus_start.png"))
    # Kasten deckt die ganze Schrift, schwarze Schrift im Kasten sichtbar
    col = fr[:, 14:100]; ys = [y for y in range(25, 64) if ((col[y, :, 0] == 0xFF) & (col[y, :, 1] == 0x55)).any()]
    check(len(ys) >= 20, f"Kasten so hoch wie die Schrift ({len(ys)} Zeilen)")
    check(int((fr[40:62, 14:100].sum(axis=2) < 30).sum()) > 50, "schwarze Schrift im Kasten sichtbar")
    f.alt(1); f._show(10, 11, 3 << 6, 0x4000); f.d.pump(); fr = f.d.eve.frames[-1]
    check(expect_box(fr, "R"), "Fokus Sample End: Kasten unten")
    f.alt(0)
    print("G) Wechsel nach View mit offener Step-Ansicht: Slicer-Seite uebernimmt")
    f.view(True); f.pad(4); f.tick()
    check(f.wview() == 1 and f.t1() == "Slice 5/16", f"nach View: {f.wview():#x} {f.t1()}")

    print("H) Zufallsablauf: 1500 Bedienschritte, Anzeige muss jeden Zustand richtig zeigen")
    rnd = random.Random(30); bad = 0; held = False; sel = 4; f.alt(0); alt = 0
    for i in range(1500):
        op = rnd.randrange(9)
        if op == 0:
            sel = rnd.randrange(16); f.mode() == SLICERMODE and f.pad(sel)
        elif op == 1: f.knob(rnd.choice([12, 11, 8, 3, 4, 6, 13, 1]), rnd.choice([1, -1]))
        elif op == 2: alt ^= 1; f.alt(alt)
        elif op == 3: f.touch(rnd.randrange(15))
        elif op == 4: held = not held; f.hold(held)
        elif op == 5: f.wait(rnd.choice([10, 500, 3200]))
        elif op == 6 and not held: f._show(2, 3, rnd.randrange(32767), rnd.randrange(32767))
        elif op == 7 and held:
            v = rnd.randrange(16); f.d.rig.write_step({9: 7, 10: v << 6}); f._show(10, 11, v << 6, 0x7FFF); hv = v
        elif op == 8: f.knob(0, 1) if rnd.random() < 0.1 else None
        fr = f.tick()
        d_ = (f.e.r32(SV + 0x7C) - f.e.r32(MILLIS)) & 0xFFFFFFFF
        if 0 < d_ < 0x80000000:
            if f.won(): bad += 1; print("    Firmware-Anzeige erwartet", i)
            continue
        if not f.won() or f.wview() != 1: bad += 1; print("    Seite weg", i, f.wview()); continue
        if not held:
            if f.t1() != f"Slice {sel+1}/16": bad += 1; print("    Text", i, f.t1(), sel)
            if abs(f.hl0() - hl_x(sel)) > 1 + abs(f.off()) * 304 // (16 * 64) + 1: bad += 1; print("    Markierung", i, f.hl0(), sel)
        if bad > 5: break
    check(bad == 0, f"Zufallsablauf: {bad} Abweichungen")

    print("I) 045: gruene Pads = Modus (Slices/Keys), Keys an die Original-Tastatur, Bulk-Anzeige in Slices")
    kb = {"n": 0, "rows": [], "keys": []}
    from unicorn.arm_const import UC_ARM_REG_R2 as _R2, UC_ARM_REG_R3 as _R3
    def h_kb(em):
        kb["n"] += 1; kb["rows"].append(em.uc.reg_read(UC_ARM_REG_R1)); kb["keys"].append((em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(_R2), em.uc.reg_read(_R3)))
    e.stub(0x3A1A8, cb=h_kb)
    f.hold(False); f.view(True)
    def raw(row, col, ev): e.call(nm["sv_kb_hook"] | 1, ED, row, col, ev)
    for ev in (0, 1): raw(0, 1, ev)
    check(e.uc.mem_read(SV + 0xB8 + T, 1)[0] == 1 and kb["n"] == 0, "Gruenes Pad 2 → Keys, nicht an die Firmware")
    sel_before = e.uc.mem_read(SV + 0x4C + T, 1)[0]
    e.w32(SV + 0x70, 0)
    raw(3, 2, 0)
    node_ = e.r32(SV + 0x70)
    raw(3, 2, 1)
    KO = 0x2002F900; e.uc.mem_write(KO, struct.pack("<iiii", 0, -1, 0, 0)); e.call(0x2301D, KO, ED + 0x14, 3, 1)
    want_ = struct.unpack("<i", bytes(e.uc.mem_read(KO + 4, 4)))[0]
    tn_ = struct.unpack("<h", bytes(e.uc.mem_read(e.r32(0x1FFF450C + 0x7C) + 4 + f.d.rig.offs[6], 2)))[0]
    check(want_ >= 0 and tn_ == want_, f"17.11.: Keys ohne gehaltenen Step: gespielte Tonhoehe landet in der Vorlage (neue Steps): {tn_} (Taste {want_})")
    check(kb["n"] == 1 and node_ != 0, f"Keys ohne gehaltenen Step: Slice direkt angespielt (Stimme {node_:#x}), nur Loslassen an die Firmware ({kb['n']}x)")
    TPL = e.r32(0x1FFF450C + 0x7C)
    def tpl(i): return struct.unpack("<h", bytes(e.uc.mem_read(TPL + 4 + f.d.rig.offs[i], 2)))[0]
    def keynote(r, c):
        e.uc.mem_write(KO, struct.pack("<iiii", 0, -1, 0, 0)); e.call(0x2301D, KO, ED + 0x14, r, c); return struct.unpack("<i", bytes(e.uc.mem_read(KO + 4, 4)))[0]
    sel_k = (e.uc.mem_read(SV + 0x4C + T, 1)[0] or 1) - 1
    e.uc.mem_write(ED + 0x113, b"\x01"); n_ = kb["n"]; e.w32(SV + 0x70, 0); raw(3, 2, 0); node_l = e.r32(SV + 0x70); s10 = tpl(10); raw(3, 2, 1); e.uc.mem_write(ED + 0x113, b"\x00")
    check(kb["n"] == n_ + 2 and kb["keys"][-2:] == [(3, 2, 0), (3, 2, 1)], f"Keys bei Live Rec: echte Taste an die Firmware, Druck und Loslassen (Aufnahme): {kb['keys'][-2:]}")
    check(s10 >> 6 == sel_k and node_l == 0, f"17.11.: Keys + Live Rec: Vorlage traegt den gewaehlten Slice {sel_k + 1} (Sample Start {s10}), kein eigenes Anspielen (Stimme {node_l:#x})")
    f.hold(True); n_ = kb["n"]; raw(3, 2, 0); aud = e.uc.mem_read(SV + 0x5C + T, 1)[0]; raw(3, 2, 1); f.hold(False)
    check(kb["n"] == n_ + 2 and aud == (sel_before or 1), f"Keys mit gehaltenem Step: an die Firmware (Note schreiben), Slice {aud} vorgemerkt")
    check(e.uc.mem_read(SV + 0x4C + T, 1)[0] == sel_before, "Keys aendert den gewaehlten Slice nicht")
    n4 = kb["n"]
    for ev in (0, 1): raw(0, 3, ev)
    check(e.uc.mem_read(SV + 0xB8 + T, 1)[0] == 1 and kb["n"] == n4, "Gruenes Pad 4: ohne Funktion, verbraucht")
    for ev in (0, 1): raw(0, 0, ev)
    n0 = kb["n"]; f.pad(5)
    check(e.uc.mem_read(SV + 0xB8 + T, 1)[0] == 0 and kb["n"] == n0 and e.uc.mem_read(SV + 0x4C + T, 1)[0] == 6, "Gruenes Pad 1 → Slices: Pad waehlt wieder Slice 6")
    # 17.11.: Live Rec im Slices-Modus – Pad 10 (Reihe 3, Spalte 1) nimmt Slice 10 auf, mit der Tonhoehe der Vorlage
    for i_ in range(28): e.uc.mem_write(0x20001D44 + 4 * i_, struct.pack("<i", 40 + i_))   # Skalen-Tastatur wie am Geraet: 28 verschiedene Noten
    tn0 = keynote(5, 2); e.uc.mem_write(TPL + 4 + f.d.rig.offs[6], struct.pack("<h", tn0))  # Vorlage = Ton einer ANDEREN Taste als Pad (3, 1)
    e.uc.mem_write(ED + 0x113, b"\x01"); n_ = kb["n"]; e.w32(SV + 0x70, 0)
    raw(3, 1, 0); s10 = tpl(10); node_l = e.r32(SV + 0x70); aud_l = e.uc.mem_read(SV + 0x5C + T, 1)[0]; raw(3, 1, 1)
    e.uc.mem_write(ED + 0x113, b"\x00")
    k_on, k_off = kb["keys"][-2:] if kb["n"] == n_ + 2 else ((0, 0, 9), (0, 0, 9))
    check(kb["n"] == n_ + 2 and k_on[2] == 0 and k_off[2] == 1 and k_on[:2] == k_off[:2], f"17.11.: Slices + Live Rec: Druck und Loslassen an die Firmware (Aufnahme), gleiche Taste {k_on[:2]}")
    check(keynote(*k_on[:2]) == tn0 and k_on[:2] == (5, 2) and keynote(3, 1) != tn0, f"17.11.: aufgenommene Tonhoehe = Vorlage ({keynote(*k_on[:2])} = {tn0}, Taste {k_on[:2]}), nicht die der Pad-Position ({keynote(3, 1)})")
    for i_ in range(28): e.uc.mem_write(0x20001D44 + 4 * i_, struct.pack("<i", 0))
    check(s10 >> 6 == 9 and aud_l == 10 and node_l == 0, f"17.11.: Vorlage traegt Slice 10 (Sample Start {s10}), vorgemerkt {aud_l}, kein eigenes Anspielen (Stimme {node_l:#x})")
    check(e.uc.mem_read(SV + 0x4C + T, 1)[0] == 10, "Live Rec: Pad waehlt den Slice auch (Anzeige)")
    buf = 0x2002F800
    def fmt(v, rel=1):
        r = e.call(nm["sl_format"] | 1, 10, v & 0xFFFFFFFF, rel, buf, 24, 0)
        return e.cstr(buf) if r else None
    e.w32(0x1FFF4518, 1 << T)
    check((fmt(128) or "").startswith("Start ") and fmt(128) == fmt(-64), f"13.11.: Bulk Sample Start zeigt den Feinversatz, keine Slices: {fmt(128)}")
    e.w32(0x1FFF4518, (1 << T) | (1 << ((T + 1) % 8)))
    check(fmt(128) is None, "Spuren gemischt (Slicer + Normal): Original-Anzeige")
    e.w32(0x1FFF4518, 1 << T)
    check(e.call(nm["sl_format"] | 1, 11, 640, 1, buf, 24, 0) == 0, "Bulk Gate: Original-Anzeige (Prozent passt)")
    print("K) 045: Grid-Breite/Move in View → Pad-LEDs sofort neu")
    f.hold(False); f.view(True); e.uc.mem_write(SV + 0xB8 + T, b"\x00")
    r0 = f.cnt["ref"]; f.wait(200); e.call(nm["sv_chg_hook"] | 1, ED, 20, 1)
    check(f.cnt["ref"] > r0, f"Micromove (Grid-Breite): Refresh {f.cnt['ref'] - r0}x")
    r0 = f.cnt["ref"]; e.call(nm["sv_chg_hook"] | 1, ED, 21, 1)
    check(f.cnt["ref"] > r0, "Move: Refresh")
    e.call(nm["sv_chg_hook"] | 1, ED, 21, -1); e.call(nm["sv_chg_hook"] | 1, ED, 20, -1)
    print("L) 045: Fokus Sample-Knob: oben Position, unten Ordner")
    e.stub(0x2D0D8)
    f.touch(2); f.tick()
    txt = f.i0() + " | " + f.i1() + " | " + f.t0() + " | " + f.t1()
    check("Sample " in txt and "/" in txt and "Breaks" in txt, f"Fokus Sample: {txt}")
    print("J) 045: Folder (Sample-Knob unten) in View auf der Slicer-Spur")
    rg = f.d.rig; offs9 = st9 = None
    kind9 = e.uc.mem_read(0xAA114 + 16 * 9, 1)[0]; a9 = f.step + 4 + rg.offs[9]
    def rd9(): return struct.unpack("<h", e.uc.mem_read(a9, 2))[0] if kind9 == 3 else e.uc.mem_read(a9, 1)[0]
    seen = {}
    def h_vorl(em):
        if em.uc.reg_read(UC_ARM_REG_R1) == 15:
            seen["vor"] = rd9()                                   # Vorlage vor dem Ordnersprung
            if kind9 == 3: em.uc.mem_write(a9, struct.pack("<h", 42))
            else: em.uc.mem_write(a9, bytes([42]))               # Attrappe: Firmware springt auf Sample 42
    e.stub(0x2D0D8, cb=h_vorl)
    rg.write_step({9: 7, 10: 2 << 6}); e.uc.mem_write(SV + 0x34 + 2 * T, struct.pack("<h", 13 + 1))   # Spur spielt Sample 13
    e.uc.mem_write(SV + 0x4C + T, bytes([5]))                       # Slice 5 gewaehlt
    f.hold(False); f.view(True)
    e.call(nm["sv_chg_hook"] | 1, ED, 15, 1)
    smp = struct.unpack("<h", e.uc.mem_read(SV + 0x34 + 2 * T, 2))[0] - 1
    check(seen.get("vor") == 13, f"Vorlage vor dem Sprung = Sample der Spur (13): {seen.get('vor')}")
    check(smp == 42, f"Spur uebernimmt das neue Sample: {smp}")
    check(e.uc.mem_read(SV + 0x5C + T, 1)[0] == 5, "Anspielen: gewaehlter Slice 5 statt Slice 1")
    print("M) 045: Encoder waehlt in View den Slice (Seite zuerst, Menue unberuehrt)")
    f.hold(False); f.view(True); e.uc.mem_write(SV + 0xB8 + T, b"\x00")
    SCRX = 0x2002E000; VTX = 0x2002E100; R0 = 0x2002E200; R1 = 0x2002E210
    uc_ = e.uc; uc_.mem_write(R0, bytes.fromhex("00207047")); uc_.mem_write(R1, bytes.fromhex("01207047"))
    uc_.mem_write(VTX, bytes(0x40)); e.w32(SCRX, VTX); e.w32(VTX + 0x28, R0 | 1)
    enc_orig = {"n": 0}; e.stub(0x2A848, cb=lambda em: enc_orig.__setitem__("n", enc_orig["n"] + 1))
    e.w32(S8 + 0x2FC, SCRX); e.uc.mem_write(SV + 0x4C + T, bytes([3]))
    e.call(nm["sv_enc_hook"] | 1, UI, 0, 1); s1 = e.uc.mem_read(SV + 0x4C + T, 1)[0]
    e.call(nm["sv_enc_hook"] | 1, UI, 0, -2 & 0xFFFFFFFF); s2 = e.uc.mem_read(SV + 0x4C + T, 1)[0]
    check(s1 == 4 and s2 == 2 and enc_orig["n"] == 0, f"Encoder: Slice 3 → {s1} → {s2}, Original nicht gerufen")
    e.w32(VTX + 0x28, R1 | 1); e.call(nm["sv_enc_hook"] | 1, UI, 0, 1)
    check(e.uc.mem_read(SV + 0x4C + T, 1)[0] == 2, "Seite nimmt den Encoder selbst: Slice bleibt")
    e.w32(SCRX, 0xA97E0); e.call(nm["sv_enc_hook"] | 1, UI, 0, 1)
    check(enc_orig["n"] == 1 and e.uc.mem_read(SV + 0x4C + T, 1)[0] == 2, "Menue offen: Original")
    e.w32(SCRX, VTX); f.view(False); e.call(nm["sv_enc_hook"] | 1, UI, 0, 1)
    check(enc_orig["n"] == 2, "ausserhalb von View: Original"); f.view(True); e.w32(S8 + 0x2FC, 0)

    print("N) 045: Step halten + Sample Start = Feinversatz des Anfangs")
    dl = {"d": []}
    e.stub(0x3B088, cb=lambda em: dl["d"].append((em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2))))
    f.d.rig.write_step({9: 7, 10: 2 * 64 + 32}); e.w32(0x1FFF450C + 0x7C, f.step); f.hold(True)
    tp = e.call(nm["wv_touched_param"] | 1, 10)
    e.call(nm["sv_chg_hook"] | 1, ED, 10, 64)
    ok1 = dl["d"][-1:] == [(10, (129 - 160) & 0xFFFFFFFF)]
    check(ok1, f"Raste +1 auf 2*64+32 (touched {tp}) → Delta {[(i, d - (1 << 32) if d >> 31 else d) for i, d in dl['d'][-1:]]} (Ziel 129 = Versatz +1)")
    f.hold(False)

    print("N2) 046: Step gehalten + Encoder = Slice dieses Steps (nicht Feinversatz)")
    f.view(True); e.w32(S8 + 0x2FC, 0)
    f.d.rig.write_step({9: 7, 10: 2 * 64 + 32 + 5}); e.w32(0x1FFF450C + 0x7C, f.step)
    e.w32(ED + 0x88, 1 << (16 * (T & 1))) if T < 2 else None   # Step 1 der Spur T gehalten (Bit Step + 16·(T mod 2) in Wort T/2)
    sp_calls = []                                             # Step-Zeiger FUN_0002c37c (sel, Spur, Index): Attrappe → Test-Step
    def h_sp(uc_, a, sz, ud):
        sp_calls.append((uc_.reg_read(UC_ARM_REG_R1), uc_.reg_read(UC_ARM_REG_R2)))
        uc_.reg_write(UC_ARM_REG_R0, f.step); uc_.reg_write(UC_ARM_REG_PC, uc_.reg_read(UC_ARM_REG_LR))
    h_sp_id = e.uc.hook_add(UC_HOOK_CODE, h_sp, begin=0x2C37C, end=0x2C37D)
    n_orig = enc_orig["n"]; dl["d"].clear(); aud0 = e.uc.mem_read(SV + 0x5C + T, 1)[0]
    e.call(nm["sv_enc_hook"] | 1, UI, 0, 1)
    sgn = lambda d: d - (1 << 32) if d >> 31 else d
    d1 = [(i, sgn(d)) for i, d in dl["d"]]; s1 = e.uc.mem_read(SV + 0x4C + T, 1)[0]
    check(sp_calls[:1] == [(T, 0)] and d1 == [(10, -32767), (10, 3 * 64 + 32)] and s1 == 4 and enc_orig["n"] == n_orig,
          f"Slice 3 → 4: Step-Sample-Start {d1} (Slice-Wechsel wie Pad, kein Feinversatz), Auswahl {s1}, Original nicht gerufen")
    dl["d"].clear(); e.call(nm["sv_enc_hook"] | 1, UI, 0, -3 & 0xFFFFFFFF)
    d2 = [(i, sgn(d)) for i, d in dl["d"]]; s2 = e.uc.mem_read(SV + 0x4C + T, 1)[0]
    check(d2 == [(10, -32767), (10, 15 * 64 + 32)] and s2 == 16, f"vom Step-Slice 3 drei zurueck = rund auf 16: {d2}, Auswahl {s2}")
    e.w32(SCRX, 0xA97E0); e.w32(S8 + 0x2FC, SCRX); dl["d"].clear()
    e.call(nm["sv_enc_hook"] | 1, UI, 0, 1)
    check(not dl["d"] and enc_orig["n"] == n_orig + 1, "Menue offen + Step gehalten: Original, Step unveraendert")
    e.w32(SCRX, VTX); e.w32(S8 + 0x2FC, 0); f.view(False); dl["d"].clear()
    e.call(nm["sv_enc_hook"] | 1, UI, 0, 1)
    check(not dl["d"] and enc_orig["n"] == n_orig + 2, "ausserhalb von View + Step gehalten: Original")
    f.view(True); dl["d"].clear()
    rd10 = lambda: struct.unpack("<h", bytes(e.uc.mem_read(f.step + 4 + f.d.rig.offs[10], 2)))[0]
    v0_ = rd10(); e.call(nm["sv_chg_hook"] | 1, ED, 10, 64); v1_ = rd10()
    from PIL import Image as _I
    def brx(fr):                                              # erste Spalte des hellen (gespielten) Bereichs in Zeile 100
        r = fr[100, :, :3].astype(int); import numpy as _np; return int(_np.argmax((r[:, 0] == 28) & (r[:, 1] == 28) & (r[:, 2] == 28)))
    for _ in range(31): e.call(nm["sv_chg_hook"] | 1, ED, 10, (-64) & 0xFFFFFFFF)
    fa = f.tick(); _I.fromarray(fa).save(os.path.join(SCREENS, "06d_view_fine_a.png"))
    for _ in range(20): e.call(nm["sv_chg_hook"] | 1, ED, 10, 64)
    fb = f.tick(); _I.fromarray(fb).save(os.path.join(SCREENS, "06d_view_fine_b.png"))
    check(brx(fb) > brx(fa), f"17.11.: View + Step gehalten: heller Bereich beginnt mit Feinversatz spaeter ({brx(fa)} → {brx(fb)} px)")
    for _ in range(20): e.call(nm["sv_chg_hook"] | 1, ED, 10, (-64) & 0xFFFFFFFF)
    for _ in range(31): e.call(nm["sv_chg_hook"] | 1, ED, 10, 64)
    v0_ = rd10(); dl["d"].clear(); e.call(nm["sv_chg_hook"] | 1, ED, 10, 64); v1_ = rd10()
    check(not dl["d"] and v1_ >> 6 == v0_ >> 6 and (v1_ & 63) == (v0_ & 63) + 1, f"Sample Start bleibt Feinversatz (+1/64 Slice, kein Slice-Sprung, 17.11.: direkt ohne Firmware-Weg): {v0_} → {v1_}, Original {dl['d']}")
    e.w32(ED + 0x88, 0); e.uc.hook_del(h_sp_id)

    print("O) 045: Move bei Beat = Nudge (ms)")
    e.call(nm["sv_cfg_set"] | 1, T, 7, 1)                     # Beat an
    n0 = e.call(nm["sv_cfg_get"] | 1, T, 21)
    e.call(nm["sv_chg_hook"] | 1, ED, 21, 1); e.call(nm["sv_chg_hook"] | 1, ED, 21, 1)
    n1 = e.call(nm["sv_cfg_get"] | 1, T, 21)
    for _ in range(80): e.call(nm["sv_chg_hook"] | 1, ED, 21, 1)
    n2 = e.call(nm["sv_cfg_get"] | 1, T, 21)
    check(n0 == 0 and n1 == 2 and n2 == 50, f"Nudge 0 → {n1} → begrenzt {n2} ms")
    for _ in range(50): e.call(nm["sv_chg_hook"] | 1, ED, 21, -1 & 0xFFFFFFFF)
    e.call(nm["sv_cfg_set"] | 1, T, 7, 0)                     # Beat aus

    print("P) 045: View verlassen → von uns verborgene Anzeigen kommen zurueck")
    f.view(False); f.tick()
    wslot = e.r32(WV + 0x10)
    cand = [k for k in range(50) if k != wslot and not 7 <= k <= 9 and f.d.slot_obj(k)]
    kx = cand[0]; ox = f.d.slot_obj(kx)
    e.w32(ox + 0xC, e.r32(ox + 0xC) | 1); e.w32(ox + 0x20, 100)          # Firmware-Anzeige in der Bildmitte, sichtbar
    f.view(True); f.tick(); f.tick()
    hid_in = not f.d.visible(kx); magic = e.r32(0x1FFFB7C0)
    f.view(False); f.tick()
    check(hid_in and magic == 0x41D00450 and f.d.visible(kx), f"Platz {kx}: in View verborgen ({hid_in}), danach wieder sichtbar ({f.d.visible(kx)})")
    check(e.r32(0x1FFFB7C4) == 0 and e.r32(0x1FFFB7C8) == 0, "Merkliste danach leer")

    print("R) 045: allgemeine Slicer-Einstellung (Vorlage) ueberlebt das Halten eines gesetzten Steps")
    rgv = f.d.rig
    def vparam(i):
        buf_ = 0x2002F900; ok_ = e.call(e.r32(e.r32(f.step) + 0x18), f.step, i, buf_)
        return struct.unpack("<h", e.uc.mem_read(buf_, 2))[0] if ok_ else None
    e.w32(ED + 0x88, 0); e.w32(0x1FFF450C + 0x7C, f.step)
    rgv.write_step({9: 7, 10: 3 * 64 + 32, 11: 0x5000, 13: 1234, 14: 2345}); f.tick()
    e.w32(ED + 0x88, 1 << (16 * T))                      # Step dieser Spur gehalten
    f.tick(); rgv.write_step({9: 7, 10: 9 * 64 + 32, 11: 0x7FFF, 13: 50, 14: 60}); f.tick()   # Firmware: Vorlage = Werte des Steps
    check(vparam(13) == 50, "waehrend des Haltens: Vorlage zeigt die Werte des Steps (wie Firmware)")
    e.w32(ED + 0x88, 0); f.tick()
    check(vparam(13) == 1234 and vparam(14) == 2345 and vparam(11) == 0x5000 and vparam(10) == 3 * 64 + 32,
          f"nach dem Loslassen: allgemeine Einstellung zurueck (Attack {vparam(13)}, Decay {vparam(14)}, Gate {vparam(11)})")
    f.tick(); e.w32(ED + 0x88, 1)                        # Step einer ANDEREN Spur (0) gehalten
    f.tick(); rgv.write_step({9: 7, 10: 0, 13: 77, 14: 88}); f.tick(); e.w32(ED + 0x88, 0); f.tick()
    check(vparam(13) == 77, "Step einer anderen Spur: Firmware-Verhalten bleibt (keine Wiederherstellung)")
    rgv.write_step({9: 7, 10: 2 << 6})
    print("P2) 045: Step-Seite offen → View einrasten → View loesen: Seite nicht leer (Bug vom Geraet)")
    rd = {"n": 0}; e.stub(0x307C4, cb=lambda em: rd.__setitem__("n", rd["n"] + 1))
    FSCR = 0x2002E400; e.w32(FSCR, 0xA0888); e.w32(S8 + 0x2FC, FSCR); e.w32(SV + 0x64, UI)
    f.view(False); f.hold(False); f._show(10, 11, 3 << 6, 0x6000); f.tick()
    ids = [e.r32(VIEW + 0xDC + 4 * i) for i in range(6)]
    vis_before = [k for k in ids if k < 50 and f.d.visible(k)]
    f.view(True); f.tick(); f.tick()
    f.view(False); f.tick()
    vis_after = [k for k in ids if k < 50 and f.d.visible(k)]
    check(e.uc.mem_read(WV + 8, 1)[0] == 0 or e.r32(WV + 0x14) != 1, "Slicer-Seite nach View aus")
    check(len(vis_after) >= 4, f"Felder der Step-Seite wieder sichtbar: vorher {vis_before}, nachher {vis_after} von {ids}")
    check(rd["n"] >= 1, f"Firmware zeichnet ihren Bildschirm neu ({rd['n']}x)")
    e.w32(S8 + 0x2FC, 0)

    print("P2b) 13.11.: Step-Seite Sample Start offen → View einrasten → loesen: unsere Ansicht auf der Seite (nicht die alte)")
    e.w32(S8 + 0x2FC, VIEW); e.w32(SV + 0x64, UI)
    f.view(False); f.hold(False); f._show(10, 11, 3 << 6, 0x6000); f.tick()
    f.view(True); f.tick(); f.tick()
    check(e.r32(WV + 0x14) == 1, "eingerastet: Slicer-Seite")
    e.w32(WV + 0x1C, 8); e.uc.mem_write(WV + 0x0A, b"\x00")       # keine Formatter-Notiz (wie am Geraet nach dem Loesen)
    f.view(False); f.tick()
    check(f.won() and e.r32(WV + 0x14) == VIEW, f"geloest: Ansicht liegt auf der Step-Seite (view {e.r32(WV + 0x14):#x}, an {f.won()})")
    fr = f.tick(); save_png(fr, os.path.join(SCREENS, "16_view_geloest.png"))
    e.w32(S8 + 0x2FC, 0)

    print("P4) 13.11.: ausserhalb von View auf der Slicer-Spur: Sample Start = Feinversatz (nie Slices), Sample = Spur-Sample")
    from emu_slicer import PAT0 as _P0
    from unicorn import UC_HOOK_CODE as _HC
    from unicorn.arm_const import UC_ARM_REG_R0 as _R0, UC_ARM_REG_R1 as _R1, UC_ARM_REG_PC as _PC, UC_ARM_REG_LR as _LR
    POBJ, PCODE = 0x2002D000, 0x2002D100
    sel_ = e.r32(ED + 8); old5c = e.r32(sel_ + 0x5C)
    e.w32(POBJ, POBJ + 0x20); e.w32(POBJ + 0x20, PCODE | 1); e.w32(POBJ + 0x40, POBJ + 0x60); e.w32(POBJ + 0x60, (PCODE + 0x10) | 1)
    e.uc.mem_write(PCODE, bytes.fromhex("7047") * 16)
    cur_t = [0]
    def _trk(uc, a_, s_, u_): cur_t[0] = uc.reg_read(_R1); uc.reg_write(_R0, POBJ + 0x40); uc.reg_write(_PC, uc.reg_read(_LR))
    def _stp(uc, a_, s_, u_): uc.reg_write(_R0, _P0 + 0x10 + cur_t[0] * 0xBA8 + 0x128 + uc.reg_read(_R1) * 42); uc.reg_write(_PC, uc.reg_read(_LR))
    h1_ = e.uc.hook_add(_HC, _trk, begin=PCODE, end=PCODE + 1); h2_ = e.uc.hook_add(_HC, _stp, begin=PCODE + 0x10, end=PCODE + 0x11)
    e.w32(sel_ + 0x5C, POBJ)
    shown = [0]; e.stub(0x2DF80, cb=lambda em: shown.__setitem__(0, shown[0] + 1))
    for i_ in range(64):                                   # alle Steps der Spur als (leere) Audio-Steps anlegen
        a_ = _P0 + 0x10 + T * 0xBA8 + 0x128 + i_ * 42
        if i_ != 5: e.uc.mem_write(a_, bytes(42)); e.w32(a_, 0xAA338)
    f.view(False); f.d.rig.write_step({9: 7, 10: 3 * 64 + 32}); stA = f.d.rig.step
    def p10(): return struct.unpack("<h", bytes(e.uc.mem_read(stA + 4 + f.d.rig.offs[10], 2)))[0]
    hw = ED + 0x88 + 4 * (T >> 1); e.w32(hw, 1 << ((T & 1) * 16 + 5))   # Step 5 der Spur gehalten
    e.call(nm["sv_chg_hook"] | 1, ED, 10, 64)
    check(p10() == 3 * 64 + 1 and shown[0] >= 1, f"Step gehalten: Sample Start +1 Raste = Feinversatz +1, Slice bleibt 3 (Wert {p10()})")
    f.view(True); dl["d"].clear(); e.call(nm["sv_chg_hook"] | 1, ED, 10, 64)
    check(p10() == 3 * 64 + 2 and not dl["d"], f"17.11.: in View + Step gehalten: Feinversatz direkt, ohne Firmware-Weg (kein Anspielen je Raste) (Wert {p10()}, Original {dl['d']})")
    e.w32(0x1FFF450C + 0x7C, stA)
    w10 = e.call(nm["wv_touched_param"] | 1, 10)
    check(w10 == 3 * 64 + 2, f"17.11.: Ansicht liest den Feinversatz des gehaltenen Steps ({w10})")
    e.call(nm["sv_chg_hook"] | 1, ED, 10, (-64) & 0xFFFFFFFF); f.view(False)
    e.w32(hw, 0); e.w32(0x1FFF4518, 1 << T)
    e.call(nm["sv_chg_hook"] | 1, ED, 10, 64); e.call(nm["sv_chg_hook"] | 1, ED, 10, 64)
    check(p10() == 3 * 64 + 3, f"Spur markiert: alle Steps Feinversatz +1 je Raste, Slice bleibt (Wert {p10()})")
    s0 = e.uc.mem_read(SV + 0x34 + 2 * T, 2)
    e.call(nm["sv_chg_hook"] | 1, ED, 9, 1)
    check(e.uc.mem_read(SV + 0x34 + 2 * T, 2) != s0 and p10() == 3 * 64 + 3, "Spur markiert: Sample-Knob wechselt das Spur-Sample, Slices der Steps bleiben")
    e.w32(0x1FFF4518, 0); e.w32(sel_ + 0x5C, old5c); e.uc.hook_del(h1_); e.uc.hook_del(h2_)

    print("P3) 045: View eingerastet, Menue oeffnen → Slicer-Seite ruht, Menue sichtbar; Menue zu → Slicer-Seite zurueck (Bug vom Geraet)")
    f.view(True); f.tick(); f.tick()
    check(f.won(), "eingerastet: Slicer-Seite steht")
    MSCR = 0x2002E600; e.w32(MSCR, 0xA97E0); e.w32(SV + 0x64, UI); rd["n"] = 0
    e.w32(S8 + 0x2FC, MSCR); f.tick(); f.tick()
    check(not f.won() and rd["n"] >= 1, f"Menue offen: Slicer-Seite aus, Firmware zeichnet das Menue neu ({rd['n']}x)")
    e.w32(S8 + 0x2FC, 0); f.tick(); f.tick()
    check(f.won(), "Menue zu: Slicer-Seite wieder da")
    f.view(False); f.tick()

    print("Q) 045: VIEW unten: gruen eingerastet, grau als Hinweis")
    f.view(True); fr = f.tick(); fr = f.tick()
    save_png(fr, os.path.join(SCREENS, "16_view_latched.png"))
    row = fr[186:212, 8:312].astype(int)
    grn = (row[:, :, 0] < 60) & (row[:, :, 1] > 170) & (row[:, :, 2] > 70) & (row[:, :, 2] < 140)
    check(int(grn.sum()) > 30, f"gruenes VIEW in der unteren Zeile ({int(grn.sum())} px)")
    print("S) 17.11.: Spur ohne festes Sample (nach dem Start): Pad und abgespielter Step nehmen dasselbe Sample")
    e.uc.mem_write(SV + 0x34 + 2 * T, struct.pack("<h", 0))              # sample[T] = 0: nichts festgelegt
    f.d.rig.write_step({9: 7, 10: 3 * 64 + 32})                         # gesetzter Step der Spur mit Sample 7
    TPLs = e.r32(0x1FFF450C + 0x7C); e.uc.mem_write(TPLs + 4 + f.d.rig.offs[9], struct.pack("<h", 3)) if TPLs != f.d.rig.step else None
    pad_smp = e.call(nm["wv_track_sample"] | 1, T); pad_smp = pad_smp - (1 << 32) if pad_smp >> 31 else pad_smp
    old_play = e.call(nm["sv_sample"] | 1, T); old_play = old_play - (1 << 32) if old_play >> 31 else old_play
    check(pad_smp == 7 and old_play == -1, f"Fehler nachgestellt: Pad spielt Sample {pad_smp}, Abspielen sah kein Spur-Sample ({old_play}) → Step nahm sein eigenes")
    new_play = e.call(nm["sv_track_sample"] | 1, T)
    smp_set = struct.unpack("<h", bytes(e.uc.mem_read(SV + 0x34 + 2 * T, 2)))[0]
    check(new_play == 7 and smp_set == 8 and e.r32(SV + 0x68) != 0, f"Abspielen nimmt jetzt Sample {new_play} wie das Pad, festgelegt (sample[T] = {smp_set}), wird gespeichert")
    print(f"ERGEBNIS: {checks - fails}/{checks} Pruefungen" + ("" if not fails else f", {fails} FEHLER"))
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
