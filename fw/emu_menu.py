#!/usr/bin/env python3
"""Maschinencode-Test von Patch 017 im Cortex-M4-Emulator (unicorn) auf dem gebauten Image:
  A) ECHTER Menue-Code der Firmware: MenuItem_ctor 0x3350C (ueber um_menu_hook), EnumItem_ctor 0x34F30 (ueber um_build),
     vtable-Methoden der Auswahl-Eintraege (Text des Werts, Anzahl, Wert setzen, Listenaufbau), gespeicherter
     std::function-Invoker → um_changed.
  B) Umschalten "Audio Out": um_poll trennt (USB0_CONTROL), kopiert die Variante, ECHTER usbotg_isr liefert nach
     Bus-Reset GET_DESCRIPTOR(Konfiguration) mit 390 / 248 / 149 Byte aus der RAM-Deskriptorliste.
  C) Multitrack-Preset/Fader → mt_cfg_t, Classic/Creative → Namenspuffer (die Perform-Eintraege zeigen dorthin).
Heap_alloc/Heap_free (0x56F5C/0x56F6C) werden durch einen einfachen Bump-Allokator ersetzt; FatFs wird nicht
beruehrt (Datei-Lesen per t_load in die Zukunft verschoben).

  D) Patch 021: Seite "Engines" (sl_menu_add aus um_menu_hook, sl_build, 8 Auswahl-Eintraege).

usage: emu_menu.py PolyendPlay_v1.6.0_mod.ppf
"""
import re, sys, struct, os
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SP
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_isr import Dev, setup, REG  # noqa: E402
from check_ppf import images  # noqa: E402

UM = 0x1FFFD300
MT_CFG = 0x1FFFD480
NAMES = 0x1FFFB400
MILLIS = 0x20000D7C
fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


def cstr(dev, a, n=64):
    b = bytes(dev.uc.mem_read(a, n)); return b.split(b"\0")[0].decode("latin1")


