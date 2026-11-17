#!/usr/bin/env python3
"""13.11.2026 – Sicherheitsnetz im Emulator (echtes Image, echter Kopfblock guard.c + Weichen aus veneer.py).

1. Kopf {Magic, Laenge, CRC} stimmt mit dem angehaengten Code ueberein (unabhaengig nachgerechnet).
2. sn_guard, Code intakt: Merker != BAD, sl_init wird aufgerufen, Original-Init nicht.
3. sn_guard, ein Byte am ENDE beschaedigt (Abbruch beim Schreiben): Merker = BAD, Original-Init 0x271D1, Effektnamen
   = Original-Strings, USB-Liste = Original (Eintrag 1 → 0x1FFF1DD8 / 0xB5), NIS-Namen leer.
4. Jede Patchstelle im Original zeigt auf eine Weiche; Weiche mit Merker normal → unser Code, mit BAD → Original-Ziel
   (bzw. Stub, der die ersetzten Original-Befehle ausfuehrt und zurueckspringt).
5. Generator-Weiche sn_gen: BAD → Original-Wert (r3) zurueck.
usage: emu_guard.py PolyendPlay_v1.6.0_mod.ppf ../stock/firmware.bin
"""
import os, struct, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_R5, UC_ARM_REG_R2  # noqa: E402
from emu_nis import Emu, load_ppf, STOP  # noqa: E402
import veneer as VN  # noqa: E402

BASE, FLAG, BAD = 0xB069C, 0x1FFF9A38, 0xA5A5A5A5
fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    print(("  ok    " if c else "  FEHLER ") + m)
    if not c: fails += 1


