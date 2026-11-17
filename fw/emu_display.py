#!/usr/bin/env python3
"""Display-Nachweis fuer Patch 023 im Cortex-M4-Emulator (unicorn) + FT81x-Modell (eve_emu.py).

Ausgefuehrt wird ECHTER Firmware-Code:
  * Grafik-Init FUN_00003FF8 (Panel-Init, EVE-Init FUN_00000450, Fonts/Bilder aus dem Flash ins RAM_G, Startbild),
  * Widget-Manager (vtable 0x9AA1C: create/invalidate/…, Bildaufbau FUN_00003C90),
  * Step-Parameter-Seite (Klasse 0xA0888): Widgets anlegen (vt[1] 0x2F0A0), FUN_0002FD54 (Formatter, Fortschritt)
    → 0x2FEC8 bl FUN_0002F36C (in 023: wv_view_hook) → FUN_0002F1AC,
  * Patch 023: wv_view_hook, wv_poll (Waveform-Spalten berechnen, Bitmap ins RAM_G), eigenes Widget wv_draw,
  * echte Spur-Stimme + Step (Rig aus emu_slicer.py), ausgeloest ueber MidiTrack_triggerStep.
Attrappen: SPI-Objekt (Byte → eve_emu), CS-Pin (FUN_00000748/770), digitalWrite/pinMode/delay, Heap (Bump-Zeiger),
die vier Hilfsobjekte der Seite (Kopf-/Fusszeile, FUN_0004098C/0002A068/00040AC8/00029CDC) sind leer.

Ergebnis: PNGs in screens/ und Pixelpruefungen.
usage: emu_display.py PolyendPlay_v1.6.0_waveform.ppf ../stock/firmware.bin
"""
import sys, os, struct, math
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_LR

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import veneer as VN  # 13.11.: Weichen (Sicherheitsnetz) aufloesen
from emu_nis import load_ppf  # noqa: E402
from emu_slicer import Rig, syms, SL  # noqa: E402
import eve_emu  # noqa: E402
from eve_emu import Eve, save_png  # noqa: E402
import numpy as np  # noqa: E402

SPI_OBJ = 0x20038000
SPI_VT = 0x20038010
SPI_FN = 0x000FFE00          # unbenutzter Flash: 4 × "bx lr"
MGR = 0x1FFF42CC             # Widget-Manager an seiner Geraete-Adresse HAL 0x1FFF3740 + 0xB8C (lrec.c benutzt sie fest)
VIEW = 0x20038400            # Step-Parameter-Seite (Klasse 0xA0888, 0x124 Byte)
THEME = 0x20038800           # Kopie der Farbtabelle 0xA42EC (0x170 Byte; im Geraet Bildschirm-Container +0x184)
ARGS = 0x20038C00
SLIST = 0x1FFF69BC           # Sample-Liste App 0x1FFF44A8 + 0x2514 (FUN_0001A1C8); +4 = 255 Eintraege × 0x38
RECS = 0x20039000            # Attrappe der Eintraege
WV = 0x1FFF9B00
SDRAM = 0x70000000
P_TAB = 0x1FFF7918
SCREENS = os.path.join(HERE, "screens")

fails = 0; checks = 0
IN_GRAN = False   # 045: Abschnitte 5/5b nur zaehlen, wenn Granular im Build aktiv ist (WITH_GRANULAR)
GRAN_OFF = os.environ.get("WITH_GRANULAR") is None

def check(c, m):
    global fails, checks
    if IN_GRAN and GRAN_OFF:
        print("  AUS   ", m, "(Granular ausgeblendet)"); return
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