def main():
    ppf = sys.argv[1]
    img = bytearray(max(images(ppf)[0]) + 1)
    for a, v in images(ppf)[0].items(): img[a] = v
    here = os.path.dirname(os.path.abspath(__file__))
    nm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(here, "fx.elf")) if len(l.split()) == 3}
    dev = Dev(img); uc = dev.uc
    heap = {"p": 0x20010000, "live": 0}
    def h_alloc(uc_, addr, size, ud):
        n = uc.reg_read(UC_ARM_REG_R0); p = heap["p"]; heap["p"] = (p + n + 7) & ~7; heap["live"] += 1
        uc.mem_write(p, bytes(n)); uc.reg_write(UC_ARM_REG_R0, p); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    def h_free(uc_, addr, size, ud):
        heap["live"] -= 1; uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, h_alloc, begin=0x56F5C, end=0x56F5C)
    uc.hook_add(UC_HOOK_CODE, h_free, begin=0x56F6C, end=0x56F6C)
    def call5(addr, a0, a1, a2, a3, a4):
        dev.w32(dev.sp, a4)                                  # 5. Argument auf dem Stack (AAPCS)
        return dev.call(addr, a0, a1, a2, a3)
    def call7(addr, a0, a1, a2, a3, a4, a5, a6):
        uc.mem_write(dev.sp, struct.pack("<III", a4, a5, a6)); return dev.call(addr, a0, a1, a2, a3)

    print("Start: usbinit_hook (um_early_init + ua_init + mt_init + usb_init)")
    dev.call(nm["usbinit_hook"])
    check(dev.r32(UM) == 0x55AEC017, "um_t initialisiert (magic)")
    check([dev.r32(UM + 4 + 4 * k) for k in range(4)] == [1, 0, 1, 1], "Standardwerte 1/0/1/1 (049: Stereo)")
    check(dev.r32(UM + 0x70 + 12 + 4) == 0x1FFFD4A0 and (dev.r32(UM + 0x70 + 12 + 8) & 0xFFFF) == 248, "RAM-Deskriptorliste: Eintrag 1 = 0x1FFFD4A0 / 248 (049: Start mit Stereo)")
    check(struct.unpack("<I", img[0x131AC:0x131B0])[0] == UM + 0x70, "Literal 0x131AC zeigt auf die RAM-Liste")
    dly0 = struct.unpack("<I", img[0xA86B4 + 8:0xA86B4 + 12])[0]; rvb15 = struct.unpack("<I", img[0xA8E34 + 20 * 15 + 8:0xA8E34 + 20 * 15 + 12])[0]
    check(dly0 == NAMES and rvb15 == NAMES + 16 * 31, "Namenszeiger Delay 0 / Reverb 15 → RAM-Puffer")
    check(cstr(dev, dly0) == "Echo 1/16" and cstr(dev, rvb15) == "Reso S&H", f"Creative-Namen im Puffer: {cstr(dev, dly0)!r}, {cstr(dev, rvb15)!r}")

    st_desc = open(os.path.join(here, "fixtures/desc_stereo.bin"), "rb").read()
    st_desc = bytearray(st_desc[:149] + bytes([8, 0x0B, 3, 2, 1, 1, 0, 0]) + st_desc[149:]); st_desc[2] = len(st_desc); st_desc = bytes(st_desc)   # 046: + IAD Audio
    print("Enumeration (049: Standard Stereo)")
    def get_cfg():
        dev.bus_reset()
        h = dev.control(setup(0x80, 6, 0x0200, 0, 9))
        full = dev.control(setup(0x80, 6, 0x0200, 0, 0xFFFF))
        w255 = dev.control(setup(0x80, 6, 0x0200, 0, 255))
        return h, full, w255
    h, full, _ = get_cfg()
    desc365 = bytes(img[nm["ua_cfg_desc"]:nm["ua_cfg_desc"] + 390])   # 049: 390 B
    check(h[2] | h[3] << 8 == 248 and full == st_desc, f"Config {len(full)} Byte = Stereo-Deskriptor")
    check(bytes(uc.mem_read(0x1FFF1E9C, 2)) == b"\x81\x02", "049: bcdDevice 0x0281 beim Start")

    print("A) Menue mit dem echten Firmware-Code")
    def page(it):
        b, e = dev.r32(it + 0x28), dev.r32(it + 0x2C)
        nb, ne = dev.r32(it + 0x34), dev.r32(it + 0x38)
        return [dev.r32(b + 4 * i) for i in range((e - b) // 4)], [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    alloc = lambda n: (lambda p: (heap.__setitem__("p", (p + n + 7) & ~7), uc.mem_write(p, bytes(n)), p)[2])(heap["p"])
    root = alloc(0x5C)
    call5(0x3350D, root, root, 0xA490C, 0, 0)                # Wurzel "Menu" (item == parent → nur Grundinit)
    sett = alloc(0x5C)
    call5(nm["um_menu_hook"] | 1, sett, root, 0xA44E0, 0x36141, 0)
    b, e = dev.r32(root + 0x28), dev.r32(root + 0x2C)
    kids = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(root + 0x34), dev.r32(root + 0x38)
    names = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    if names[:1] == ["Engines"]: kids, names = kids[1:], names[1:]   # 17.11.: Engines im Hauptmenue (nur mit Slicer-Zustand)
    check(len(kids) == 3 and names == ["USB Audio", "envyu Settings", "Settings"], f"Hauptmenue-Kinder (046): {names}")
    check(kids[2] == sett and dev.r32(kids[0] + 0x44) == nm["um_build"] | 1 and dev.r32(kids[1] + 0x44) == nm["env_build"] | 1
          and dev.r32(kids[2] + 0x44) == 0x36141 and dev.r32(kids[0] + 0x48) == 0, "Builder: USB Audio → um_build, envyu Settings → env_build, Settings unveraendert 0x36141")
    check(all(dev.r32(k) == 0xA4928 and dev.r32(k + 4) == root for k in kids), "vtable MenuItem 0xA4928, Elternzeiger Wurzel")
    env = kids[1]
    dev.call(nm["env_build"] | 1, 0x1234, env)
    ek, en = page(env)
    check(en == ["Color Mode", "Keep Selection", "About"], f"envyu Settings (ohne Slicer-Zustand): {en}")
    check(dev.r32(ek[0]) == 0xA49CC and dev.r32(ek[0] + 0x38) == 0x1FFFB860 and dev.r32(ek[0] + 4) == env
          and dev.r32(ek[1]) == 0xA49CC and dev.r32(ek[1] + 0x38) == 0x1FFF9A4C and dev.r32(ek[2]) == 0xA4928 and dev.r32(ek[2] + 0x44) == nm["ab_build"] | 1,
          "Color Mode / Keep Selection = Enum-Zeilen (0x1FFFB860 / 0x1FFF9A4C), About = Unterseite → ab_build")
    usb = kids[0]
    dev.w32(MILLIS, 5000)
    dev.call(nm["um_build"] | 1, 0x1234, usb)
    b, e = dev.r32(usb + 0x28), dev.r32(usb + 0x2C)
    items = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(usb + 0x34), dev.r32(usb + 0x38)
    inames = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(inames == ["Audio Out", "Multitrack 9/10", "Track Level", "FX Mode", "Rec", "Wait For Signal", "Pause On Silence", "Audio In"], f"Seite USB Audio: {inames}")
    check(dev.r32(items[7]) == 0xA4928 and dev.r32(items[7] + 0x44) == nm["ui_in_build"] | 1 and dev.r32(items[7] + 4) == usb, "046: Audio In = Unterseite von USB Audio → ui_in_build")
    items = items[:7]
    exp = [(UM + 4, 4, ["Off", "Stereo", "Stereo + Multi", "Stereo + In", "Stereo+Multi (Win)"]), (UM + 8, 1, ["Master", "FX Return"]),   # 049: + Win
           (UM + 12, 1, ["Pre Fader", "Post Fader"]), (UM + 16, 1, ["Classic", "Creative"]),
           (UM + 0xDC, 1, ["Mono", "Stereo"]),   # 026: Rec Mono/Stereo
           (0x1FFF9A44, 1, ["Off", "On"]), (0x1FFF9A48, 1, ["Off", "On"])]   # 045 (11.11.): Wait For Signal / Pause On Silence
    for k, it in enumerate(items):
        vt = dev.r32(it)
        ok = vt == 0xA49CC and dev.r32(it + 0x38) == exp[k][0] and dev.r32(it + 0x3C) == exp[k][1]
        ok &= dev.r32(it + 0x1C) == 0x31A35 and dev.r32(it + 0x20) == nm["um_changed"] | 1 and dev.r32(it + 0x14) == UM
        cnt = dev.call(dev.r32(vt + 7 * 4), it)                       # Anzahl (0x31355: max + 1)
        txt = cstr(dev, dev.call(dev.r32(vt + 3 * 4), it))            # Text des aktuellen Werts (0x32559)
        cur = dev.r32(exp[k][0])
        ok &= cnt == exp[k][1] + 1 and txt == exp[k][2][cur]
        dev.call(dev.r32(vt + 5 * 4), it, 0)                          # Wert setzen (0x31349)
        txt0 = cstr(dev, dev.call(dev.r32(vt + 3 * 4), it))
        ok &= dev.r32(exp[k][0]) == 0 and txt0 == exp[k][2][0]
        dev.call(dev.r32(vt + 5 * 4), it, cur)
        lst = dev.call(dev.r32(vt + 8 * 4), it)                       # Liste aufbauen (0x32201) → Tabelle
        ok &= [cstr(dev, dev.r32(lst + 4 * j)) for j in range(cnt)] == exp[k][2]
        check(ok, f"{inames[k]}: vtable 0xA49CC, Wert@{exp[k][0]:08X}, {cnt} Eintraege, Text {txt!r}→{txt0!r}, Liste {exp[k][2]}")
    dev.w32(MILLIS, 7777)
    dev.call(dev.r32(items[3] + 0x20), items[3] + 0x14, 1)          # Firmware ruft nach der Auswahl den Invoker
    check(dev.r32(UM + 0x4C) == 7777, "gespeicherter Invoker → um_changed (t_change = millis)")

    print("B) Audio Out umschalten (Neuanmeldung, echter usbotg_isr)")
    dev.w32(UM + 0x48, 0x7FFFFFFF)                                  # Datei nicht lesen (keine SD im Emulator)
    def poll_at(ms): dev.w32(MILLIS, ms); dev.call(nm["um_poll"] | 1)
    t = 10000
    import hashlib
    COMB_MD5 = "bdc96f9abfc5d798fc0367592559c489"     # 049: kombinierter Deskriptor = 046 (am Mac bewaehrt), zur Laufzeit erzeugt
    for mode, ln, ref, nif in ((2, 365, "md5", 6), (4, 390, desc365, 7), (0, 149, None, 3), (3, 331, None, 6), (1, 248, st_desc, 5)):   # 049: Start ist Stereo
        dev.w32(UM + 4, mode)
        poll_at(t)
        detached = dev.hw.r[0x108] == 0 and uc.mem_read(0x20000D67, 1)[0] == 0
        poll_at(t + 200); still = dev.hw.r[0x108] == 0
        poll_at(t + 450); attached = dev.hw.r[0x108] == 0x10
        h, full, w255 = get_cfg()
        ok = detached and still and attached and h is not None and (h[2] | h[3] << 8) == ln and h[4] == nif and len(full) == ln
        if ref == "md5": ok &= full is not None and hashlib.md5(full).hexdigest() == COMB_MD5
        elif ref is not None: ok &= full == ref
        elif mode == 0: ok &= full[9:] == desc365[9:149] and full[:2] == b"\x09\x02"
        else: ok &= full is not None and full[:2] == b"\x09\x02"
        ok &= bytes(uc.mem_read(0x1FFF1E9C, 2)) == bytes([(0x77, 0x81, 0x82, 0x83, 0x84)[mode], 0x02])   # 049: bcdDevice je Modus
        ok &= dev.r32(0x1FFFD47C) == (0 if mode == 2 else 1)                                         # 049: mt_t.split
        ok &= w255 == full[:255]
        check(ok, f"Audio Out={mode}: getrennt→verbunden, GET_DESCRIPTOR {len(full) if full else 0} Byte, {h[4] if h else '-'} Interfaces, wLength 255 → {len(w255) if w255 else 0}")
        dev.control(setup(0x00, 5, 7, 0, 0)); dev.control(setup(0x00, 9, 1, 0, 0))
        check(uc.mem_read(0x20000D67, 1)[0] == 1, f"  SET_CONFIGURATION danach ok")
        t += 1000
    check(dev.r32(UM + 0x58) == 5, "5 Neuanmeldungen gezaehlt")

    print("C) Multitrack und Classic/Creative")
    dev.w32(UM + 8, 1); dev.w32(UM + 12, 0); poll_at(t)
    check(uc.mem_read(MT_CFG + 4, 1)[0] == 1 and uc.mem_read(MT_CFG + 5, 1)[0] & 1 == 0, "Multitrack 9/10 = FX Return, Track Level = Pre Fader → mt_cfg_t")
    dev.w32(UM + 16, 0); poll_at(t + 10)
    check(cstr(dev, dly0) == "One2One" and cstr(dev, rvb15) == "Radiator", f"Classic: Originalnamen {cstr(dev, dly0)!r}, {cstr(dev, rvb15)!r}")
    allnames = [cstr(dev, NAMES + 16 * i) for i in range(32)]
    check(all(len(s) > 0 for s in allnames) and len(set(allnames)) == 32, f"32 verschiedene Originalnamen gesetzt")
    dev.w32(UM + 16, 1); poll_at(t + 20)
    check(cstr(dev, dly0) == "Echo 1/16", "Creative: FX-Suite-Namen zurueck")

    print("D) Patch 021: Seite \"Engines\" mit dem echten Firmware-Code")
    SL = 0x1FFF9800
    dev.call(nm["sl_reset"] | 1)
    dev.w32(SL + 0x68, 0x7FFFFFFF)                                  # /engines.cfg nicht lesen (keine SD)
    root = alloc(0x5C); call5(0x3350D, root, root, 0xA490C, 0, 0)
    sett = alloc(0x5C); call5(nm["um_menu_hook"] | 1, sett, root, 0xA44E0, 0x36141, 0)
    b, e = dev.r32(root + 0x28), dev.r32(root + 0x2C)
    kids = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(root + 0x34), dev.r32(root + 0x38)
    names = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(names[:1] == ["Engines"] and dev.r32(kids[0] + 0x44) == nm["sl_build"] | 1 and dev.r32(kids[0] + 4) == root,
          f"17.11.: Hauptmenue beginnt mit Engines (→ sl_build): {names[:1]}")
    eng = kids[0]
    kids, names = kids[1:], names[1:]
    check(names == ["USB Audio", "envyu Settings", "Settings"] and kids[2] == sett, f"Hauptmenue-Kinder: {names}")
    env = kids[1]; dev.call(nm["env_build"] | 1, 0x1234, env)
    envk, en = page(env)
    check(en == ["Color Mode", "Keep Selection", "About"], f"envyu Settings ohne Engines: {en}")
    dev.call(nm["sl_build"] | 1, 0x1234, eng)
    b, e = dev.r32(eng + 0x28), dev.r32(eng + 0x2C)
    items = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(eng + 0x34), dev.r32(eng + 0x38)
    inames = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(inames == [f"Track {i}" for i in range(1, 9)], f"Seite Engines: {inames}")
    modes = ["Normal", "Slicer"]   # 045: nur die Art, Slice-Zahl in View; Granular ausgeblendet
    allok = len(items) == 8
    for k, it in enumerate(items):
        vt = dev.r32(it)
        ok = vt == 0xA49CC and dev.r32(it + 0x38) == 0x1FFFB840 + 4 * k and dev.r32(it + 0x3C) == 1
        ok &= dev.r32(it + 0x1C) == 0x31A35 and dev.r32(it + 0x20) == nm["sl_changed"] | 1 and dev.r32(it + 0x14) == SL
        cnt = dev.call(dev.r32(vt + 7 * 4), it)
        txt = cstr(dev, dev.call(dev.r32(vt + 3 * 4), it))
        dev.call(dev.r32(vt + 5 * 4), it, 1); dev.call(nm["sl_kind_apply"] | 1)
        txt4 = cstr(dev, dev.call(dev.r32(vt + 3 * 4), it))
        ok &= cnt == 2 and txt == "Normal" and txt4 == "Slicer" and dev.r32(SL + 4 + 4 * k) == 4
        lst = dev.call(dev.r32(vt + 8 * 4), it)
        ok &= [cstr(dev, dev.r32(lst + 4 * j)) for j in range(cnt)] == modes
        dev.call(dev.r32(vt + 5 * 4), it, 0); dev.call(nm["sl_kind_apply"] | 1)
        ok &= dev.r32(SL + 4 + 4 * k) == 0
        allok &= bool(ok)
    check(allok, "8 Zeilen: vtable 0xA49CC, Wert@0x1FFFB840+4t, Normal/Slicer, Slicer → 16 Slices, Normal → 0")
    dev.w32(MILLIS, 8888)
    dev.call(dev.r32(items[5] + 0x20), items[5] + 0x14, 1)
    check(dev.r32(SL + 0x6C) == 8888, "gespeicherter Invoker → sl_changed (t_change = millis)")
    print("E) 046: Seite \"About\" (nur lesbar)")
    about = envk[2]
    check(dev.r32(about + 0x44) == nm["ab_build"] | 1, "Builder About → ab_build")
    hdr = nm["SN_HDR"]; crc = dev.r32(hdr + 8)
    dev.call(nm["ab_build"] | 1, 0x1234, about)
    b, e = dev.r32(about + 0x28), dev.r32(about + 0x2C)
    items = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(about + 0x34), dev.r32(about + 0x38)
    inames = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(inames == ["Version", "Build", "Based On", "Hardware"], f"Zeilen: {inames}")
    VER = re.search(r'ENVYU_VERSION "([^"]+)"', open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "version.h")).read()).group(1)   # 049: aus version.h
    want = [VER, "%08x" % crc, "Play 1.6.0", "Rev A"]
    got = []; ok = True
    for k, it in enumerate(items):
        vt = dev.r32(it)
        cnt = dev.call(dev.r32(vt + 7 * 4), it)
        got.append(cstr(dev, dev.call(dev.r32(vt + 3 * 4), it)))
        dev.call(dev.r32(vt + 5 * 4), it, 0)
        ok &= vt == 0xA49CC and cnt == 1 and dev.r32(it + 0x38) == 0x1FFFB6C0 + 4 * k and dev.r32(it + 0x20) == nm["ab_noop"] | 1
        ok &= cstr(dev, dev.call(dev.r32(vt + 3 * 4), it)) == want[k]
    check(got == want and ok, f"Werte {got} (je 1 Eintrag, Wert-Slot 0x1FFFB6C0+4k, leerer Invoker – nichts wird gespeichert)")
    t0 = dev.r32(UM + 0x4C)
    dev.call(dev.r32(items[1] + 0x20), items[1] + 0x14, 0)
    check(dev.r32(UM + 0x4C) == t0, "Auswahl in About aendert keine Einstellung (t_change unveraendert)")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
