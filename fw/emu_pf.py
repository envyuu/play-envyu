#!/usr/bin/env python3
"""045 (12.11.): Perform behaelt die Spurauswahl – pf_reset_keep im Emulator auf dem gebauten Image.

Echt: pf_reset_keep, Auswahl-Setter FUN_0002e77c inkl. Step-Liste FUN_0002e000, gepatchte bl an 0x223B6/0x222D4.
Attrappen: Bildschirm der Auswahl (+0x60, nicht aktiv → FUN_0002df80 kehrt zurueck), FUN_0003a974 (Reset der Funktionspads: loescht wie FUN_0002dfb4 Bitmaske + Step-Liste),
FUN_00020bb0 (Fusszeile, nur gezaehlt), Spurliste der Auswahl (+0x5C) mit Spurobjekten (vt+0x38 gezaehlt).
usage: emu_pf.py PolyendPlay_v1.6.0_mod.ppf
"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2  # noqa: E402
import veneer as VN  # 13.11.: Weichen (Sicherheitsnetz) aufloesen
from emu_nis import Emu, load_ppf  # noqa: E402

fails = 0
def check(c, m):
    global fails
    print(("  ok    " if c else "  FEHLER ") + m)
    if not c: fails += 1

def bl_target(img, a):
    h1, h2 = struct.unpack_from("<HH", img, a)
    s = (h1 >> 10) & 1; imm10 = h1 & 0x3FF; j1 = (h2 >> 13) & 1; j2 = (h2 >> 11) & 1; imm11 = h2 & 0x7FF
    i1 = 1 - (j1 ^ s); i2 = 1 - (j2 ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
    if s: off -= 1 << 25
    return a + 4 + off

def main():
    img = load_ppf(sys.argv[1])
    nm = {p[2]: int(p[0], 16) for p in (l.split() for l in subprocess.run(["arm-none-eabi-nm", os.path.join(HERE, "fx.elf")], capture_output=True, text=True, check=True).stdout.splitlines()) if len(p) == 3}
    pf = nm["pf_reset_keep"]
    check(VN.follow(img, bl_target(img, 0x223B6)) == nm["pf_reset_keep_enter"] and VN.follow(img, bl_target(img, 0x222D4)) == pf, "Perform betreten → pf_reset_keep_enter, verlassen → pf_reset_keep")
    e = Emu(img)
    FP, CTL, SEL, LIST, TL, TLVT, TOBJ, TOVT, FN = 0x2002A000, 0x1FFF44E0, 0x1FFF450C, 0x2002A100, 0x2002AA00, 0x2002AA20, 0x2002AA40, 0x2002AA60, 0x2002AB00
    e.uc.mem_write(SEL, bytes(0x130)); e.uc.mem_write(LIST, bytes(0x804))
    e.w32(FP + 4, CTL); e.w32(FP + 8, SEL); e.w32(SEL + 4, LIST)
    e.uc.mem_write(FN, b"\x70\x47" * 8)                     # bx lr (Attrappen)
    e.w32(TL, TLVT); e.w32(TLVT, FN | 1)                    # Spurliste: vt[0](liste, i) → Spurobjekt
    e.w32(TOBJ, TOVT); e.w32(TOVT + 0x38, (FN + 2) | 1)
    st = {"reset": 0, "foot": 0, "on": []}
    e.w32(SEL + 0x5C, TL)
    DISP = 0x2002AC00; e.uc.mem_write(DISP, bytes(0x300)); e.w32(DISP, 1); e.w32(DISP + 0xBF * 4, 2)   # FUN_0002df80: Seite nicht sichtbar → kehrt zurueck
    e.w32(SEL + 0x60, DISP)
    def get_t(em): em.uc.reg_write(UC_ARM_REG_R0, TOBJ)
    e.stub(FN, cb=get_t)
    def t_on(em): st["on"].append(em.uc.reg_read(UC_ARM_REG_R1))
    e.stub(FN + 2, cb=t_on)
    def reset(em):
        st["reset"] += 1
        check(em.uc.reg_read(UC_ARM_REG_R0) == FP, "Original-Reset bekommt die Funktionspads")
        em.w32(SEL + 0xC, 0); em.w32(LIST + 0x800, 0)
    e.stub(0x3A974, cb=reset)
    def foot(em): st["foot"] += 1; check(em.uc.reg_read(UC_ARM_REG_R0) == CTL, "Fusszeile mit SeqController")
    e.stub(0x20BB0, cb=foot)

    print("1) Spuren 2, 5 und MIDI 9 gewaehlt → Perform betreten")
    e.w32(SEL + 0xC, (1 << 2) | (1 << 5) | (1 << 9))
    e.call(pf, FP)
    n = e.r32(LIST + 0x800)
    pairs = {(e.uc.mem_read(LIST + 2 * i, 1)[0], e.uc.mem_read(LIST + 2 * i + 1, 1)[0]) for i in range(n)}
    check(st["reset"] == 1, "Original-Reset lief")
    check(e.r32(SEL + 0xC) == (1 << 2) | (1 << 5) | (1 << 9), f"Bitmaske wiederhergestellt (0x{e.r32(SEL + 0xC):x})")
    check(n == 3 * 64 and pairs == {(t, s) for t in (2, 5, 9) for s in range(64)}, f"Step-Liste: alle 64 Steps der 3 Spuren ({n})")
    check(st["on"] == [1, 1, 1] and st["foot"] == 1, f"Spurobjekte eingeschaltet {st['on']}, Fusszeile 1×")
    print("2) nichts gewaehlt → nur Original")
    st.update(reset=0, foot=0, on=[]); e.w32(SEL + 0xC, 0)
    e.call(pf, FP)
    check(st["reset"] == 1 and e.r32(SEL + 0xC) == 0 and e.r32(LIST + 0x800) == 0 and st["foot"] == 0 and st["on"] == [], "unveraendert wie Original")
    print("3) Bits oberhalb 15 werden nicht wiederhergestellt; kaputter Zeiger → nur Original")
    e.w32(SEL + 0xC, 0xFFFF0001); st.update(reset=0, foot=0, on=[])
    e.call(pf, FP)
    check(e.r32(SEL + 0xC) == 1 and st["on"] == [1], f"nur Spur 0 (0x{e.r32(SEL + 0xC):x})")
    e.w32(FP + 8, 0x12345); st.update(reset=0, foot=0, on=[])
    e.call(pf, FP)
    check(st["reset"] == 1 and st["on"] == [], "Auswahl-Zeiger ungueltig: kein Wiederherstellen")
    print("3a) 12.11.: Menue Keep Selection = Off → Original (Auswahl geloescht)")
    e.w32(FP + 8, SEL); RSA = 0x1FFF9A40
    e.w32(RSA, 0x52534554); e.w32(RSA + 4, 1); e.w32(RSA + 8, 1); e.w32(RSA + 12, 0)
    e.w32(SEL + 0xC, 1 << 4); st.update(reset=0, foot=0, on=[])
    e.call(pf, FP)
    check(st["reset"] == 1 and e.r32(SEL + 0xC) == 0 and st["on"] == [], "Off: Auswahl weg wie im Original")
    e.w32(RSA + 12, 1); e.w32(SEL + 0xC, 1 << 4); st.update(reset=0, foot=0, on=[])
    e.call(pf, FP)
    check(e.r32(SEL + 0xC) == 1 << 4, "On: Auswahl bleibt")
    print("3b) Betreten: nach dem Wiederherstellen alle 32 Funktionspads der aktuellen Seite neu zeichnen (vt+0x30)")
    PAGE, PGVT, DRAW = 0x2002AE00, 0x2002AE40, 0x2002AE80
    e.uc.mem_write(DRAW, b"\x70\x47" * 2); e.w32(PAGE, PGVT); e.w32(PGVT + 0x30, DRAW | 1); e.w32(CTL + 0x24B4, PAGE)
    drawn = []
    e.stub(DRAW, cb=lambda em: drawn.append((em.uc.reg_read(UC_ARM_REG_R0), em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2), em.r32(SEL + 0xC))))
    e.w32(FP + 8, SEL); e.w32(SEL + 0xC, 1 << 3); st.update(reset=0, foot=0, on=[])
    e.call(nm["pf_reset_keep_enter"], FP)
    check(len(drawn) == 32 and {(r, k) for _, r, k, _ in drawn} == {(r, k) for r in range(8) for k in range(4)} and all(p_ == PAGE and m == 8 for p_, _, _, m in drawn),
          f"32 Pads gezeichnet, Auswahl dabei schon wieder gesetzt ({len(drawn)})")
    drawn.clear(); e.w32(SEL + 0xC, 0); e.call(nm["pf_reset_keep_enter"], FP)
    check(drawn == [], "nichts gewaehlt: kein zusaetzliches Zeichnen")
    drawn.clear(); e.w32(SEL + 0xC, 1 << 3); e.call(pf, FP)
    check(drawn == [], "Verlassen: zeichnet nicht (die neue Seite zeichnet danach selbst)")
    blink(img, nm)
    print(f"{'OK' if not fails else 'FEHLER'}: emu_pf {fails} Fehler")
    return fails != 0


def blink(img, nm):
    """Anschlag-Feedback: pf_trigger_hook = MidiTrack_triggerStep + Zeitstempel; pf_poll blinkt das Mute-Pad."""
    print("4) Aufrufer von triggerStep umgebogen, Spur aus ctx")
    for a in (0x49D64, 0x49EAE, 0x49FBE):
        check(VN.follow(img, bl_target(img, a)) == nm["pf_trigger_hook"], f"bl @0x{a:X} → pf_trigger_hook")
    e = Emu(img)
    PA, PB = 0x200025D0, 0x2000E060
    for ctx, want in ((PA + 0x10 + 0x88, 0), (PA + 0x10 + 0xBA8 * 9 + 0x88, 9), (PB + 0x10 + 0xBA8 * 15 + 0x88, 15),
                      (PB + 0x10 + 0xBA8 * 16 + 0x88, -1), (PA + 0x10 + 0x8C, -1), (0x2001F000, -1)):
        got = e.call(nm["pf_track_of"] | 1, ctx)
        got = got - (1 << 32) if got >= 1 << 31 else got
        check(got == want, f"ctx 0x{ctx:08X} → Spur {got} (soll {want})")
    print("5) pf_trigger_hook verhaelt sich wie das Original (resolveEvent/playChord als Attrappen)")
    STEP, EV, VT, PLAY = 0x2002B000, 0x2002B100, 0x2002B200, 0x2002B300
    e.uc.mem_write(PLAY, b"\\x70\\x47" * 4)
    rec = []
    res = {"r": 0, "mute": 0}
    def resolve(em):
        rec.append(("res", em.uc.reg_read(UC_ARM_REG_R0), em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2)))
        em.uc.mem_write(EV + 0x16, bytes([res["mute"]])); em.uc.reg_write(UC_ARM_REG_R0, res["r"])
    e.stub(0x48D00, cb=resolve)
    from unicorn.arm_const import UC_ARM_REG_R3
    def play(em): rec.append(("play", em.uc.reg_read(UC_ARM_REG_R0), em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2), em.uc.reg_read(UC_ARM_REG_R3)))
    e.stub(PLAY, cb=play)
    e.w32(STEP, VT); e.w32(VT + 0x54, PLAY | 1)
    PF = 0x1FFF9F80; MILLIS = 0x20000D7C
    e.call(nm["pf_poll"] | 1)                                     # legt pf_t an (Kaltstart: Muell im RAM)
    check(e.r32(PF) == 0x50463031, "pf_t angelegt")
    ctx = PA + 0x10 + 0xBA8 * 2 + 0x88
    for on, r, mute in ((0, 5, 0), (1, 0, 0), (1, 7, 1), (1, 9, 0)):
        e.uc.mem_write(STEP + 0x29, bytes([0x80 if on else 0x00]))
        res.update(r=r, mute=mute)
        out = []
        for fn in (0x4904C, nm["pf_trigger_hook"]):
            rec.clear(); e.w32(PF + 4 + 8, 0); e.w32(MILLIS, 1234)
            e.call(fn, STEP, ctx, EV)
            out.append((list(rec), e.r32(PF + 4 + 8)))
        marked = out[1][1] == (1234 | 1)
        check(out[0][0] == out[1][0] and marked == bool(on and r), f"Step {'an' if on else 'aus'}, resolve {r}, stumm {mute}: gleiche Aufrufe {out[1][0]}, markiert {marked}")
    print("6) pf_poll: Mute-Pad blinkt in Engine-Farbe, danach zeichnet die Firmware")
    SC = 0x1FFF44E0; PERF, FP2, PADS, PVT, KB = 0x2002C000, 0x2002C100, 0x2002C300, 0x2002C340, 0x2002C400
    e.uc.mem_write(FP2, bytes(0x130)); e.w32(SC + 0x24B4, PERF); e.w32(SC + 0x24D8, PERF); e.w32(SC + 0x24CC, FP2)
    e.w32(FP2 + 0xC, PADS); e.w32(PADS, PVT); e.uc.mem_write(KB, b"\\x70\\x47" * 4); e.w32(PVT + 4, KB | 1)
    leds, redraw, kb = [], [], {"on": 0}
    def led(em): leds.append((em.uc.reg_read(UC_ARM_REG_R1), bytes(em.uc.mem_read(em.uc.reg_read(UC_ARM_REG_R2), 3)).hex()))
    e.stub(KB, cb=led)
    e.stub(0x222EC, cb=lambda em: redraw.append((em.uc.reg_read(UC_ARM_REG_R0), em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2))))
    e.stub(0x230A4, cb=lambda em: em.uc.reg_write(UC_ARM_REG_R0, kb["on"]))
    SL = 0x1FFF9800; e.w32(SL, 0x511CE021)
    for t in range(8): e.w32(SL + 4 + 4 * t, 0)
    e.w32(SL + 4 + 4 * 4, 4)                                       # Spur 4 = Slicer 16
    for t in range(16): e.w32(PF + 4 + 4 * t, 0)
    e.w32(PF + 0x44, 0)
    e.w32(FP2 + 0x11C, 1 << 3)                                     # Spur 3 gemutet
    e.w32(MILLIS, 5000)
    for t in (2, 3, 4): e.w32(PF + 4 + 4 * t, 5000 | 1)
    e.w32(MILLIS, 5010); e.call(nm["pf_poll"] | 1)
    check(sorted(leds) == [(2 * 20 + 16, "6aff5d"), (3 * 20 + 16, "1a4017"), (4 * 20 + 16, "ffff61")],
          f"Audio hell gruen, gemutet gedimmt, Slicer gelb (Mute-Spalte): {sorted(leds)}")
    leds.clear(); e.w32(MILLIS, 5040); e.call(nm["pf_poll"] | 1)
    check(leds == [] and redraw == [], "waehrend des Blinkens nichts neu")
    e.w32(MILLIS, 5075); e.call(nm["pf_poll"] | 1)
    check(sorted(redraw) == [(PERF, 2, 0), (PERF, 3, 0), (PERF, 4, 0)], f"nach 70 ms zeichnet die Firmware die Pads (vt[12]): {sorted(redraw)}")
    print("7) MIDI-Seite, andere Seite, Tastatur")
    leds.clear(); redraw.clear()
    e.uc.mem_write(FP2 + 0x112, b"\\x01"); e.w32(PF + 4 + 4 * 9, 6000 | 1); e.w32(MILLIS, 6005); e.call(nm["pf_poll"] | 1)
    check(leds == [(1 * 20 + 16, "ffb9ff")], f"MIDI-Spur 10 → Reihe 1 rosa: {leds}")
    leds.clear(); e.w32(SC + 0x24B4, 0x2002C800); e.w32(PF + 4 + 4 * 10, 6006 | 1); e.w32(MILLIS, 6010); e.call(nm["pf_poll"] | 1)
    check(leds == [] and e.r32(PF + 0x44) == 0, "andere Seite aktiv: kein Blinken, nichts gemerkt")
    e.w32(SC + 0x24B4, PERF); kb["on"] = 1; e.w32(MILLIS, 6012); e.call(nm["pf_poll"] | 1)
    check(leds == [], "Tastatur ueberlagert: kein Blinken")

if __name__ == "__main__":
    sys.exit(main())