class Disp:
    def __init__(self, img, patched, nm, track=0):
        self.rig = rig = Rig(img, patched, nm, track=track)
        self.e = e = rig.e
        self.nm = nm; self.patched = patched
        e.uc.mem_map(SDRAM, 0x400000)
        self.eve = eve = Eve()
        uc = e.uc
        from unicorn.arm_const import UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC
        uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20); uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)   # FPU an (CPACR)
        uc.mem_write(SPI_FN, bytes.fromhex("7047" * 8))
        e.w32(SPI_OBJ, SPI_VT)
        for i in range(5): e.w32(SPI_VT + 4 * i, (SPI_FN + 4 * min(i, 3)) | 1)
        def h_xfer(uc_, a, s, ud):
            uc.reg_write(UC_ARM_REG_R0, eve.xfer(uc.reg_read(UC_ARM_REG_R1)))
        uc.hook_add(UC_HOOK_CODE, h_xfer, begin=SPI_FN + 12, end=SPI_FN + 13)
        e.stub(0x748, cb=lambda _: eve.cs_low())
        e.stub(0x770, cb=lambda _: eve.cs_high())
        for a in (0x138C0, 0x13930, 0x139EC, 0x56F6C): e.stub(a)   # digitalWrite, pinMode, delay, Heap_free
        for a in (0x4098C, 0x2A068, 0x40AC8, 0x29CDC): e.stub(a)   # Kopf-/Fusszeilen-Objekte der Seite
        # ---- Widget-Manager wie im HAL-Konstruktor FUN_0000BA7C (HAL+0xB8C … +0xC14)
        uc.mem_write(MGR, bytes(0x100))
        slots = e.call(0x30C0, 200); qu = e.call(0x30C0, 0xCC)
        e.w32(MGR, 0x9AA1C); e.w32(MGR + 8, slots); e.w32(MGR + 0x1C, qu)
        e.w32(MGR + 0x3C, 0x80); e.w32(MGR + 0x40, 0x80); e.w32(MGR + 0x44, 1); e.w32(MGR + 0x48, 1)
        e.call(0x1288, MGR + 0x4C)                                 # "IPS TFT"-Objekt
        pins = ARGS; uc.mem_write(pins, bytes([10, 9, 20, 21, 22, 0, 0, 0]))
        text = ARGS + 0x20; uc.mem_write(text, b"emu 023\0")
        self.call6(0x3FF8, MGR, 1, pins, SPI_OBJ, text, 0)
        self.boot = eve.frames[-1] if eve.frames else None
        # ---- Step-Parameter-Seite
        uc.mem_write(VIEW, bytes(0x124)); e.w32(VIEW, 0xA0888)
        for k in range(0x37, 0x3D): e.w32(VIEW + 4 * k, 0xFFFFFFFF)
        uc.mem_write(VIEW + 0x104, b"\xff\xff")
        uc.mem_write(THEME, bytes(img[0xA42EC:0xA42EC + 0x170]))
        e.w32(VIEW + 4, MGR); e.w32(VIEW + 0x24, THEME)
        for k, a in enumerate((0x10, 0x14, 0x18, 0x1C)): e.w32(VIEW + a, 0x20038F00 + 16 * k)
        e.w32(VIEW + 0x10C, P_TAB)
        e.call(0x2F0A1, VIEW)                                      # vt[1]: Widgets anlegen
        uc.mem_write(VIEW + 0x28, b"\x01")                         # Seite aktiv
        self.pump()
        uc.mem_write(RECS, bytes(255 * 0x38)); e.w32(SLIST + 4, RECS)

    def set_sample(self, idx, addr, frames, rate=44100.0):
        """Eintrag idx der Sample-Liste: {Vektor begin/end/cap (1 Datenzeiger), Frames, Rate (float), Name +0x14}"""
        e = self.e; r = RECS + 0x38 * idx; vec = RECS + 255 * 0x38 + 16 * idx
        e.w32(vec, addr); e.w32(r, vec); e.w32(r + 4, vec + 4); e.w32(r + 8, vec + 4)
        e.w32(r + 0xC, frames); e.wf(r + 0x10, rate); e.uc.mem_write(r + 0x14, b"BREAK01\0")
        self.rig.sample = (addr, frames, int(rate)); self.sidx = idx

    def call6(self, addr, a0, a1, a2, a3, a4, a5, limit=60_000_000):
        e = self.e
        e.uc.mem_write(e.sp, struct.pack("<II", a4 & 0xFFFFFFFF, a5 & 0xFFFFFFFF))
        return e.call(addr, a0, a1, a2, a3, limit=limit)

    def pump(self, n=400):
        """Hauptschleife des Widget-Managers: FUN_00003C90 bis die Warteschlange leer ist."""
        e = self.e
        f0 = len(self.eve.frames)
        for _ in range(n):
            e.call(0x3C91, MGR)
            if e.uc.mem_read(MGR + 0x14, 1)[0] == 0 and e.r32(MGR + 0x20) == e.r32(MGR + 0x24): break
        return len(self.eve.frames) - f0

    def show(self, i0, i1, v0, v1, have=(1, 1), rel=(0, 0), step=True, extra=None):
        e = self.e
        a = ARGS + 0x40
        if step:                                                   # Step im Pattern-Speicher (echte vtable 0xAA338), Sample 0
            vals = {9: self.sidx, i0: v0, i1: v1}; vals.update(extra or {}); self.rig.write_step(vals)
        e.uc.mem_write(a, bytes([i0, i1]) + struct.pack("<hh", v0, v1) + bytes(have) + bytes(rel))
        self.call6(0x2FD55, VIEW, a, a + 2, a + 6, a + 8, self.rig.step if step else 0)
        if self.patched and "wv_poll" in self.nm:
            for _ in range(200):
                e.call(self.nm["wv_poll"] | 1)
                if e.r32(WV + 0x0C) == 0: break                   # nichts mehr zu tun
        self.pump()
        return self.eve.frames[-1]

    def slot_obj(self, k): return self.e.r32(self.e.r32(MGR + 8) + 4 * k)
    def visible(self, k):
        o = self.slot_obj(k); return bool(o) and bool(self.e.r32(o + 0xC) & 1)


