#!/usr/bin/env python3
'''Patch 034: GEMELDETER BUG im ECHTEN Firmware-Code: Attack-Knob in View mit gewaehlter Spur. Echt: Ui_onKnobTurn, SeqCtl_knobTurn,
SeqKnob_turn, sv_chg_hook, FUN_0002d0d8, FUN_0002cf94, Wertebereich-Objekte, Standard-Step, sv_poll, Display. Attrappen nur: Anzeige der Step-Seite.'''
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from emu_display import Disp, syms, load_ppf, SDRAM, WV, ARGS, RECS, VIEW
from emu_slicer import PAT0
import numpy as np
checks = fails = 0
def check(c, m):
    global checks, fails
    checks += 1
    if not c: fails += 1; print('  FEHLER', m)
    else: print('  ok   ', m)
ED = 0x20024000; UI = 0x20024400; S8 = 0x20024800; SEQ = 0x20025000; SCR = 0x20028000; SEL = 0x1FFF450C; SL = 0x1FFF9800; T = 1
BUF = 0x20029000; DEFSTEP = 0x2002A000
def main():
    nm = syms(); img = load_ppf(sys.argv[1]); d = Disp(img, True, nm); e = d.e
    e.heap = max(e.heap, 0x2002B000)   # 045: Heap hinter die festen Testobjekte (SCR 0x20028000, BUF, DEFSTEP); sv_x_t ist groesser geworden
    frames = 88200; sig = np.zeros(frames)
    for k in range(16):
        s0 = k * frames // 16; n = frames // 16; sig[s0:s0 + n] = np.exp(-np.arange(n) / 1200) * np.sin(np.arange(n) * 0.05)
    e.uc.mem_write(SDRAM, np.clip(sig * 30000, -32767, 32767).astype('<i2').tobytes())
    d.set_sample(7, SDRAM, frames); d.rig.track = T; d.rig.step = PAT0 + 0x10 + T * 0xBA8 + 0x128 + 42 * 5
    d.rig.set_mode(T, 4)
    d.rig.write_step({9: 7, 10: 0, 13: 1770})                # Step der Spur: Attack 17,7 % wie am Geraet
    # Standard-Step: echtes Audio-Step-Objekt mit allen Bits, Attack 0
    e.uc.mem_write(DEFSTEP, bytes(e.uc.mem_read(d.rig.step, 42)))
    keep = d.rig.step; d.rig.step = DEFSTEP; d.rig.write_step({9: 7, 10: 0, 13: 0, 14: 0, 6: 60, 2: 0}); d.rig.step = keep
    for a in (0x20AC0, 0x210E8, 0x1F180, 0x2073C, 0x208C0, 0x2141C, 0x20B34, 0x20F54): e.stub(a)
    orig = []; e.stub(0x3B088, cb=lambda _: orig.append(1))
    shown = []
    e.stub(0x2FF2C, cb=lambda _: shown.append(1))             # StepParamView_showParam (Anzeige, gesondert getestet)
    e.stub(0x2CF5C, ret=0)                                    # MIDI-CC-Versand
    e.call(0x22035)
    e.w32(0x1FFF6994, ED); e.w32(ED, 0xA5A54); e.w32(ED + 4, 0x20024C00); e.w32(ED + 8, SEL); e.uc.mem_write(ED + 0x18, b'\x01')
    e.w32(UI + 8, S8); e.w32(S8 + 0x2FC, 0); e.w32(UI + 0xC, SEQ); e.w32(SEQ + 0x24B4, ED); e.w32(SEQ + 0x24B8, 0x1FFF7918); e.w32(SEQ + 0xC, SCR)
    e.w32(SCR, VIEW); e.w32(SCR + 4, VIEW + 4); e.w32(SCR + 0xBF * 4, VIEW); e.w32(SCR + 0xC0 * 4, VIEW)
    # Auswahl wie am Geraet: Spur gewaehlt, Mehrfach-Zaehler gesetzt
    e.w32(SEL + 4, BUF); e.w32(BUF + 0x800, 1); e.w32(SEL + 0xC, 1 << T); e.w32(SEL + 0x7C, DEFSTEP); e.w32(SEL + 0x5C, 0)
    e.uc.mem_write(SEL + 0x84, b'\x00'); e.w32(SEL + 0x130, 0x1FFF7918); e.w32(SEL + 0x60, ARGS + 0x400); e.w32(ARGS + 0x400, VIEW)
    MVT = 0x20029F00; e.w32(SEL + 0x64, MVT - 0x40); e.w32(MVT - 0x40, MVT); e.w32(MVT + 8, 0xFFE91); e.stub(0xFFE90, ret=0)
    e.w32(0x20000D7C, 100000)
    e.w32(SL + 0xAC + 4 * T, d.rig.node)
    off = struct.unpack('<22B', bytes(e.uc.mem_read(0x20019DC4, 22)))
    def val(step, i): return struct.unpack('<h', e.uc.mem_read(step + 4 + off[i], 2))[0]
    def tick():
        e.call(nm['sv_poll'] | 1)
        for _ in range(300):
            e.call(nm['wv_poll'] | 1)
            if e.r32(WV + 0x0C) == 0: break
        d.pump(); return d.eve.frames[-1]
    def envp(fr):
        p = fr[66:182, 8:312]; return int(((p[:, :, 0] == 0x9A) & (p[:, :, 1] == 0x9A) & (p[:, :, 2] == 0x9A)).sum())
    def knob(k, dl=1): e.call(0x2A71D if False else nm['sv_knob_hook'] | 1, UI, k, dl & 0xFFFFFFFF)
    # GEMELDET (035): Pad, bevor ein Step beruehrt oder die Spur gespielt wurde: genau der Slice, nicht das ganze Sample
    kbplay = []; e.stub(0x23334, cb=lambda _: kbplay.append(1))
    tr = PAT0 + 0x10 + T * 0xBA8
    e.w32(tr + 0x88, d.rig.node); e.w32(tr + 0x8C, 0x20020300); e.w32(tr + 0x90, T)
    e.w32(0x20020300, 0x20020380); e.w32(0x20020380 + 0x10, 0xFFE81); e.stub(0xFFE80, ret=0x200203C0)
    e.w32(SL + 0xAC + 4 * T, 0)                              # sl.node unbekannt: Spur seit dem Start nicht gespielt
    s_ = d.rig.node + 0xD8
    n = 16; kk = 5
    e.call(nm['sv_kb_hook'] | 1, ED, 1 + kk // 4, kk % 4, 0)
    st, en, on = e.r32(s_ + 0x28), e.r32(s_ + 0x2C), e.uc.mem_read(s_ + 0x18, 1)[0]
    es, ee = frames * (kk * 0x7FFF // n) // 0x7FFF, frames * ((kk + 1) * 0x7FFF // n) // 0x7FFF
    check(on == 1 and not kbplay, f'erstes Pad: Stimme direkt ausgeloest, kein Umweg ueber die Tastatur (spielt {on}, Umweg {len(kbplay)})')
    check((st, en) == (es, ee), f'erstes Pad: genau Slice 6 ({st}, {en}), erwartet ({es}, {ee}), ganzes Sample waere (0, {frames})')
    e.call(nm['sv_kb_hook'] | 1, ED, 1 + kk // 4, kk % 4, 1)
    from klick_mess import render
    check(e.uc.mem_read(s_ + 0x2C, 1) is not None, 'Loslassen ohne Absturz')
    render(d.rig, 60); check(e.uc.mem_read(s_ + 0x18, 1)[0] == 0, 'One-Shot: Stimme am Slice-Ende aus')
    # Leerer Standard-Step (fruehester Zustand): Pad darf nicht stumm sein, Volume bleibt
    vt0 = e.r32(d.rig.node); sl0 = e.call(e.r32(vt0 + 0x18), d.rig.node, 0)
    v0 = struct.unpack('<h', e.uc.mem_read(sl0 + 2, 2))[0]; check(v0 != 0, f'Volume nach leerem Standard-Step unveraendert ({v0})')
    # GEMELDET (037): Pad loslassen mitten im Ton, im echten Render-Pfad mit konstantem Signal
    e.uc.mem_write(SDRAM + 0x200000, struct.pack('<h', 20000) * 88200); d.set_sample(7, SDRAM + 0x200000, 88200)
    d.rig.sample = (SDRAM + 0x200000, 88200, 44100)
    kp = 2
    full0 = {0: 7600, 1: 0, 2: 0, 3: 0, 4: 16, 5: 0, 6: 60, 7: 0, 8: 0, 9: 7, 10: 0, 11: 0x7FFF, 12: 0, 13: 0, 14: 0}   # Standard-Step wie am Geraet
    keep = d.rig.step; d.rig.step = DEFSTEP; d.rig.write_step(full0); d.rig.step = keep
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 0)
    x = [v for b in render(d.rig, 6) for v in b]; full = max(abs(v) for v in x)
    # 038: One-Shot: Loslassen aendert nichts, der Slice spielt bis zum Ende, ohne Kante
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 1)
    check(e.uc.mem_read(s_ + 0x18, 1)[0] == 1, 'Pad loslassen: Slice spielt weiter')
    y = [v for b in render(d.rig, 60) for v in b]; z = x[-1:] + y
    lastnz = max((i for i, v in enumerate(y) if v), default=0)
    jump = max(abs(z[i + 1] - z[i]) for i in range(len(z) - 1))
    slice_len = 88200 // 16
    check(abs((lastnz + 6 * 128) - slice_len) <= 2 * 128 + 8, f'spielt bis zum Slice-Ende ({lastnz + 6 * 128} von {slice_len} Frames)')
    check(full > 5000 and jump <= full * 0.08, f'am Ende keine harte Kante (Pegel {full}, Sprung {jump})')
    check(e.uc.mem_read(s_ + 0x18, 1)[0] == 0, 'Stimme danach aus')
    # Neues Pad, waehrend der alte Slice noch spielt
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 0); x = [v for b in render(d.rig, 8) for v in b]
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 1)
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp + 1, 0); y = [v for b in render(d.rig, 6) for v in b]; z = x[-1:] + y
    check(max(abs(z[i + 1] - z[i]) for i in range(len(z) - 1)) <= full * 0.08, 'neues Pad schneidet den alten Slice ohne Kante')
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp + 1, 1); render(d.rig, 60)
    # 038: Play-Marker im Bild (Gelb 0xFFD64D), echter Render-Pfad
    ms = e.r32(0x20000D7C)
    def marker(fr):
        row = fr[70, 8:312]; xs = [i for i in range(304) if tuple(row[i]) == (0xFF, 0xD6, 0x4D)]
        return xs[0] if xs else None
    f0 = tick(); check(marker(f0) is None, 'kein Marker, solange nichts spielt')
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 0)
    xs = []
    for i in range(6):
        render(d.rig, 6); ms += 21; e.w32(0x20000D7C, ms); fr = tick(); xs.append(marker(fr))
    print('   Marker-Pixel:', xs)
    check(all(v is not None for v in xs) and xs == sorted(xs) and xs[-1] > xs[0], f'Marker laeuft mit dem Ton: {xs}')
    k0 = xs[0]; exp = 304 * (kp * 88200 // 16 + 6 * 128) // 88200
    check(abs(k0 - exp) <= 2, f'Marker sitzt an der Stelle des Tons (Pixel {k0}, erwartet {exp})')
    # Rate begrenzt: innerhalb von 20 ms kein neues Bild
    n0 = len(d.eve.frames); render(d.rig, 6); e.call(nm['wv_poll'] | 1); d.pump()
    check(len(d.eve.frames) == n0, f'hoechstens alle 20 ms neu ({len(d.eve.frames) - n0} Bilder)')
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 1); render(d.rig, 60); ms += 21; e.w32(0x20000D7C, ms)
    fe = tick(); check(marker(fe) is None, 'Marker verschwindet am Slice-Ende')
    # Kosten je Marker-Bild im schlimmsten Fall: 64 Slices + Huellkurve
    from unicorn import UC_HOOK_CODE
    d.rig.set_mode(T, 8); keep2 = d.rig.step; d.rig.step = DEFSTEP; d.rig.write_step({**full0, 13: 3000, 14: 4000}); d.rig.step = keep2
    e.w32(SL + 4 + 4 * T, 8); ms += 30; e.w32(0x20000D7C, ms); tick()
    e.call(nm['sv_kb_hook'] | 1, ED, 1, 0, 0); render(d.rig, 1); ms += 30; e.w32(0x20000D7C, ms)
    cn = [0]; hk = e.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s__, u: cn.__setitem__(0, cn[0] + 1))
    e.call(nm['sv_poll'] | 1); e.call(nm['wv_poll'] | 1); stps = d.pump(); e.uc.hook_del(hk)
    words = len(d.eve.last_dl)
    print(f'   Marker-Bild, schlimmster Fall: {cn[0]} Befehle, Display-Liste {words} Worte ({words * 4} Byte)')
    check(words * 4 < 7000, f'Display-Liste im Budget ({words * 4} Byte von 8192)')
    check(cn[0] < 180000, f'Bild kostet < 1 ms Rechenzeit bei 180 MHz ({cn[0]} Befehle)')
    d.rig.set_mode(T, 4); keep2 = d.rig.step; d.rig.step = DEFSTEP; d.rig.write_step(full0); d.rig.step = keep2
    e.call(nm['sv_kb_hook'] | 1, ED, 1, 0, 1); render(d.rig, 60)
    # Hoehere Note: gleiche Kante
    keep = d.rig.step; d.rig.step = DEFSTEP; d.rig.write_step({**full0, 6: 84}); d.rig.step = keep
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 0); x = [v for b in render(d.rig, 3) for v in b]
    e.call(nm['sv_kb_hook'] | 1, ED, 1, kp, 1); y = [v for b in render(d.rig, 40) for v in b]; z = x[-1:] + y
    check(max(abs(z[i + 1] - z[i]) for i in range(len(z) - 1)) <= full * 0.08, 'hohe Note: Ende ohne harte Kante')
    d.set_sample(7, SDRAM, frames); d.rig.sample = (SDRAM, frames, 44100)
    keep = d.rig.step; d.rig.step = DEFSTEP; d.rig.write_step({**full0, 13: 1500, 6: 72, 2: 3000}); d.rig.step = keep
    e.call(nm['sv_kb_hook'] | 1, ED, 1, 0, 0)
    vt_ = e.r32(d.rig.node); slot = lambda i: e.call(e.r32(vt_ + 0x18), d.rig.node, i)
    src1 = lambda i: struct.unpack('<h', e.uc.mem_read(slot(i) + 2, 2))[0]
    check(src1(10) == 1500 and src1(1) == 3000, f'erstes Pad: Attack/Cutoff der Vorlage anliegend ({src1(10)}, {src1(1)})')
    check(src1(7) == 7, f'erstes Pad: Sample des Steps ({src1(7)})')
    e.call(nm['sv_kb_hook'] | 1, ED, 1, 0, 1)
    d.rig.step = DEFSTEP; d.rig.write_step({9: 7, 10: 0, 13: 0, 14: 0, 6: 60, 2: 0}); d.rig.step = keep
    fr0 = tick(); check(envp(fr0) == 0, 'Start: Attack 0, keine Kurve')
    a0 = val(d.rig.step, 13)
    for _ in range(30): knob(12)
    check(not orig, 'Original-Verteiler 0x3B088 nicht benutzt')
    check(val(DEFSTEP, 13) == 300, f'gewaehlte Spur + Mehrfach-Zaehler: Standard-Step Attack 0 → {val(DEFSTEP, 13)} (Raster 10)')
    check(val(d.rig.step, 13) == a0, f'vorhandener Step unberuehrt ({val(d.rig.step, 13)})')
    check(e.r32(SEL + 0xC) == 1 << T, 'Spurauswahl danach wieder da')
    check(len(shown) == 30, f'Anzeige je Raste ({len(shown)})')
    for _ in range(200): knob(12)
    fr1 = tick(); check(envp(fr1) > 40, f'Bild: Kurve erscheint OHNE Step/Pad (13.11.: nur am gewaehlten Slice) ({envp(fr1)} Pixel)')
    knob(12, -1); check(val(DEFSTEP, 13) == 2290, f'Knob runter: {val(DEFSTEP, 13)}')
    for _ in range(100): knob(12, -1)
    fr2 = tick()
    check(not np.array_equal(fr1, fr2), 'Bild folgt jeder Aenderung')
    for i, k in ((14, None),): pass
    e.w32(VIEW + 200, 1); knob(12); check(val(DEFSTEP, 14) == 10, f'Fokus unten: Decay {val(DEFSTEP, 14)}'); e.w32(VIEW + 200, 0)
    knob(1); nv = e.uc.mem_read(DEFSTEP + 4 + off[6], 1)[0]; check(nv == 61, f'Note {nv}')
    knob(11); check(val(DEFSTEP, 2) == 100, f'Cutoff {val(DEFSTEP, 2)}')
    print(f'{checks} Pruefungen, {fails} Fehler'); sys.exit(1 if fails else 0)
if __name__ == '__main__': main()
