#!/usr/bin/env python3
"""Maschinencode-Test von Patch 017 im Cortex-M4-Emulator (unicorn) auf dem gebauten Image:
  A) ECHTER Menue-Code der Firmware: MenuItem_ctor 0x3350C (ueber um_menu_hook), EnumItem_ctor 0x34F30 (ueber um_build),
     vtable-Methoden der Auswahl-Eintraege (Text des Werts, Anzahl, Wert setzen, Listenaufbau), gespeicherter
     std::function-Invoker → um_changed.
  B) Umschalten "Audio Out": um_poll trennt (USB0_CONTROL), kopiert die Variante, ECHTER usbotg_isr liefert nach
     Bus-Reset GET_DESCRIPTOR(Konfiguration) mit 357 / 240 / 149 Byte aus der RAM-Deskriptorliste.
  C) Multitrack-Preset/Fader → mt_cfg_t, Classic/Creative → Namenspuffer (die Perform-Eintraege zeigen dorthin).
Heap_alloc/Heap_free (0x56F5C/0x56F6C) werden durch einen einfachen Bump-Allokator ersetzt; FatFs wird nicht
beruehrt (Datei-Lesen per t_load in die Zukunft verschoben).

  D) Hauptmenue: USB Audio vor Settings.

usage: emu_menu.py PolyendPlay_v1.6.0_mod.ppf
"""
import sys, struct, os
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
    check([dev.r32(UM + 4 + 4 * k) for k in range(4)] == [2, 0, 1, 1], "Standardwerte 2/0/1/1")
    check(dev.r32(UM + 0x70 + 12 + 4) == 0x1FFFD4C0 and (dev.r32(UM + 0x70 + 12 + 8) & 0xFFFF) == 357, "RAM-Deskriptorliste: Eintrag 1 = 0x1FFFD4C0 / 357")
    check(struct.unpack("<I", img[0x131AC:0x131B0])[0] == UM + 0x70, "Literal 0x131AC zeigt auf die RAM-Liste")
    dly0 = struct.unpack("<I", img[0xA86B4 + 8:0xA86B4 + 12])[0]; rvb15 = struct.unpack("<I", img[0xA8E34 + 20 * 15 + 8:0xA8E34 + 20 * 15 + 12])[0]
    check(dly0 == NAMES and rvb15 == NAMES + 16 * 31, "Namenszeiger Delay 0 / Reverb 15 → RAM-Puffer")
    check(cstr(dev, dly0) == "Echo 1/16" and cstr(dev, rvb15) == "Reso S&H", f"Creative-Namen im Puffer: {cstr(dev, dly0)!r}, {cstr(dev, rvb15)!r}")

    print("Enumeration (Stereo + Multi)")
    def get_cfg():
        dev.bus_reset()
        h = dev.control(setup(0x80, 6, 0x0200, 0, 9))
        full = dev.control(setup(0x80, 6, 0x0200, 0, 0xFFFF))
        w255 = dev.control(setup(0x80, 6, 0x0200, 0, 255))
        return h, full, w255
    h, full, _ = get_cfg()
    desc357 = bytes(img[nm["ua_cfg_desc"]:nm["ua_cfg_desc"] + 357])
    check(h[2] | h[3] << 8 == 357 and full == desc357, f"Config {len(full)} Byte = ua_cfg_desc")

    print("A) Menue mit dem echten Firmware-Code")
    alloc = lambda n: (lambda p: (heap.__setitem__("p", (p + n + 7) & ~7), uc.mem_write(p, bytes(n)), p)[2])(heap["p"])
    root = alloc(0x5C)
    call5(0x3350D, root, root, 0xA490C, 0, 0)                # Wurzel "Menu" (item == parent → nur Grundinit)
    sett = alloc(0x5C)
    call5(nm["um_menu_hook"] | 1, sett, root, 0xA44E0, 0x36141, 0)
    b, e = dev.r32(root + 0x28), dev.r32(root + 0x2C)
    kids = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(root + 0x34), dev.r32(root + 0x38)
    names = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(len(kids) == 2 and names == ["USB Audio", "Settings"], f"Hauptmenue-Kinder: {names}")
    check(len(kids) == 2 and kids[1] == sett and dev.r32(kids[0] + 0x44) == nm["um_build"] | 1 and dev.r32(kids[1] + 0x44) == 0x36141
          and dev.r32(kids[0] + 0x48) == 0, "Builder: USB Audio → um_build, Settings unveraendert 0x36141")
    check(all(dev.r32(k) == 0xA4928 and dev.r32(k + 4) == root for k in kids), "vtable MenuItem 0xA4928, Elternzeiger")
    usb = kids[0]
    dev.w32(MILLIS, 5000)
    dev.call(nm["um_build"] | 1, 0x1234, usb)
    b, e = dev.r32(usb + 0x28), dev.r32(usb + 0x2C)
    items = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(usb + 0x34), dev.r32(usb + 0x38)
    inames = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(inames == ["Audio Out", "Multitrack 9/10", "Track Level", "FX Mode", "Rec"], f"Seite USB Audio: {inames}")
    exp = [(UM + 4, 3, ["Off", "Stereo", "Stereo + Multi", "Stereo + In"]), (UM + 8, 1, ["Master", "FX Return"]),
           (UM + 12, 1, ["Pre Fader", "Post Fader"]), (UM + 16, 1, ["Classic", "Creative"]),
           (UM + 0xDC, 1, ["Mono", "Stereo"])]   # 026: Rec Mono/Stereo
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
    st_desc = open(os.path.join(here, "fixtures/desc_stereo.bin"), "rb").read()
    t = 10000
    for mode, ln, ref, nif in ((1, 240, st_desc, 5), (0, 149, None, 3), (2, 357, desc357, 6)):
        dev.w32(UM + 4, mode)
        poll_at(t)
        detached = dev.hw.r[0x108] == 0 and uc.mem_read(0x20000D67, 1)[0] == 0
        poll_at(t + 200); still = dev.hw.r[0x108] == 0
        poll_at(t + 450); attached = dev.hw.r[0x108] == 0x10
        h, full, w255 = get_cfg()
        ok = detached and still and attached and h is not None and (h[2] | h[3] << 8) == ln and h[4] == nif and len(full) == ln
        if ref is not None: ok &= full == ref
        else: ok &= full[9:] == desc357[9:149] and full[:2] == b"\x09\x02"
        ok &= w255 == full[:255]
        check(ok, f"Audio Out={mode}: getrennt→verbunden, GET_DESCRIPTOR {len(full) if full else 0} Byte, {h[4] if h else '-'} Interfaces, wLength 255 → {len(w255) if w255 else 0}")
        dev.control(setup(0x00, 5, 7, 0, 0)); dev.control(setup(0x00, 9, 1, 0, 0))
        check(uc.mem_read(0x20000D67, 1)[0] == 1, f"  SET_CONFIGURATION danach ok")
        t += 1000
    check(dev.r32(UM + 0x58) == 3, "3 Neuanmeldungen gezaehlt")

    print("C) Multitrack und Classic/Creative")
    dev.w32(UM + 8, 1); dev.w32(UM + 12, 0); poll_at(t)
    check(uc.mem_read(MT_CFG + 4, 1)[0] == 1 and uc.mem_read(MT_CFG + 5, 1)[0] & 1 == 0, "Multitrack 9/10 = FX Return, Track Level = Pre Fader → mt_cfg_t")
    dev.w32(UM + 16, 0); poll_at(t + 10)
    check(cstr(dev, dly0) == "One2One" and cstr(dev, rvb15) == "Radiator", f"Classic: Originalnamen {cstr(dev, dly0)!r}, {cstr(dev, rvb15)!r}")
    allnames = [cstr(dev, NAMES + 16 * i) for i in range(32)]
    check(all(len(s) > 0 for s in allnames) and len(set(allnames)) == 32, f"32 verschiedene Originalnamen gesetzt")
    dev.w32(UM + 16, 1); poll_at(t + 20)
    check(cstr(dev, dly0) == "Echo 1/16", "Creative: FX-Suite-Namen zurueck")

    print("D) Hauptmenue mit dem echten Firmware-Code")
    root = alloc(0x5C); call5(0x3350D, root, root, 0xA490C, 0, 0)
    sett = alloc(0x5C); call5(nm["um_menu_hook"] | 1, sett, root, 0xA44E0, 0x36141, 0)
    b, e = dev.r32(root + 0x28), dev.r32(root + 0x2C)
    kids = [dev.r32(b + 4 * i) for i in range((e - b) // 4)]
    nb, ne = dev.r32(root + 0x34), dev.r32(root + 0x38)
    names = [cstr(dev, dev.r32(nb + 4 * i)) for i in range((ne - nb) // 4)]
    check(names == ["USB Audio", "Settings"], f"Hauptmenue-Kinder: {names}")
    check(len(kids) == 2 and dev.r32(kids[0] + 0x44) == nm["um_build"] | 1 and kids[1] == sett, "Builder: USB Audio → um_build, Settings unveraendert")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
