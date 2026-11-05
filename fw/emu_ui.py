#!/usr/bin/env python3
"""Maschinencode-Test von Patch 020 (USB-Eingang PC → Play): fuehrt den ECHTEN Code des gepatchten Rev-A-Images aus –
usbinit_hook → usb_init, um_poll (Neuanmeldung "Stereo + In"), usbotg_isr 0x12B9C mit ep_hook/setup_hook,
audio_tap → audio_callback 0xBA18 → (App-Attrappe) → ECHTES Mixer-Update 0x15884 mit den Umleitungen
0x15A3E (ui_cb_hook, Zusatzeingang Master) und 0x15CB6 (ui_send_hook, Delay/Reverb-Eingang) – im Cortex-M4-Emulator.
Vergleich jeweils mit dem Image von Patch 019 (ohne USB-Eingang) bei gleichen Spursignalen.

Geprueft:
  1. Ohne USB-Eingang: Mixer-Summen und Send-Eingaenge bitgleich mit 019 (Hooks aktiv, aber wirkungslos).
  2. Neuanmeldung "Stereo + In": 323-B-Deskriptor, SET_INTERFACE IF5, ENDPT6 0x1C, ENDPT7 0x14, Rueckmeldung 3 Byte.
  3. Streaming PC → Play nach der Rueckmeldung: Master-Summen = 019 + USB-Signal (jeder Frame, lueckenlos),
     Delay-/Reverb-Eingang = 019 + 0,25 * USB mono, Stereo-Ausgang EP 0x86 liefert parallel den Mix (Loopback).
  4. Original-Zusatzeingang der Firmware (Stream+0x150) wird weiter aufgerufen und addiert.
  5. Mixer ausserhalb von audio_tap (WAV-Export): keine Einspeisung.
  6. Stopp, SET_CONFIGURATION, Rueckkehr zu "Stereo + Multi": EP7 gehoert wieder dem Multitrack.

usage: emu_ui.py PolyendPlay_v1.6.0_usb_in.ppf ../019_menu_nis/PolyendPlay_v1.6.0_menu_nis.ppf
"""
import sys, struct, os, subprocess, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_isr import setup  # noqa: E402
from emu_mt import Rig, load, refsym, MIX, CNT, track_sig  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
UI = 0x1FFF9000
UM = 0x1FFFD300
MT = 0x1FFFD400
MILLIS = 0x20000D7C
CAP = 0x20021800
INCB_BUF = 0x20021A00
INCB_CTX = 0x20021F08
NOM_FB = 722824

fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


def build_stub():
    d = tempfile.mkdtemp(prefix="emu_stub_ui_")
    elf = os.path.join(d, "stub.elf"); binf = os.path.join(d, "stub.bin")
    subprocess.check_call(["arm-none-eabi-gcc", "-mcpu=cortex-m4", "-mthumb", "-Os", "-ffreestanding", "-nostdlib",
                           "-fno-builtin", "-Wl,-Ttext=0xF0000", "-Wl,-e,app_audio", os.path.join(HERE, "emu_stub_ui.c"), "-o", elf])
    subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", elf, binf])
    nm = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines() if len(l.split()) == 3}
    return open(binf, "rb").read(), nm


def s16(v): v &= 0xFFFF; return v - 0x10000 if v & 0x8000 else v
def sat16(v): return 32767 if v > 32767 else -32768 if v < -32768 else v
def usb_sig(f):
    """PC-Signal: L traegt den Framezaehler (15 Bit), R ein davon abhaengiges Muster."""
    return (f & 0x7FFF) - 0x4000, s16(f * 2654435761 >> 9) >> 1


def sums(acc):
    v = struct.unpack("<256i", acc[:1024])
    return v[:128], v[128:]


