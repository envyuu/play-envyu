#!/usr/bin/env python3
"""Patch 045: Tap Tempo im Emulator. Echter Code: Hooks aus dem Image (UI-vtable 0xA0160/0xA0164 → sv_knob_hook/sv_touch_hook,
Menue-vtable 0xA97EC → tt_menu_draw), echter Tempo-Setter FUN_00021030 (Hard-Float s0). Attrappen: Original-Zeichnen 0x44638,
Kopfzeilen-Text 0x409E4, FUN_0002bdbc (Tempo an die Engine, hier nur s0 mitlesen), Original-Knob-Handler 0x2A71C/0x2A74C,
Sperr-Objekt *(SeqCtl+4) vt[2].
Aufruf: emu_tap.py PPF"""
import os, struct, sys
import veneer as VN  # 13.11.: Weichen (Sicherheitsnetz) aufloesen
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_S0, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from emu_slicer import Rig, syms, load_ppf

checks = fails = 0
def check(c, m):
    global checks, fails
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)

MILLIS = 0x20000D7C
UI = 0x2002A000; MGR = 0x2002A100; SCR = 0x2002A500; HDR = 0x2002A600; SEQ = 0x2002A700; ST = 0x2002AA00
LOCK = 0x2002AC00; LOCKVT = 0x2002AC10; RET = 0x2002AC40; OTHER = 0x2002AD00
MENU_VT = 0xA97E0

def f2b(f): return struct.unpack("<I", struct.pack("<f", f))[0]
def b2f(b): return struct.unpack("<f", struct.pack("<I", b))[0]