def main():
    ppf, orig = sys.argv[1], sys.argv[2]
    os.makedirs(SCREENS, exist_ok=True)
    nm = syms()
    img = load_ppf(ppf)
    oimg = bytearray(open(orig, "rb").read())

    p22 = os.path.join(HERE, "../022_usbin_slicer/PolyendPlay_v1.6.0_usbin_slicer.ppf")
    if os.path.exists(p22):
        print("0) Patchstellen gegenueber 022 (auf dem Geraet)")
        i22 = load_ppf(p22)
        diff = [a for a in range(0, 0xB069C) if img[a] != i22[a]]
        new = list(range(0x2FEC8, 0x2FECC)) + list(range(0xA015C, 0xA0160))
        for a24 in (0x1E0D2, 0x1E206, 0x1DFF4, 0x41A92, 0xA99D4): new += list(range(a24, a24 + 4))   # 026: Stellen von 024
        for a26 in (0x3B77E, 0xA5A70, 0xA0160, 0xA0164, 0xA5A68, 0x3A75A, 0x3A784): new += list(range(a26, a26 + 4))   # 026: View-Slicer
        new += list(range(0xA97EC, 0xA97F0)) + list(range(0x30840, 0x30844)) + list(range(0xA0168, 0xA016C))                                                                      # 045: Tap Tempo
        for a45 in (0x52C4, 0x6E20, 0x6F28, 0x6FAE, 0x711E, 0x7176, 0x722E, 0x4EBC): new += list(range(a45, a45 + 4))   # 045: Color Mode
        new += [0x3894E, 0x38DE6]                                          # 045: Randomize-Typgrenze 5 → 8
        for a45 in (0x3891C, 0x38AFC, 0x38C0C, 0x38D64, 0x38E70, 0x38B60, 0x38D5C, 0x21B26): new += list(range(a45, a45 + 4))   # 045 (11.11.): Audio-Liste, rz_*
        new += [0x38950, 0x38DE8]                                          # 045 (11.11.): Audio-Typgrenze 9 → 16
        for a45 in (0x223B6, 0x222D4, 0x49D64, 0x49EAE, 0x49FBE, 0xA5A80): new += list(range(a45, a45 + 4))   # 045 (12.11.): Perform behaelt die Spurauswahl
        new += [0x9BAD4]                                                   # 046: Laenge Konfig.-Deskriptor 357 → 365 (IAD Audio)
        new += list(range(0x405C, 0x4060))                                 # 046: Startbild (Gfx_init-Endsprung → bs_boot)
        new += list(range(0xA05FC, 0xA0600))                               # 17.11.: Ladebildschirm (vtable 0xA05F8 [1] → ld_create)
        rest = sorted({a & ~3 for a in diff if a not in new})
        def tgt(im, a):                                # Zeiger (|1) oder Thumb-BL-Ziel
            w = struct.unpack_from("<I", im, a)[0]
            if 0xB069C <= (w & ~1) < 0x100000: return True   # 12.11.: Code reicht ueber 0xC0000 (Grenze war Fehlalarm)
            h1, h2 = struct.unpack_from("<HH", im, a)
            if h1 >> 11 == 0x1E and h2 >> 14 == 3:
                S = (h1 >> 10) & 1; J1 = (h2 >> 13) & 1; J2 = (h2 >> 11) & 1
                off = (S << 24) | ((1 - (J1 ^ S)) << 23) | ((1 - (J2 ^ S)) << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
                if S: off -= 1 << 25
                return 0xB069C <= a + 4 + off < 0x100000
            return False
        moved = all(any(tgt(img, x) and tgt(i22, x) for x in (a, a - 2, a + 2)) for a in rest)   # BL auch an 2-Byte-Grenzen
        check(all(img[w:w + 4] != i22[w:w + 4] for w in (0x2FEC8, 0xA015C, 0x1E0D2, 0x1E206, 0x1DFF4, 0x41A92, 0xA99D4)) and moved,
              f"neu nur 0x2FEC8, 0xA015C (023) und die 5 Stellen von 024; sonst {len(rest)} Zeiger in den angehaengten Code (verschoben): {[hex(a) for a in rest]}")
        from check_ppf import images
        check(images(ppf)[1] == images(p22)[1], "Rev-B-Image unveraendert")
    print("1) Grafik-Init (echter Code FUN_00003FF8) im Original-Image")
    d0 = Disp(oimg, False, {})
    eve = d0.eve
    check(eve.r32(0x302000) == 0x7C and d0.boot is not None, "REG_ID 0x7C gelesen, Startbild per DLSWAP/CMD_SWAP gerendert")
    check(sorted(eve.fonts) == [8, 9, 10, 11, 12], f"CMD_SETFONT2 fuer Handles {sorted(eve.fonts)}")
    fm = {f: (eve.r32(p + 140), eve.r32(p + 136)) for f, (p, _) in eve.fonts.items()}
    check(fm[8] == (22, 16) and fm[11] == (54, 36), f"Fontgroessen (Hoehe, Breite): {fm}")
    save_png(d0.boot, os.path.join(SCREENS, "00_boot_original.png"))
    check(int((d0.boot.max(axis=2) > 60).sum()) > 2000, "Startbild: Logo/Text sichtbar")

    print("2) Step-Parameter-Seite im Original: Sample Start / Sample End (Spur 1 Normal)")
    d0.e.uc.mem_write(SDRAM, bytes(88200 * 2)); d0.set_sample(3, SDRAM, 88200)
    fr = d0.show(10, 11, 0x4000, 0x6000)
    save_png(fr, os.path.join(SCREENS, "01_step_sample_start_original.png"))
    ids = [d0.e.r32(VIEW + 0xDC + 4 * i) for i in range(6)]
    check(all(i < 50 for i in ids) and all(d0.visible(i) for i in ids), f"6 Widgets (Name/Wert/Balken ×2) angelegt + sichtbar: Slots {ids}")
    txt = [c for c in eve.cmd_log if c[0] == 0x0C]
    tl = [c[5] for c in txt[-4:]]
    check("Sample Start" in tl and "Sample End" in tl and "1000/2000 ms" in tl and "1500/2000 ms" in tl, f"CMD_TEXT der Seite (Original-Formatter mit Sample-Liste): {tl}")
    check(int((fr[30:50, 10:150].max(axis=2) > 100).sum()) > 50, "Pixel: Name oben links")
    check(int((fr[100:112, 7:307].max(axis=2) > 10).sum()) > 300, "Pixel: Fortschrittsbalken Block 1")
    orig_frame = fr.copy(); orig_dl = eve.last_dl[:]
    print(f"   Display-Liste {len(orig_dl)} Worte, Primitive {eve.stats['prims']}")

    print("3) 045: Spur 1 Normal: Waveform mit Start/Ende (vorher Original-Seite)")
    d1 = Disp(img, True, nm)
    sigN = np.zeros(88200)
    for k_ in range(8): sigN[k_ * 11025:(k_ + 1) * 11025] = np.exp(-np.arange(11025) / 1500) * np.sin(np.arange(11025) * 0.07)
    d1.e.uc.mem_write(SDRAM, np.clip(sigN * 30000, -32767, 32767).astype("<i2").tobytes()); d1.set_sample(3, SDRAM, 88200)
    fr1 = d1.show(10, 11, 0x4000, 0x6000)
    save_png(fr1, os.path.join(SCREENS, "03n_normal_start_end.png"))
    WX_, WY_, WW_, WH_ = 8, 64, 304, 120
    lumN = fr1[WY_:WY_ + WH_, WX_:WX_ + WW_].astype(int).max(axis=2)
    xa_, xb_ = WW_ * 0x4000 // 0x7FFF, WW_ * 0x6000 // 0x7FFF
    inN = lumN[:, xa_ + 3:xb_ - 3]; outN = np.concatenate([lumN[:, :xa_ - 3], lumN[:, xb_ + 3:]], 1)
    check(not np.array_equal(fr1, orig_frame) and int((inN > 200).sum()) > 100, f"Normal-Spur: Waveform, gespielter Bereich hell ({int((inN > 200).sum())} Px)")
    check(int((outN > 200).sum()) < 30 and int((outN > 60).sum()) > 300, "Normal-Spur: ausserhalb Start…Ende gedimmt")
    print("3b) 045: Spur 1 Normal, Attack/Decay-Seite: Waveform mit Huellkurve (Formatter 13/14 → fmt_ad)")
    fr1b = d1.show(13, 14, 2000, 3000, extra={10: 0x2000, 11: 0x7000})
    save_png(fr1b, os.path.join(SCREENS, "03o_normal_attack_decay.png"))
    try:
        FT_ = SDRAM + 0x100000; d1.e.uc.mem_write(FT_, bytes(0x1000))           # Ordnertabelle (SampleList+8, Namen +0x404 je 17 B)
        for i_, n_ in enumerate([b"Drums", b"Loops", b"Vox"]): d1.e.uc.mem_write(FT_ + 0x404 + 17 * i_, n_ + b"\0")
        d1.e.w32(SLIST + 8, FT_)
        fr1c = d1.show(9, 15, 3, 0, extra={10: 0x2000, 11: 0x7000})
        save_png(fr1c, os.path.join(SCREENS, "03p_normal_sample_folder.png"))
        lumC = fr1c[WY_:WY_ + WH_, WX_:WX_ + WW_].astype(int).max(axis=2)
        check(int((lumC > 60).sum()) > 300, f"Sample/Folder-Seite zeigt die Waveform ({int((lumC > 60).sum())} Px)")
    except Exception as ex_:
        check(False, f"Sample/Folder-Seite: {ex_}")
    lumB = fr1b[WY_:WY_ + WH_, WX_:WX_ + WW_].astype(int).max(axis=2)
    check(d1.e.r32(0x1FFFB700 + 0x70) == 0xF, "Formatter 13/14/9/15 umgebogen (fad_t.ok)")
    check(int((lumB > 200).sum()) > 100, f"Attack/Decay-Seite zeigt die Waveform ({int((lumB > 200).sum())} Px hell)")
    x0_, x1_ = WW_ * 0x2000 // 0x7FFF, WW_ * 0x7000 // 0x7FFF; xa_ = x0_ + (x1_ - x0_) * 20 // 100
    boxB = fr1b[WY_:WY_ + WH_, WX_:WX_ + WW_].astype(int)
    envp = ((abs(boxB[:, :, 0] - 0x9A) < 8) & (abs(boxB[:, :, 2] - 0x9A) < 8))   # 13.11.: Umriss 0x9A9A9A
    check(int(envp[5:WH_ - 5, x0_ + 3:xa_ - 3].sum()) > 20, f"Huellkurve: Attack-Rampe ueber 20 % des gespielten Teils ({int(envp[5:WH_ - 5, x0_ + 3:xa_ - 3].sum())} Px)")
    check(d1.e.r32(WV) == 0x57A7E023, "wv_t initialisiert (magic)")

    print("3d) 13.11.: Spur gewaehlt (Bulk, Werte relativ) – Ansicht erscheint auf normaler und Slicer-Spur, Texte relativ")
    try:
        d1.e.w32(0x1FFF4518, 1); d1.e.w32(0x1FFF450C + 0x7C, d1.rig.step)
        for mode_, want_ in ((0, "Start +0.0 %"), (4, "Start +0%")):
            d1.rig.set_mode(0, mode_)
            d1.rig.write_step({9: d1.sidx, 10: 0x1000, 11: 0x6000, 13: 2000, 14: 3000})
            frf = d1.show(10, 11, 0, 0, rel=(1, 1), step=False)
            tf = [c[5] for c in d1.eve.cmd_log if c[0] == 0x0C][-6:]
            lumF = frf[WY_:WY_ + WH_, WX_:WX_ + WW_].astype(int).max(axis=2)
            check(d1.e.uc.mem_read(WV + 8, 1)[0] == 1 and want_ in tf and int((lumF > 200).sum()) > 100,
                  f"Spur gewaehlt, {'Slicer' if mode_ else 'normal'}: Waveform + Text {want_!r} ({tf})")
            save_png(frf, os.path.join(SCREENS, f"03r_spur_gewaehlt_{'slicer' if mode_ else 'normal'}.png"))
        d1.e.w32(0x1FFF4518, 3); frf = d1.show(10, 11, 0, 0, rel=(1, 1), step=False)
        check(d1.e.uc.mem_read(WV + 8, 1)[0] == 1, "13.11.: zwei Spuren gewaehlt: Ansicht der ersten Spur (Nutzerwunsch)")
        d1.rig.set_mode(0, 4); d1.e.w32(0x1FFF4518, 1)
        d1.rig.write_step({9: d1.sidx, 10: 0x1000, 11: 0x6000, 13: 2000, 14: 3000})
        frl = d1.show(13, 14, 2000, 3000, extra={10: 0x1000, 11: 0x6000})
        tl = [c[5] for c in d1.eve.cmd_log if c[0] == 0x0C][-6:]
        check(any(x.startswith("Attack ") for x in tl) and any(x.startswith("Decay ") for x in tl), f"13.11.: Slicer zeigt Bezeichnungen vor den Werten ({tl})")
        save_png(frl, os.path.join(SCREENS, "03s_slicer_attack_decay.png"))
        d1.e.w32(0x1FFF4518, 0); d1.rig.set_mode(0, 0)
    except Exception as ex_:
        check(False, f"Spur gewaehlt: {ex_}")
    print("3c) 12.11.: Slicer-Spur, Sample/Folder-Seite OHNE Anheften: Waveform mit Slices wie bei normalen Spuren")
    try:
        d1.rig.set_mode(0, 4)
        fr1d = d1.show(9, 15, 3, 0, extra={10: 0x2000, 11: 0x7000})
        save_png(fr1d, os.path.join(SCREENS, "03q_slicer_sample_folder.png"))
        lumD = fr1d[WY_:WY_ + WH_, WX_:WX_ + WW_].astype(int).max(axis=2)
        check(int((lumD > 60).sum()) > 300 and d1.e.uc.mem_read(WV + 8, 1)[0] == 1, f"Slicer: Sample/Folder-Seite zeigt die Waveform ({int((lumD > 60).sum())} Px)")
        d1.rig.set_mode(0, 0)
    except Exception as ex_:
        check(False, f"Slicer Sample/Folder-Seite: {ex_}")
    print("4) Patch 023, Spur 1 = Slicer 16, Breakbeat-artiges Sample im SDRAM")
    frames = 88200
    rnd = np.random.RandomState(5)
    sig = np.zeros(frames)
    for k in range(16):                                    # 16 Anschlaege mit abklingender Huellkurve, verschieden laut
        s0 = k * frames // 16; n = frames // 16
        env = np.exp(-np.arange(n) / (900 + 300 * (k % 3))) * (0.25 + 0.75 * ((k * 7) % 5) / 4)
        sig[s0:s0 + n] = env * np.sin(np.arange(n) * (0.05 + 0.02 * (k % 4))) + rnd.randn(n) * env * 0.2
    data = np.clip(sig * 30000, -32767, 32767).astype("<i2")
    d1.e.uc.mem_write(SDRAM, data.tobytes())
    d1.set_sample(7, SDRAM, frames)
    d1.rig.set_mode(0, 4)
    v10 = 2 << 6; gate = 0x5FFF                            # Slice 3/16, Gate 75 %
    fr2 = d1.show(10, 11, v10, gate)
    save_png(fr2, os.path.join(SCREENS, "02_slicer16_slice3_gate75.png"))
    e = d1.e
    on = e.uc.mem_read(WV + 0x08, 1)[0]
    slot = e.r32(WV + 0x10)
    check(on == 1 and slot < 50 and d1.visible(slot), f"Waveform-Modus an, eigenes Widget in Slot {slot} sichtbar")
    ids = [e.r32(VIEW + 0xDC + 4 * i) for i in range(6)]
    check(not any(d1.visible(i) for i in ids[2:]) and not d1.visible(ids[1]), "Original-Wert-/Balken-Widgets und Name 2 verborgen")
    tl = [c[5] for c in eve_cmds(d1, 0x0C)[-6:]]
    check("Slice 3/16" in tl and "Gate 75%" in tl, f"Texte im eigenen Widget: {tl}")
    # Geometrie (muss zu wave.c passen)
    WX, WY, WW, WH = 8, 64, 304, 120
    box = fr2[WY:WY + WH, WX:WX + WW].astype(int)
    lum = box.max(axis=2)
    xs0 = WW * 2 // 16; xs1 = WW * 3 // 16
    inside = lum[:, xs0 + 2:xs1 - 2]; outside = np.concatenate([lum[:, :xs0 - 2], lum[:, xs1 + 2:]], 1)
    check(int((inside > 200).sum()) > 100, f"Pixel: Slice 3 hell (Waveform weiss), {int((inside > 200).sum())} Px")
    check(int((outside > 200).sum()) < 30 and int((outside > 60).sum()) > 300, "Pixel: andere Slices gedimmt (grau)")
    gx = xs0 + (xs1 - xs0) * gate // 0x7FFF
    col = fr2[WY + 4:WY + WH - 4, WX + gx - 1:WX + gx + 2].astype(int)
    check(int(((col[:, :, 0] > 200) & (col[:, :, 1] < 140)).sum()) > 40, f"Pixel: Gate-Marke (Akzentfarbe) bei x={WX + gx}")
    # Huellkurve: Spalte mit Anschlag hoch, Ende des Slices niedrig
    hcol = lambda x: int((lum[:, x] > 60).sum())
    check(hcol(xs0 + 1) > 3 * max(1, hcol(xs1 - 3)), f"Pixel: Anschlag am Slice-Anfang ({hcol(xs0 + 1)} px) > Ausklang ({hcol(xs1 - 3)} px)")
    top = lum[2]                                           # oberste Zeile der Box: dort keine Waveform
    exp = [k * WW // 16 for k in range(1, 16)]
    hit = [x for x in exp if top[x] >= 0x40]
    gaps = [x for x in range(4, WW - 4) if all(abs(x - y) > 2 for y in exp) and not (xs0 <= x <= xs1) and top[x] >= 0x40]
    check(len(hit) == 15 and not gaps, f"Pixel: 15 Slice-Grenzen genau bei k·304/16 ({len(hit)} gefunden, {len(gaps)} Fremdspalten)")
    mydl = e.r32(e.r32(e.r32(MGR + 8) + 4 * slot) + 8)
    print(f"   Display-Liste gesamt {len(d1.eve.last_dl)} Worte, eigenes Widget {mydl} Byte, Original-Seite {len(orig_dl)} Worte")
    check(0 < mydl < 0x1000 and len(d1.eve.last_dl) * 4 < 0x2000, "eigene Liste < 4 KB, gesamte Display-Liste < 8 KB (Grenze des Bildaufbaus)")

    global IN_GRAN; IN_GRAN = True
    print("5) Granular (Spur 1 = Granular): Position 40 %, Grain 250 ms")
    d1.rig.set_mode(0, 9)
    gl = int(round(math.sqrt((250 - 5) / 995.0) * 0x7FFF))
    fr3 = d1.show(10, 11, int(0.4 * 0x7FFF), gl)
    save_png(fr3, os.path.join(SCREENS, "03_granular_pos40_grain250.png"))
    tl = [c[5] for c in eve_cmds(d1, 0x0C)[-6:]]
    check(any(t.startswith("Grain 2") and t.endswith("ms") for t in tl), f"Texte Granular: {tl}")
    lum = fr3[WY:WY + WH, WX:WX + WW].max(axis=2)
    px = int(WW * 0.4)
    check(int((lum[:, px - 1:px + 2] > 200).sum()) > 60, f"Pixel: Positionsmarke bei x={WX + px}")

    print("5b) Granular mit Spray 60 % und Window 50 %")
    _cp = d1.e.call(nm["gr_cfg"] | 1, 0); d1.e.uc.mem_write(_cp + 1, bytes([50, 60]))   # 039: Window, Spray je Spur
    fr3b = d1.show(10, 11, int(0.55 * 0x7FFF), int(round(math.sqrt((120 - 5) / 995.0) * 0x7FFF)))
    save_png(fr3b, os.path.join(SCREENS, "03b_granular_spray.png"))
    lum = fr3b[WY:WY + WH, WX:WX + WW].astype(int)
    px = int(WW * 0.55); sw = 6000 * 22050 // 10000 * WW // frames
    sp = lum[3, px - sw + 2:px - 2]
    nsp = sum(1 for v in sp if abs(int(v[0]) - 0x1E) <= 2); nbg = sum(1 for v in lum[3, 4:px - sw - 4] if int(v[0]) <= 2)
    check(len(sp) > 10 and nsp >= 0.6 * len(sp) and nbg > 0.6 * (px - sw - 8), f"Pixel: Spray-Band links der Position ({sw} px, {nsp}/{len(sp)} Px 0x1E, davor kein grauer Kasten)")

    IN_GRAN = False
    print("6) Zurueck auf einen normalen Parameter (Volume/Pan): Waveform aus, Original-Widgets wieder da")
    fr4 = d1.show(0, 1, 0x1000, 0)
    on = e.uc.mem_read(WV + 0x08, 1)[0]
    check(on == 0 and not d1.visible(slot), "Waveform-Modus aus, eigenes Widget verborgen")
    check(all(d1.visible(i) for i in ids), "alle 6 Original-Widgets wieder sichtbar")
    save_png(fr4, os.path.join(SCREENS, "04_back_to_volume.png"))

    print("7) Seite wird abgebaut (vt[2] 0x2F14C): wv_poll verbirgt das eigene Widget")
    d1.show(10, 11, v10, gate)
    check(e.uc.mem_read(WV + 0x08, 1)[0] == 1, "  (wieder an)")
    e.call(0x2F14D, VIEW)
    e.call(nm["wv_poll"] | 1)
    d1.pump()
    check(e.uc.mem_read(WV + 0x08, 1)[0] == 0 and not d1.visible(slot), "nach Abbau der Seite: aus + verborgen")
    print("8) Live Rec (lrec.c): ohne usbrec.c (023 allein) bleibt jede Taste Original")
    calls = []
    def h_btn(uc_, a, s_, ud):
        calls.append((e.uc.reg_read(UC_ARM_REG_R1), e.uc.reg_read(UC_ARM_REG_R2)))
        e.uc.reg_write(UC_ARM_REG_R0, 1); e.uc.reg_write(UC_ARM_REG_PC, e.uc.reg_read(UC_ARM_REG_LR))
    UI = 0x20038E00; e.uc.mem_write(UI, bytes(0x40))
    hk = struct.unpack("<I", bytes(img[0xA015C:0xA0160]))[0]
    check(VN.follow(img, hk) == nm["lr_button_hook"] | 1, "vtable 0xA015C → lr_button_hook")
    e.uc.hook_add(UC_HOOK_CODE, h_btn, begin=0x2A774, end=0x2A775)
    MILLIS = 0x20000D7C
    def btn(b, ev, t, dev=None):
        dv = dev or d1
        dv.e.w32(MILLIS, t); dv.e.call(hk, UI, b, ev)
    seq = [(7, 0, 1000), (7, 2, 1401), (7, 2, 1701), (7, 2, 1901), (7, 2, 2051), (7, 1, 2100), (3, 0, 2200), (3, 1, 2300)]
    if "rec_request" not in nm:                        # 023 allein
        for b_, ev, t in seq: btn(b_, ev, t)
        check(calls == [(b_, ev) for b_, ev, _ in seq], f"alle {len(seq)} Ereignisse sofort und unveraendert weitergegeben")
        e.call(nm["lr_poll"] | 1)
        check(e.r32(0x1FFF9EB0 + 0x20) == 0xFFFFFFFF, "keine Statusanzeige (rec_status fehlt)")
    else:                                              # 026: echtes usbrec.c im Image
        print("   (026: echtes usbrec.c – Langdruck startet die echte Aufnahme-Anfrage)")
        e.call(nm["rec_init"]); e.w32(0x1FFF9000, 0x55A0D120); e.w32(0x1FFF9004, 1)   # Variante "Stereo + In"
        for b_, ev, t in seq: btn(b_, ev, t)
        st = e.r32(0x1FFF9A80 + 4)
        check(st == 1 and calls == [(3, 0), (3, 1)], f"Langdruck Live Rec → echte Aufnahme angefragt (Zustand {st}), nichts weitergegeben; Taste 3 normal ({calls})")
        calls.clear()
        for b_, ev, t in [(7, 0, 5000), (7, 1, 5200)]: btn(b_, ev, t)
        check(calls == [(7, 0), (7, 1)], f"kurzer Druck: Live Rec beim Loslassen weitergegeben ({calls})")
        e.call(nm["lr_poll"] | 1)
        calls.clear()

    print("9) Live Rec mit Attrappe von usbrec.c (fx_rectest.bin, nur Test): lang = USB-Aufnahme, kurz = Live Rec")
    timg = bytearray(img); tb = open(os.path.join(HERE, "fx_rectest.bin"), "rb").read()
    timg[0xB069C:0xB069C + len(tb)] = tb
    tnm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(HERE, "fx_rectest.elf")) if len(l.split()) == 3}
    d2 = Disp(timg, True, tnm); e2 = d2.e
    hk = tnm["lr_button_hook"] | 1                  # 026: Adresse im Test-Binary (Linkreihenfolge weicht ab)
    e2.uc.mem_write(SDRAM, data.tobytes()); d2.set_sample(7, SDRAM, frames); d2.rig.set_mode(0, 4)
    d2.show(10, 11, v10, gate)
    calls.clear(); e2.uc.hook_add(UC_HOOK_CODE, h_btn, begin=0x2A774, end=0x2A775)
    e = e2   # h_btn liest Register von e
    RS = 0x1FFF9F60; e2.uc.mem_write(RS, bytes(28)); e2.uc.mem_write(UI, bytes(0x40))
    for b_, ev, t in [(7, 0, 5000), (7, 2, 5401), (7, 2, 5701), (7, 2, 5901), (7, 2, 6051)]: btn(b_, ev, t, d2)
    check(calls == [] and e2.r32(RS + 20) == 1 and e2.r32(RS + 24) == 1, f"lang (1051 ms): rec_request(1) einmal, nichts weitergegeben ({calls})")
    btn(7, 2, 6131, d2); btn(7, 1, 6200, d2)
    check(calls == [] and e2.r32(RS + 20) == 1, "Loslassen nach Langdruck geschluckt")
    e2.w32(RS + 4, 37)
    e2.call(tnm["lr_poll"] | 1); d2.pump()
    lslot = e2.r32(0x1FFF9EB0 + 0x20)
    tl = [c[5] for c in d2.eve.cmd_log if c[0] == 0x0C][-5:]
    xs = [c[1] for c in d2.eve.cmd_log if c[0] == 0x0C][-4:]
    check(lslot < 50 and d2.visible(lslot) and tl == ["REC", "0", ":", "0", "3"], f"Statusanzeige in Slot {lslot}: {tl}")
    check(xs[2] - xs[0] == xs[3] - xs[2] + 4 or (xs[3] - xs[2] == 9 and xs[2] - xs[1] == 7), f"045: Zeit in festen Feldern (x {xs})")
    from PIL import Image as _I
    _I.fromarray(d2.eve.frames[-1]).save(os.path.join(SCREENS, "05b_rec_pille.png"))
    print("5c) 045: Tempo-Pille im Menue (feste Ziffernfelder, Punkt beim Tippen)")
    # wie am Geraet (PEEK 10.11.): sichtbarer Kopfzeilen-Balken auf einem niedrigeren Platz als die Pille (liegt im Stapel darueber)
    MGRA = e2.r32(0x1FFF42CC + 8); kbar = next(k for k in range(50) if e2.r32(MGRA + 4 * k) == 0)
    BAR = 0x2002E800; NOPF = 0x2002E7F0; e2.uc.mem_write(NOPF, bytes([0x00, 0x20, 0x70, 0x47]))   # movs r0,#0; bx lr
    e2.uc.mem_write(BAR, bytes(0x60)); e2.uc.mem_write(BAR + 0x40, b"".join(((NOPF | 1)).to_bytes(4, "little") for _ in range(10)))
    e2.w32(BAR, BAR + 0x40); e2.w32(BAR + 0xC, 0x6001); e2.w32(BAR + 0x1C, 2); e2.w32(BAR + 0x20, 0xFFFFFFFA); e2.w32(BAR + 0x24, 0x13C); e2.w32(BAR + 0x28, 0x1D)
    e2.w32(MGRA + 4 * kbar, BAR)
    pos = {}
    for x10, arm in ((1200, 1), (980, 0), (1110, 1)):
        n0 = len(d2.eve.cmd_log)
        e2.call(tnm["tw_set"] | 1, 1, x10, arm); d2.pump()
        tx = [(c[1], c[5]) for c in d2.eve.cmd_log[n0:] if c[0] == 0x0C]
        pos[x10] = tx
        _I.fromarray(d2.eve.frames[-1]).save(os.path.join(SCREENS, f"05c_tempo_{x10 // 10}{'_tap' if arm else ''}.png"))
    xb = {k: [x for x, t in v if t == "BPM"] for k, v in pos.items()}
    x1s = {k: [x for x, t in v if t == "1"] for k, v in pos.items()}
    check(all(xb[k] == xb[1200] for k in xb) and xb[1200], f"BPM steht immer an derselben Stelle: {xb}")
    check([t for x, t in pos[980]] == ["9", "8", "BPM"] and pos[980][0][0] == pos[1200][1][0], f"98: Ziffern im 2./3. Feld wie bei 120 ({pos[980]})")
    check(x1s[1110] and x1s[1110][0] == pos[1200][0][0], f"111: die schmale 1 sitzt im selben Feld wie bei 120 ({pos[1110]})")
    twslot = e2.r32(0x1FFFB870 + 0x68)
    check(kbar < twslot and not (e2.r32(BAR + 0xC) & 1), f"Kopfzeilen-Balken (Platz {kbar}, ueber der Pille auf {twslot}) ausgeblendet, solange die Pille steht")
    e2.w32(BAR + 0xC, 0x6001); e2.call(tnm["tw_keep"] | 1)
    check(not (e2.r32(BAR + 0xC) & 1), "Firmware zeigt den Balken wieder → tw_keep blendet ihn erneut aus")
    e2.call(tnm["tw_set"] | 1, 0, 0, 0); d2.pump()
    check(not d2.visible(twslot) and not (e2.r32(BAR + 0xC) & 1) and e2.r32(0x1FFFB870 + 0x7C) == 2,
          "Menue zu: Tempo-Pille verborgen, Balken bleibt aus, solange die REC-Pille steht")
    fr5 = d2.eve.frames[-1]
    save_png(fr5, os.path.join(SCREENS, "05_usb_rec_status_over_waveform.png"))
    pill = fr5[3:22, 95:225].astype(int)
    check(int(((pill[:, :, 0] > 200) & (pill[:, :, 1] < 80)).sum()) > 20 and int((pill.min(axis=2) > 200).sum()) > 30, "Pixel: roter Punkt + weisser Text oben Mitte")
    for b_, ev, t in [(7, 0, 9000), (7, 2, 9401)]: btn(b_, ev, t, d2)
    check(calls == [], "kurz: Druecken + erste Wiederholung zurueckgehalten")
    btn(7, 1, 9600, d2)
    check(calls == [(7, 0), (7, 1)], f"kurz (600 ms): beim Loslassen Druecken+Loslassen ans Original ({calls})")
    calls.clear(); e2.uc.mem_write(UI + 0x18, b"\x01")
    btn(7, 0, 9700, d2); btn(7, 1, 9800, d2)
    check(calls == [(7, 0), (7, 1)], "Shift + Live Rec (Undo) unveraendert")
    e2.uc.mem_write(UI + 0x18, b"\x00"); calls.clear()
    for b_, ev, t in [(7, 0, 10000), (7, 2, 10401), (7, 2, 10701), (7, 2, 10901), (7, 2, 11051)]: btn(b_, ev, t, d2)
    check(e2.r32(RS + 20) == 2 and e2.r32(RS + 24) == 0, "zweiter Langdruck: rec_request(0)")
    btn(7, 1, 11100, d2)
    e2.call(tnm["lr_poll"] | 1); d2.pump()
    check(not d2.visible(lslot), "Aufnahme aus → Statusanzeige verborgen")
    # 12.11.: Start abgelehnt (falsche Audio-Out-Variante) → rote Pille "Need Stereo + In" fuer 2,5 s statt langem Toast
    e2.w32(MILLIS, 20000); e2.w32(0x1FFF9EB0 + 0xAC, 20000 | 1)
    n0 = len(d2.eve.cmd_log)
    e2.call(tnm["lr_poll"] | 1); d2.pump()
    ft = [c[5] for c in d2.eve.cmd_log[n0:] if c[0] == 0x0C]
    frf = d2.eve.frames[-1]
    _I.fromarray(frf).save(os.path.join(SCREENS, "05d_rec_fail.png"))
    redp = frf[3:22, 72:248].astype(int)
    check(d2.visible(lslot) and ft == ["Need Stereo + In"] and int(((redp[:, :, 0] > 180) & (redp[:, :, 1] < 60)).sum()) > 1500,
          f"Fehler-Pille rot mit Text ({ft})")
    e2.w32(MILLIS, 22600); e2.call(tnm["lr_poll"] | 1); d2.pump()
    check(not d2.visible(lslot) and e2.r32(0x1FFF9EB0 + 0xAC) == 0, "nach 2,5 s wieder weg")
    if "kbar" in dir() or True:
        try:
            check(e2.r32(BAR + 0xC) & 1 and e2.r32(0x1FFFB870 + 0x7C) == 0, "045: Aufnahme aus → Kopfzeilen-Balken wieder da")
            e2.w32(MGRA + 4 * kbar, 0)
        except NameError: pass
    print("9) Patch 029: Slicer-Seite in View (wv_kb), ohne grauen Kasten, mit Versatz")
    dv = Disp(img, True, nm); ev = dv.e
    ev.uc.mem_write(SDRAM, data.tobytes()); dv.set_sample(7, SDRAM, frames)
    ev.uc.mem_write(RECS + 0x38 * 7 + 0x14, b"Rec 003\0")
    dv.rig.set_mode(0, 4); dv.rig.write_step({9: 7, 10: 2 << 6})
    ev.w32(0x1FFF450C + 0x7C, dv.rig.step); ev.w32(0x1FFF450C + 0x5C, 0)
    ev.uc.mem_write(0x1FFFB940 + 0x44, bytes([24]))
    ta = ARGS + 0x100; tb = ARGS + 0x120; tc = ARGS + 0x140
    ev.uc.mem_write(ta, b"Slice 3/16\0"); ev.uc.mem_write(tc, b"16 Slices  +46 ms\0"); ev.uc.mem_write(tb, b"Gate 75%\0")
    ev.uc.mem_write(ev.sp, struct.pack("<IIII", ta, tc, tb, 0))
    ev.call(nm["wv_kb"] | 1, 1, 0, 2, 0x5FFF, limit=60_000_000)
    for _ in range(200):
        ev.call(nm["wv_poll"] | 1)
        if ev.r32(WV + 0x0C) == 0: break
    dv.pump()
    frv = dv.eve.frames[-1]
    save_png(frv, os.path.join(SCREENS, "06_view_slicer_029.png"))
    texts = [c[5] for c in dv.eve.cmd_log if c[0] == 0x0C][-5:]   # 045: + "VIEW"
    check(any("Rec 003" in x for x in texts) and any("Slice 3/16" in x for x in texts) and any("16 Slices" in x and "+" in x and "ms" in x for x in texts) and any("Gate 75%" in x for x in texts), f"Texte der Seite: {texts}")
    box = frv[64:184, 8:312]
    check(not ((box[:, :, 0] == 0x11) & (box[:, :, 1] == 0x11) & (box[:, :, 2] == 0x11)).any(), "kein grauer Kasten (0x111111) mehr in der Box")
    check(not ((box[:, :, 0] == 0x30) & (box[:, :, 1] == 0x30) & (box[:, :, 2] == 0x30)).any() and (box.min(axis=2) > 0xE0).sum() > 20, "13.11.: kein heller Kasten mehr, aktiver Slice ueber helle Waveform erkennbar")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


def eve_cmds(d, code): return [c for c in d.eve.cmd_log if c[0] == code]


if __name__ == "__main__":
    main()