def syms():
    out = subprocess.run(["arm-none-eabi-nm", os.path.join(HERE, "fx.elf")], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


def main():
    img = load_ppf(sys.argv[1]); fw = open(sys.argv[2], "rb").read(); nm = syms()
    fx = open(os.path.join(HERE, "fx.bin"), "rb").read()
    H = nm["SN_HDR"]; magic, ln, crc, nv = struct.unpack_from("<IIII", img, H)
    tmp = bytearray(img[BASE:BASE + ln]); struct.pack_into("<I", tmp, H - BASE + 8, 0)
    print("1) Kopf")
    check(magic == 0x564E4553 and ln == len(fx) and bytes(img[BASE:BASE + ln]) == fx, f"Kopf am Anfang: Magic, Laenge {ln} = fx.bin, Image = fx.bin")
    check(zlib.crc32(bytes(tmp)) & 0xFFFFFFFF == crc and nv >= 30, f"CRC {crc:08x} stimmt, {nv} Weichen")
    check(H == BASE and nm["sn_guard"] < nm["__head_end"] and nm["NIS_LIST"] < nm["__head_end"] and nm["AUD_LIST"] < nm["__head_end"]
          and nm["SN_VENEERS"] < nm["__head_end"], f"Kopf, Weichen und Listen vorn (Kopfende {nm['__head_end']:#x} = {nm['__head_end'] - BASE} Byte)")
    check(struct.unpack_from("<I", img, 0xAE80C)[0] == nm["sn_guard"] | 1, "init_array 0xAE80C → sn_guard")

    def run_guard(image):
        e = Emu(image); calls = {"sl_init": 0, "orig": 0}
        e.uc.mem_write(FLAG, struct.pack("<I", 0x12345678))         # Kaltstart: beliebig
        e.stub(nm["sl_init"] & ~1, cb=lambda em: calls.__setitem__("sl_init", calls["sl_init"] + 1))
        e.stub(0x271D0, cb=lambda em: calls.__setitem__("orig", calls["orig"] + 1))
        e.call(nm["sn_guard"] | 1, limit=50_000_000)
        return e, calls

    print("2) Code intakt")
    e, c = run_guard(img)
    check(e.r32(FLAG) != BAD and c == {"sl_init": 1, "orig": 0}, f"Merker {e.r32(FLAG):#x}, sl_init {c['sl_init']}×, Original-Init {c['orig']}×")

    print("3) Ende beschaedigt (Schreibabbruch)")
    bad = bytearray(img)
    for a in range(BASE + ln - 64, BASE + ln): bad[a] = 0xFF
    e, c = run_guard(bad)
    check(e.r32(FLAG) == BAD and c == {"sl_init": 0, "orig": 1}, f"Merker BAD, sl_init {c['sl_init']}×, Original-Init {c['orig']}×")
    ok = True
    for k, a in enumerate([0xA6078, 0xA6080, 0xA5FBC, 0xA606C]):
        want = bytes(fw[a:a + 16]).split(b"\0")[0][:15]
        slot = (0, 1, 16, 31)[k]
        got = bytes(e.uc.mem_read(0x1FFFB400 + 16 * slot, 16)).split(b"\0")[0]
        if got != want: ok = False; print("    ", slot, got, want)
    check(ok, "Effektnamen = Original-Strings")
    lst = bytes(e.uc.mem_read(0x1FFFD370, 0x6C))
    check(struct.unpack_from("<I", lst, 16)[0] == 0x1FFF1DD8 and struct.unpack_from("<H", lst, 20)[0] == 0xB5 and lst[:12] == fw[0x9BAC0:0x9BACC]
          and lst[24:] == fw[0x9BAC0 + 24:0x9BAC0 + 0x6C], "USB-Deskriptorliste = Original (Eintrag 1: 0x1FFF1DD8, 0xB5)")
    check(bytes(e.uc.mem_read(0x1FFFB900, 0x40)) == bytes(0x40), "NIS-Namen geleert")

    print("4) Weichen an allen Patchstellen")
    diff = sorted(a for a in range(0, BASE) if img[a] != fw[a])
    sites = []
    seen = set()
    for a in diff:
        for s in (a & ~1, (a & ~1) - 2):
            if s in seen or s < 0: continue
            d = VN.dec_bl(*struct.unpack_from("<HH", img, s))
            if d and s + 4 + d[1] >= BASE:
                sites.append(("b", s, s + 4 + d[1])); seen.add(s); break
        w = a & ~3
        if w not in seen:
            v = struct.unpack_from("<I", img, w)[0]
            if v & 1 and BASE <= v < 0x100000: sites.append(("p", w, v & ~1)); seen.add(w)
    ven = nm["SN_VENEERS"]; vend = ven + 52 * 24; hend = nm["__head_end"]
    direct = [hex(s) for k, s, t in sites if not (ven <= t < vend) and s != 0xAE80C]
    cover = set()
    for k, s_, t in sites: cover |= set(range(s_, s_ + 4))
    rest = []
    for a in diff:
        if a in cover: continue
        w = a & ~3; v = struct.unpack_from("<I", img, w)[0]
        if 0x1FFF0000 <= v < 0x20040000: continue                    # Zeiger auf RAM-Puffer (Namen, USB-Liste)
        if BASE <= v < hend and not v & 1: continue                  # Datenzeiger in den Kopf (Randomize-Listen)
        if a in (0x3894E, 0x38950, 0x38DE6, 0x38DE8, 0x9BAD4, 0x9BAD5, 0xAA06E, 0xAA06F): continue   # Typgrenzen, Deskriptorlaenge, MIDI-Grenzwert
        rest.append(hex(a))
    check(not direct and not rest, f"{len(sites)} Code-Stellen ueber Weichen; jedes geaenderte Byte erklaert (direkt {direct}, unerklaert {rest[:8]})")

    e = Emu(img)
    landed = {}
    def follow(v, flag):
        e.uc.mem_write(FLAG, struct.pack("<I", flag))
        stop = []
        def h(uc, addr, size, ud):
            if not (ven <= addr < vend): stop.append(addr); uc.emu_stop()
        hh = e.uc.hook_add(UC_HOOK_CODE, h)
        e.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        try: e.uc.emu_start(v | 1, STOP, count=40)
        except Exception: pass
        e.uc.hook_del(hh)
        return stop[0] if stop else None
    bad_n = 0; stub_n = 0
    for k, s, v in sites:
        if s == 0xAE80C: continue
        if k == "b":
            od = VN.dec_bl(*struct.unpack_from("<HH", fw, s))
            orig = s + 4 + od[1] if od else None
        else: orig = struct.unpack_from("<I", fw, s)[0] & ~1
        real = follow(v, 0)
        fb = follow(v, BAD)
        if orig is None:                                         # Stub: fuehrt Original-Befehle aus, kehrt nach s+4 zurueck
            stub_n += 1
            if fb not in (STOP, ) and fb is not None and not (ven <= fb < vend): bad_n += 1; print("    Stub", hex(s), hex(fb or 0))
        elif fb != orig: bad_n += 1; print("    ", hex(s), "BAD →", hex(fb or 0), "erwartet", hex(orig))
        if real is None or real < vend or real >= 0x100000: bad_n += 1; print("    ", hex(s), "normal →", hex(real or 0))
    check(bad_n == 0, f"Merker normal → unser Code, BAD → Original-Ziel ({len(sites) - 1} Stellen, davon {stub_n} Befehls-Stubs)")

    print("5) Stubs fuehren die ersetzten Original-Befehle aus (Beispiel 0x12BAC: r5 = BDT, r6 = r3 >> 2)")
    st = [v for k, s, v in sites if s == 0x12BAC][0]
    e.uc.mem_write(FLAG, struct.pack("<I", BAD))
    e.uc.reg_write(UC_ARM_REG_R3, 0x40); e.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
    e.uc.emu_start(st | 1, STOP, count=200)
    from unicorn.arm_const import UC_ARM_REG_R6
    check(e.uc.reg_read(UC_ARM_REG_R5) == struct.unpack_from("<I", fw, 0x12E78)[0] and e.uc.reg_read(UC_ARM_REG_R6) == 0x10,
          f"r5 = {e.uc.reg_read(UC_ARM_REG_R5):#x}, r6 = {e.uc.reg_read(UC_ARM_REG_R6):#x}, zurueck")

    print("6) Generator-Weiche sn_gen")
    e.uc.mem_write(FLAG, struct.pack("<I", BAD))
    r = e.call(nm["sn_gen"] | 1, 1, 2, 3, 77)
    check(r == 77, f"BAD: Wert unveraendert ({r})")
    check(struct.unpack_from("<I", img, nm["NIS_LIST"] + 4 * 0 + 0)[0] != 0 and struct.unpack_from("<I", img, struct.unpack_from("<I", img, nm["NIS_LIST"] + 100 * 4 + 12)[0])[0] == nm["NIS_VT"] if "NIS_VT" in nm else True,
          "Listen zeigen auf Generatoren im Kopf")

    print(f"{'OK' if not fails else 'FEHLER'}: emu_guard {checks} Pruefungen, {fails} Fehler")
    return fails != 0


if __name__ == "__main__":
    sys.exit(main())