def main():
    img = load_ppf(sys.argv[1]); nm = syms()
    rg = Rig(img, True, nm); e = rg.e; uc = e.uc
    uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20); uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)   # FPU an
    for a in (UI, MGR, SCR, HDR, SEQ, ST, LOCK, OTHER): uc.mem_write(a, bytes(0x100))
    uc.mem_write(ST, bytes(0x200))
    uc.mem_write(RET, bytes.fromhex("7047"))
    e.w32(LOCK, LOCKVT)
    for i in range(4): e.w32(LOCKVT + 4 * i, RET | 1)
    e.w32(UI + 8, MGR); e.w32(UI + 0xC, SEQ); e.w32(MGR + 0x2FC, OTHER); e.w32(OTHER, 0xA0888)
    e.w32(SCR, MENU_VT); e.w32(SCR + 0x10, HDR)
    e.w32(SEQ + 4, LOCK); e.w32(SEQ + 0x1C, ST); e.w32(SEQ + 0x24, 0x2002AE00)
    e.wf(ST + 0x1C8, 120.0)
    log = {"hdr": [], "eng": [], "draw": 0, "touch": 0, "turn": 0}
    log["fw"] = []
    e.stub(0x409E4, cb=lambda em: log["fw"].append(em.cstr(em.uc.reg_read(UC_ARM_REG_R1))))   # 045: Firmware-Text rechts (muss leer sein)
    def _tw(em):                                       # 045: eigene Tempo-Anzeige (tapw.c) → wie frueher als Text protokollieren
        on, x10, arm = em.uc.reg_read(UC_ARM_REG_R0), em.uc.reg_read(UC_ARM_REG_R1), em.uc.reg_read(UC_ARM_REG_R2)
        if not on: log["off"] = log.get("off", 0) + 1; return
        b = f"{x10 // 10}" + (f".{x10 % 10}" if x10 % 10 else "")
        log["hdr"].append(f"TAP {b}" if arm else f"{b} BPM")
    e.stub(nm["tw_set"], cb=_tw)
    e.stub(0x2BDBC, cb=lambda em: log["eng"].append((b2f(em.uc.reg_read(UC_ARM_REG_S0)), em.uc.reg_read(UC_ARM_REG_R0))))
    e.stub(0x44638, cb=lambda em: log.__setitem__("draw", log["draw"] + 1))
    e.stub(0x2A74C, cb=lambda em: log.__setitem__("touch", log["touch"] + 1))
    e.stub(0x2A71C, cb=lambda em: log.__setitem__("turn", log["turn"] + 1))
    TURN = VN.follow(e.img, e.r32(0xA0160)); TOUCH = VN.follow(e.img, e.r32(0xA0164)); DRAW = VN.follow(e.img, e.r32(0xA97EC))
    check(TURN == nm["sv_knob_hook"] | 1 and TOUCH == nm["sv_touch_hook"] | 1 and DRAW == nm["tt_menu_draw"] | 1,
          "Patchstellen: UI vt[2]/[3] → sv_knob_hook/sv_touch_hook, Menue vt+0x0C → tt_menu_draw")
    def ms(t): e.w32(MILLIS, t)
    def touch(k, ev=0): e.call(TOUCH, UI, k, ev)
    def turn(k, d): e.call(TURN, UI, k, d)
    def bpm(): return e.rf(ST + 0x1C8)
    def poll(): e.call(nm["tt_poll"] | 1)

    print("1) Menue zu: alles wie bisher")
    ms(100); touch(5); touch(5, 1); turn(5, 2)
    check(log["touch"] == 2 and log["turn"] == 1 and bpm() == 120.0 and not log["hdr"], "Tempo-Knob geht an die Firmware, kein Tap")

    print("2) Menue auf: Kopfzeile")
    e.w32(MGR + 0x2FC, SCR); e.w32(0x1FFF44E0 + 0x1C, 0); e.call(DRAW, SCR)
    check(log["draw"] == 1 and not log["hdr"], "SeqCtl 0x1FFF44E0 noch leer: Original gezeichnet, Kopfzeile unberuehrt")
    e.w32(0x1FFF44E0 + 0x1C, ST); e.call(DRAW, SCR)        # wie am Geraet: SeqCtl liegt bei 0x1FFF44E0
    check(log["draw"] == 2 and log["hdr"][-1:] == ["120 BPM"], f"vor jedem Knob-Ereignis: Kopfzeile {log['hdr'][-1:]}")
    e.w32(0x1FFFB940 + 0x64, UI); off0 = log.get("off", 0); poll(); poll()
    check(log.get("off", 0) == off0 and e.r32(0x1FFFB600 + 8) == UI, "045: Tempo-Pille bleibt stehen, bevor ein Knob beruehrt wurde (UI aus dem Tasten-Hook)")

    print("3) Tap: erste Beruehrung nur bereit, dann 500 ms = 120 BPM, dann 400 ms")
    t0, n_eng = log["touch"], len(log["eng"])
    ms(1000); touch(5)
    check(log["hdr"][-1] == "TAP 120" and bpm() == 120.0 and len(log["eng"]) == n_eng, f"erste Beruehrung: {log['hdr'][-1]}, Tempo unveraendert")
    touch(5, 1)
    for t in (1500, 2000, 2500, 3000):
        ms(t); touch(5); touch(5, 1)
    check(bpm() == 120.0 and len(log["eng"]) == n_eng, "12.11.: 2. bis 5. Tap setzen noch nichts")
    ms(3500); touch(5); touch(5, 1)
    check(bpm() == 120.0 and log["eng"][-1][0] == 120.0 and log["eng"][-1][1] == 0x2002AE00, f"6. Tap (5 x 500 ms) → {bpm()} BPM, Engine bekommt s0 = {log['eng'][-1]}")
    check(uc.mem_read(ST + 0x1C4, 1)[0] == 1, "Projekt als geaendert markiert (+0x1C4)")
    for t in (3833, 4166, 4499, 4832, 5165): ms(t); touch(5); touch(5, 1)
    check(bpm() == 180.0 and log["hdr"][-1] == "TAP 180", f"Tempowechsel (333 ms, > 30 % schneller = neue Serie) → {bpm()} BPM, Kopfzeile {log['hdr'][-1]}")
    lg = [struct.unpack("<H", uc.mem_read(0x1FFFB600 + 0x58 + 2 * i, 2))[0] for i in range(16)]
    check(e.r32(0x1FFFB600 + 0x54) == 10 and lg[:10] == [500] * 5 + [333] * 5, f"Protokoll der Abstaende: {lg[:10]}")
    ms(5230); touch(5)
    check(bpm() == 180.0, "Prellen (68 ms) ignoriert")
    touch(5, 1)
    check(log["touch"] == t0, "kein Beruehren/Loslassen ging an die Firmware (Menue bleibt offen)")

    print("3b) 045: 16 Taps mit +-20 ms Zittern um 125 BPM (480 ms)")
    import random; rnd = random.Random(7); tt_ = 20000; ms(tt_); touch(5); touch(5, 1)
    for i in range(1, 16):
        tt_ = 20000 + 480 * i + rnd.randint(-20, 20); ms(tt_); touch(5); touch(5, 1)
    check(bpm() == 125.0, f"Ausgleichsgerade: {bpm()} BPM (Mittel der letzten 4 Abstaende waere ungenauer)")
    print("3c) 12.11.: Messung vom Geraet (Metronom 120, Tipp-Streuung 24 ms) – erst ab Tap 6, Hysterese 0,7 BPM")
    real = [475, 488, 458, 540, 510, 482, 513, 488, 484, 507, 489, 510, 516, 543, 479, 458]
    tt_ = 60000; ms(tt_); touch(5); touch(5, 1); seen = []
    n0 = len(log["eng"])
    for i, d in enumerate(real):
        tt_ += d; ms(tt_); touch(5); touch(5, 1); seen.append(bpm())
    sets = [x[0] for x in log["eng"][n0:]]
    check(len(sets) <= 4 and sets[-1] in (120.0, 121.0) and seen[-1] in (120.0, 121.0), f"wenige Spruenge {sets}, am Ende {seen[-1]} (Verlauf {seen})")
    ms(tt_ + 7000); touch(5); touch(5, 1); e.wf(ST + 0x1C8, 180.0)
    print("4) Andere Knobs im Menue, Drehen")
    ms(30000); touch(0); touch(0, 1); turn(0, 3); turn(12, -1)
    check(log["touch"] == t0 and log["turn"] == 1, "Master Volume/andere Knobs: verbraucht, Menue bleibt")
    turn(5, 3)
    check(bpm() == 183.0 and log["hdr"][-1] == "183 BPM", f"Tempo-Knob +3 → {bpm()}, Kopfzeile {log['hdr'][-1]}")
    for k in range(5): turn(5, 100)
    check(bpm() == 400.0, f"Obergrenze 400 ({bpm()})")
    for k in range(10): turn(5, -100)
    check(bpm() == 10.0, f"Untergrenze 10 ({bpm()})")

    check(log["fw"] and all(t == "" for t in log["fw"]), f"045: Firmware-Text rechts bleibt leer ({set(log['fw'])})")
    print("5) Zeit laeuft ab, Tempo von aussen")
    ms(40000); touch(5); touch(5, 1)
    check(log["hdr"][-1] == "TAP 10", "wieder bereit")
    ms(40000 + 2600); poll()
    check(log["hdr"][-1] == "10 BPM", f"nach 2,6 s: {log['hdr'][-1]}")
    e.wf(ST + 0x1C8, 98.5); n = len(log["hdr"]); poll()
    check(len(log["hdr"]) == n + 1 and log["hdr"][-1] == "98.5 BPM", f"Tempo von aussen: {log['hdr'][-1]}")
    poll(); check(len(log["hdr"]) == n + 1, "ohne Aenderung kein neues Schreiben")
    ms(49000); touch(5); ms(49000 + 7000); touch(5)
    check(bpm() == 98.5 and log["hdr"][-1] == "TAP 98.5", "nach 7 s Pause: neu anfangen statt 8,6-BPM-Tap")
    touch(5, 1)

    print("6) Menue zu waehrend Knob beruehrt")
    ms(20000); touch(3)                                   # im Menue gedrueckt
    e.w32(MGR + 0x2FC, OTHER)                             # Menue zu (Back)
    t1 = log["touch"]; touch(3, 1)
    check(log["touch"] == t1, "Loslassen eines im Menue gedrueckten Knobs bleibt verbraucht")
    touch(3); touch(3, 1)
    check(log["touch"] == t1 + 2, "danach wieder normal")
    poll(); check(e.r32(0x1FFFB600 + 0xC) == 0, "Menue zu: TAP aus")

    print("7) Kaltstart-Schutz: Muell im SeqCtl-Zeiger")
    e.w32(MGR + 0x2FC, SCR); e.w32(SEQ + 0x1C, 0xDEADBEEF); n_e = len(log["eng"])
    ms(30000); touch(5); touch(5, 1); ms(30500); touch(5); touch(5, 1); turn(5, 1); e.call(DRAW, SCR)
    check(len(log["eng"]) == n_e, "kein Schreibzugriff mit ungueltigem Zeiger, kein Absturz")

    print("8) 045 Kopfzeile: Engine der gewaehlten Spur statt Projektname")
    O = 0x2002B000; M2 = 0x2002B100; MV = 0x2002B140; F = 0x2002B200; SLB = 0x1FFF9800
    uc.mem_write(O, bytes(0x80)); uc.mem_write(M2, bytes(0x40)); uc.mem_write(MV, bytes(0x40))
    for i in range(3): uc.mem_write(F + 4 * i, bytes.fromhex("7047"))
    e.w32(M2, MV); e.w32(MV + 0x34, (F) | 1); e.w32(MV + 0x08, (F + 4) | 1); e.w32(MV + 0x0C, (F + 8) | 1)
    e.w32(O + 4, M2); e.w32(O + 0x28, 0x77); uc.mem_write(O + 0x60, b"\x01")
    hl = {"txt": [], "orig": 0, "upd": 0}
    e.stub(F, cb=lambda em: hl["txt"].append(em.cstr(em.uc.reg_read(UC_ARM_REG_R2)) if em.uc.reg_read(UC_ARM_REG_R1) == 0x77 else "falsches Widget"))
    e.stub(F + 4, cb=lambda em: hl.__setitem__("upd", hl["upd"] + 1))
    def h_orig(em):
        hl["orig"] += 1; em.uc.mem_write(O + 0x61, b"\x00")      # Original: nach dem Neuschreiben Merker loeschen
    e.stub(0x29F7C, cb=h_orig)
    HOOK = None
    w0, w1 = struct.unpack_from("<HH", bytes(e.img[0x30840:0x30844]))
    S_ = (w0 >> 10) & 1; J1 = (w1 >> 13) & 1; J2 = (w1 >> 11) & 1
    off = (S_ << 24) | ((1 - (J1 ^ S_)) << 23) | ((1 - (J2 ^ S_)) << 22) | ((w0 & 0x3FF) << 12) | ((w1 & 0x7FF) << 1)
    if S_: off -= 1 << 25
    HOOK = VN.follow(e.img, 0x30844 + off)
    check(HOOK == nm["hdr_hook"], f"Patchstelle 0x30840: bl hdr_hook ({HOOK:#x})")
    def run(): e.call(HOOK | 1, O)
    for t in range(8): e.w32(SLB + 4 + 4 * t, 0)
    e.w32(SLB + 4 + 4 * 2, 4); e.w32(SLB + 4 + 4 * 5, 2)        # Spur 3 Slicer 16, Spur 6 Slicer 8
    e.w32(0x1FFF4518, 0); run()
    check(hl["orig"] == 1 and hl["txt"] == [], "nichts gewaehlt: nur Original")
    e.w32(0x1FFF4518, 1 << 2); run()
    check(hl["txt"][-1:] == ["T3 Slicer 16"] and hl["upd"] >= 1, f"Spur 3 gewaehlt: {hl['txt'][-1:]}")
    n = len(hl["txt"]); run(); run()
    check(len(hl["txt"]) == n, "unveraendert: kein Neuschreiben")
    e.w32(0x1FFF4518, (1 << 2) | (1 << 5) | (1 << 0)); run()
    check(hl["txt"][-1] == "Slicer T3 T6", f"mehrere: {hl['txt'][-1]}")
    uc.mem_write(O + 0x62, b"\x01"); n = len(hl["txt"]); e.w32(0x1FFF4518, 1 << 5); run()
    check(len(hl["txt"]) == n, "Meldung steht (+0x62): Kopfzeile nicht anfassen")
    uc.mem_write(O + 0x62, b"\x00"); run()
    check(hl["txt"][-1] == "T6 Slicer 8", f"danach: {hl['txt'][-1]}")
    uc.mem_write(O + 0x61, b"\x01"); n = len(hl["txt"]); run()
    check(len(hl["txt"]) == n + 1 and hl["txt"][-1] == "T6 Slicer 8", "Firmware schreibt neu (+0x61): wieder ueberschreiben")
    e.w32(0x1FFF4518, 1 << 0); run()
    check(uc.mem_read(O + 0x61, 1)[0] == 1, "Normal-Spur gewaehlt: Merker +0x61 → Projektname kommt zurueck")
    run(); n = len(hl["txt"]); run()
    check(len(hl["txt"]) == n and uc.mem_read(O + 0x61, 1)[0] == 0, "danach Ruhe")
    uc.mem_write(O + 0x60, b"\x00"); e.w32(0x1FFF4518, 1 << 2); n = len(hl["txt"]); run()
    check(len(hl["txt"]) == n, "Statuszeile aus (+0x60 = 0): nichts")

    print(f"ERGEBNIS: {checks - fails}/{checks} Pruefungen ok" + (f", {fails} FEHLER" if fails else ""))
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