def main():
    ppf, ref = sys.argv[1], sys.argv[2]
    img, refimg = load(ppf), load(ref)
    nm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(HERE, "fx.elf")) if len(l.split()) == 3}
    stub, snm = build_stub()
    rig = Rig(img, stub, snm); rig.nm = nm
    rref = Rig(refimg, stub, snm, mt=False); rref.nm = {"audio_tap": refsym(ref)}
    dev, uc = rig.dev, rig.dev.uc
    for r in (rig, rref):                                   # SRAM_L mit Muell wie nach dem Einschalten
        r.dev.uc.mem_write(0x1FFF8800, bytes((i * 53 + 7) & 0xFF for i in range(0x1000)))
    refnm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(os.path.dirname(os.path.abspath(ref)), "fx.elf")) if len(l.split()) == 3}
    dev.call(nm["usbinit_hook"]); rref.dev.call(refnm["usbinit_hook"])
    check(dev.r32(UI) == 0x55A0D120 and dev.r32(UI + 4) == 0 and dev.r32(UI + 0x34) == NOM_FB, "ui_t initialisiert: magic, Modus 0, Rueckmeldung nominal")

    def both_block(b, present=0xFF, via_tap=True):
        for r in (rig, rref): r.vary_gains(b)
        a = rig.block(b, present=present, via_tap=via_tap); c = rref.block(b, present=present, via_tap=via_tap)
        capa = bytes(dev.uc.mem_read(CAP, 512)); capc = bytes(rref.dev.uc.mem_read(CAP, 512))
        return a, c, capa, capc

    print("1. Ohne USB-Eingang: bitgleich mit 019")
    same = True
    for b in range(16):
        a, c, capa, capc = both_block(b, present=0xFF if b % 3 else 0x5A)
        same &= a[1:] == c[1:] and capa == capc
    check(same, "16 Bloecke: L/R, Master-Summen und Delay/Reverb-Eingang identisch")

    print("2. Neuanmeldung 'Stereo + In' (um_poll, echter usbotg_isr)")
    dev.w32(UM + 0x44, 0); dev.w32(UM + 0x48, 0x7FFFFFFF)  # keine SD im Emulator: nicht lesen, nicht speichern
    def poll_at(ms): dev.w32(MILLIS, ms); dev.call(nm["um_poll"] | 1)
    dev.w32(UM + 4, 3); poll_at(10000)
    check(dev.hw.r[0x108] == 0, "getrennt (Pullup aus)")
    poll_at(10450)
    check(dev.hw.r[0x108] == 0x10 and dev.r32(UI + 4) == 1, "verbunden, ui-Modus 1")
    dev.bus_reset()
    full = dev.control(setup(0x80, 6, 0x0200, 0, 0xFFFF))
    desc = bytes(img[nm["UI_DESC_IN"]:nm["UI_DESC_IN"] + 323])
    check(full == desc and full[4] == 6, f"GET_DESCRIPTOR {len(full) if full else 0} Byte = UI_DESC_IN, 6 Interfaces")
    dev.control(setup(0x00, 5, 9, 0, 0)); dev.control(setup(0x00, 9, 1, 0, 0))
    check(dev.control(setup(0x81, 0x0A, 0, 5, 1)) == b"\x00", "GET_INTERFACE IF5 → 0")
    check(dev.control(setup(0x01, 0x0B, 2, 5, 0)) is None, "SET_INTERFACE IF5 alt 2 → STALL")
    check(dev.control(setup(0x01, 0x0B, 1, 4, 0)) == b"", "SET_INTERFACE IF4 alt 1 (Stereo-Ausgang)")
    check(dev.hw.endpt(6) == 0x14, f"ENDPT6 = 0x{dev.hw.endpt(6):02x} (nur IN)")
    check(dev.control(setup(0x01, 0x0B, 1, 5, 0)) == b"", "SET_INTERFACE IF5 alt 1 (Wiedergabe)")
    check(dev.hw.endpt(6) == 0x1C and dev.hw.endpt(7) == 0x14, f"ENDPT6 = 0x{dev.hw.endpt(6):02x} (IN+OUT), ENDPT7 = 0x{dev.hw.endpt(7):02x}")
    check(dev.control(setup(0x81, 0x0A, 0, 5, 1)) == b"\x01", "GET_INTERFACE IF5 → 1")
    check(dev.control(setup(0xA2, 0x81, 0x0100, 0x06, 3)) == bytes.fromhex("44ac00"), "GET_CUR Rate EP 0x06")
    fbp = dev.token(7, 1, 0x9)
    check(fbp is not None and len(fbp) == 3 and int.from_bytes(fbp, "little") == NOM_FB, f"EP 0x87: Rueckmeldung {fbp.hex() if fbp else None} = 10.14 nominal")
    # IF4 parallel: Stereo-Ausgang laeuft weiter (ENDPT6 behaelt OUT)
    check(dev.control(setup(0x01, 0x0B, 0, 4, 0)) == b"" and dev.hw.endpt(6) == 0x1C, "IF4 alt 0 laesst EP6-OUT an (0x1C)")
    dev.control(setup(0x01, 0x0B, 1, 4, 0))

    print("3. Streaming PC → Play (Host folgt der Rueckmeldung), Loopback ueber EP 0x86")
    seq, acc_f, fb = 0, 0.0, NOM_FB / 16384.0
    t_audio, ms, b = 0.0, 0, 100
    bad_m = bad_s = 0; inj_blocks = 0; starts = []; loop_frames = []
    master_seq = []                                         # (L, R) des Mixes in Ausgabereihenfolge
    nxt = [None]                                            # erwarteter USB-Frame des naechsten Blocks (L traegt nur 15 Bit)
    fbs = []
    while b < 100 + 260:
        if ms % 32 == 0:
            p = dev.token(7, 1, 0x9)
            if p: fb = int.from_bytes(p, "little") / 16384.0; fbs.append(fb)
        acc_f += fb; n = int(acc_f); acc_f -= n
        data = b"".join(struct.pack("<hh", *usb_sig(seq + k)) for k in range(n)); seq += n
        r = dev.token(6, 0, 0x1, data)
        if r is None: bad_s += 1000
        q = dev.token(6, 1, 0x9)
        if q: loop_frames += [struct.unpack("<hh", q[o:o + 4]) for o in range(0, len(q), 4)]
        ms += 1
        while t_audio <= ms:
            a, c, capa, capc = both_block(b)
            aL, aR = sums(a[3]); cL, cR = sums(c[3])
            master_seq += list(zip(a[1], a[2]))
            dL = [x - y for x, y in zip(aL, cL)]; dR = [x - y for x, y in zip(aR, cR)]
            if any(dL) or any(dR):
                inj_blocks += 1
                s0 = nxt[0] if nxt[0] is not None else dL[0] + 0x4000
                starts.append(s0); nxt[0] = s0 + 128
                for k in range(128):
                    el, er = usb_sig(s0 + k)
                    if dL[k] != el or dR[k] != er: bad_m += 1
                    m = (el + er) >> 1
                    for fid in (0, 1):
                        orig = struct.unpack_from("<h", capc, fid * 256 + 2 * k)[0]
                        got = struct.unpack_from("<h", capa, fid * 256 + 2 * k)[0]
                        if got != sat16(orig + ((m * 16384) >> 16)): bad_s += 1
            elif capa != capc: bad_s += 1
            b += 1; t_audio += 128 / 44117.647 * 1000
    gaps = sum(1 for x, y in zip(starts, starts[1:]) if y - x != 128)
    st = [dev.r32(UI + 4 * k) for k in range(24)]
    print(f"  {inj_blocks} Bloecke eingespeist, {st[6]} Pakete, Unterlaeufe {st[10]}, Ueberlaeufe {st[9]}, "
          f"Rueckmeldung {min(fbs):.4f}..{max(fbs):.4f} Frames/ms, Zustand {st[3]}")
    check(inj_blocks >= 240, f"{inj_blocks}/260 Bloecke mit USB-Signal (Rest = Vorlauf)")
    check(bad_m == 0 and gaps == 0, f"Master-Summe = 019 + USB L/R, jeder Frame ({bad_m} Fehler), Bloecke lueckenlos ({gaps} Spruenge)")
    check(bad_s == 0, f"Delay/Reverb-Eingang = 019 + 0,25 * USB mono ({bad_s} Fehler)")
    check(st[10] == 0 and st[9] == 0, "keine Unter-/Ueberlaeufe im Ring")
    check(all(abs(f - NOM_FB / 16384.0) < 0.5 for f in fbs), "Rueckmeldung bleibt im Regelbereich (+-0,5 Frames/ms)")
    # Loopback: Stereo-Ausgang = derselbe Mix (eigene Ringlatenz, zusammenhaengendes Stueck)
    mid = len(loop_frames) // 2
    cand = [i for i, v in enumerate(master_seq) if v == loop_frames[mid]]
    okl = len(loop_frames) > 5000 and any(master_seq[i:i + 800] == loop_frames[mid:mid + 800] for i in cand)
    check(okl, f"EP 0x86 (Stereo zum PC) liefert den Mix inkl. USB-Signal ({len(loop_frames)} Frames)")

    print("4. Original-Zusatzeingang der Firmware wird weiter addiert")
    for r in (rig, rref):
        r.dev.uc.mem_write(INCB_BUF, struct.pack("<256h", *[(k * 37) - 2000 for k in range(256)]))
        r.dev.w32(MIX + 0x150, snm["in_cb"] | 1); r.dev.w32(MIX + 0x1DC, 0xC0FFEE)
    ok4 = True
    for i in range(6):
        acc_f += fb * 3; n = int(acc_f); acc_f -= n
        for part in range(3):
            m = n // 3 + (1 if part < n % 3 else 0)
            dev.token(6, 0, 0x1, b"".join(struct.pack("<hh", *usb_sig(seq + k)) for k in range(m))); seq += m
        a, c, _, _ = both_block(b); b += 1
        aL, aR = sums(a[3]); cL, cR = sums(c[3])
        s0 = nxt[0]; nxt[0] += 128
        ok4 &= all(aL[k] - cL[k] == usb_sig(s0 + k)[0] and aR[k] - cR[k] == usb_sig(s0 + k)[1] for k in range(128))
    check(ok4 and dev.r32(INCB_CTX) == 0xC0FFEE and dev.r32(UI + 0x54) >= 6,
          f"6 Bloecke: 019 (mit Zusatzeingang) + USB, ctx durchgereicht, Original {dev.r32(UI + 0x54)}x aufgerufen")
    for r in (rig, rref): r.dev.w32(MIX + 0x150, 0)

    print("5. Mixer ausserhalb von audio_tap (WAV-Export)")
    a, c, capa, capc = both_block(b, via_tap=False); b += 1
    check(a[3] == c[3] and capa == capc, "keine Einspeisung, Summen und Sends wie 019")

    print("6. Stopp, SET_CONFIGURATION, zurueck zu 'Stereo + Multi'")
    check(dev.control(setup(0x01, 0x0B, 0, 5, 0)) == b"" and dev.hw.endpt(6) == 0x14, f"IF5 alt 0: ENDPT6 = 0x{dev.hw.endpt(6):02x}")
    check(dev.token(6, 0, 0x1, b"\x00" * 176) is None and dev.token(7, 1, 0x9) is None, "danach NAK auf EP6-OUT und EP7")
    same = True
    for i in range(4):
        a, c, capa, capc = both_block(b); b += 1
        same &= a[3] == c[3] and capa == capc
    check(same, "4 Bloecke ohne Strom: wie 019")
    dev.control(setup(0x01, 0x0B, 1, 5, 0))
    dev.control(setup(0x00, 9, 1, 0, 0))
    check(dev.control(setup(0x81, 0x0A, 0, 5, 1)) == b"\x00" and dev.r32(UI + 0xC) == 0, "nach SET_CONFIGURATION IF5 → 0, Strom aus")
    bm = dev.r32(0x1FFF1DD0)
    check(bin(bm >> 2).count("1") == 24, f"Paket-Pool 24/30 frei (Bitmap 0x{bm:08x})")
    dev.w32(UM + 4, 2); poll_at(20000); poll_at(20450)
    check(dev.r32(UI + 4) == 0, "Modus 0 nach 'Stereo + Multi'")
    dev.bus_reset()
    full = dev.control(setup(0x80, 6, 0x0200, 0, 0xFFFF))
    check(full is not None and len(full) == 357, f"Deskriptor wieder 357 Byte")
    dev.control(setup(0x00, 5, 9, 0, 0)); dev.control(setup(0x00, 9, 1, 0, 0))
    check(dev.control(setup(0x01, 0x0B, 1, 5, 0)) == b"" and dev.r32(MT + 0xC) == 1, "SET_INTERFACE IF5 alt 1 → Multitrack (mt alt 1)")
    sizes = set()
    for i in range(30):
        if i % 3 == 0: both_block(b); b += 1
        p = dev.token(7, 1, 0x9)
        if p: sizes.add(len(p))
    check(sizes and sizes <= {0, 880, 900}, f"EP7 liefert Multitrack-Pakete {sorted(sizes)}")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
