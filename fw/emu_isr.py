#!/usr/bin/env python3
"""Maschinencode-Test des Stereo-Teils (Patch 012) auf dem Image von Patch 015: fuehrt den ECHTEN Teensy-USB-Code des gepatchten Rev-A-Images
(usbinit_hook → usb_init, usbotg_isr 0x12B9C mit den Umleitungen, audio_tap → audio_callback 0xBA18)
in einem Cortex-M4-Emulator (unicorn) aus. Die USB-FS-Hardware (ISTAT/STAT/CTL/ENDPTn, BDT-Ping-Pong,
Token) wird in Python nachgebildet, der "Host" spielt eine Linux-artige Enumeration und streamt.

usage: emu_isr.py PolyendPlay_v1.6.0_multitrack_ENTWURF.ppf [firmware.bin]
(015: Stereo-Teil aus 012 auf dem 015-Image; Multitrack-Teil: emu_mt.py)
Braucht: pip install unicorn  (z. B. in einer venv)
"""
import sys, struct, os
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UcError
from unicorn.arm_const import *

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_ppf import images  # noqa: E402

CFG_LEN = 357                     # 015: Konfigurationsdeskriptor
DESC_RAM = 0x1FFFD4C0
STOP = 0x000FFF00                 # Rueckkehradresse (unbenutzter Flash), Emulation endet dort
BDT = 0x1FFF0000
USB0 = 0x40072000
REG = dict(ISTAT=0x80, INTEN=0x84, ERRSTAT=0x88, STAT=0x90, CTL=0x94, ADDR=0x98)
fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


class UsbHw:
    """minimales USB-FS-Modell (K66 Kap. 'USB OTG'): Register, BDT-Ping-Pong, Token → TOKDNE."""
    def __init__(self):
        self.r = bytearray(0x1000)
        self.odd = {}                               # (ep, tx) -> 0/1
        self.stall_seen = False
    def read(self, uc, off, size, ud):
        return int.from_bytes(self.r[off:off + size], "little")
    def write(self, uc, off, size, val, ud):
        if off == REG["ISTAT"]:                     # write-1-to-clear
            self.r[off] &= ~val & 0xFF; return
        if off == REG["CTL"] and val & 0x02:        # ODDRST
            self.odd = {}
        if off == 0xC0 and (val & 0x02): self.stall_seen = True     # ENDPT0 EPSTALL
        self.r[off:off + size] = val.to_bytes(size, "little")
    def endpt(self, n): return self.r[0xC0 + 4 * n]


class Dev:
    def __init__(self, img):
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x0, 0x100000)
        uc.mem_write(0, bytes(img))
        uc.mem_write(STOP, b"\xfe\xe7" * 8)           # b .
        uc.mem_map(0x1FFF0000, 0x10000)
        uc.mem_map(0x20000000, 0x30000)
        uc.mem_map(0x40000000, 0x72000)
        self.hw = UsbHw()
        uc.mmio_map(USB0, 0x1000, self.hw.read, None, self.hw.write, None)
        uc.mem_map(0x40073000, 0x8D000)
        uc.mem_map(0xE0000000, 0x100000)
        uc.mem_write(0x40020000, b"\x80")             # FTFE FSTAT.CCIF = 1 (usb_init_serialnumber)
        # wie ResetHandler: .data kopieren (0xAEC30 → 0x1FFF1CA0, 0x1A6C Byte), .bss ist 0
        uc.mem_write(0x1FFF1CA0, bytes(img[0xAEC30:0xAEC30 + 0x1A6C]))
        # SRAM_L ab 0x1FFFC000 mit Muell (wie nach dem Einschalten)
        uc.mem_write(0x1FFFC000, bytes((i * 37 + 11) & 0xFF for i in range(0x3F00)))
        self.uc = uc
        self.sp = 0x2002F000
    def call(self, addr, *args, limit=2_000_000):
        uc = self.uc
        for i, a in enumerate(args): uc.reg_write(UC_ARM_REG_R0 + i, a)
        uc.reg_write(UC_ARM_REG_SP, self.sp)
        uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        try:
            uc.emu_start(addr | 1, STOP, count=limit)
        except UcError as e:
            raise RuntimeError(f"Emulation abgebrochen bei pc=0x{uc.reg_read(UC_ARM_REG_PC):x}: {e}")
        pc = uc.reg_read(UC_ARM_REG_PC)
        if pc != STOP: raise RuntimeError(f"nicht zurueckgekehrt (pc=0x{pc:x}, Limit?)")
        return uc.reg_read(UC_ARM_REG_R0)
    def r32(self, a): return struct.unpack("<I", self.uc.mem_read(a, 4))[0]
    def w32(self, a, v): self.uc.mem_write(a, struct.pack("<I", v))
    def isr(self): self.call(0x12B9C)

    # ---- Host-Seite ----
    def token(self, ep, tx, pid, data=b""):
        """Ein Token ausfuehren. tx=1: IN (Geraet sendet). Rueckgabe: gesendete Daten (IN) oder None bei NAK."""
        odd = self.hw.odd.get((ep, tx), 0)
        idx = ep * 4 + tx * 2 + odd
        d = self.r32(BDT + 8 * idx); addr = self.r32(BDT + 8 * idx + 4)
        if not d & 0x80: return None                  # nicht OWN → NAK
        if tx:
            n = d >> 16
            out = bytes(self.uc.mem_read(addr, n)) if n else b""
        else:
            n = len(data); assert n <= (d >> 16), "Puffer zu klein"
            if n: self.uc.mem_write(addr, data)
            out = b""
        self.w32(BDT + 8 * idx, (d & 0x40) | (n << 16) | (pid << 2))   # OWN weg, BC, TOK_PID
        self.hw.odd[(ep, tx)] = odd ^ 1
        self.hw.r[REG["STAT"]] = (ep << 4) | (tx << 3) | (odd << 2)
        self.hw.r[REG["ISTAT"]] |= 0x08
        self.isr()
        return out
    def bus_reset(self):
        self.hw.r[REG["ISTAT"]] |= 0x01
        self.isr()
    def control(self, setup, data_out=b""):
        """Kontrolltransfer: SETUP, Datenstufe, Status. Rueckgabe (daten|None bei STALL)."""
        self.hw.stall_seen = False
        self.token(0, 0, 0xD, setup)
        if self.hw.stall_seen: return None
        bmrt, wl = setup[0], setup[6] | setup[7] << 8
        if bmrt & 0x80:
            got = b""
            while True:
                p = self.token(0, 1, 0x9)
                if p is None: raise RuntimeError("EP0 IN NAK")
                got += p
                if len(p) < 64 or len(got) >= wl: break
            self.token(0, 0, 0x1, b"")                   # Status OUT
            return got
        if data_out: self.token(0, 0, 0x1, data_out)
        p = self.token(0, 1, 0x9)                       # Status IN (ZLP)
        if p is None: raise RuntimeError("keine Statusstufe")
        return p


def setup(bmrt, breq, wv, wi, wl): return struct.pack("<BBHHH", bmrt, breq, wv, wi, wl)


def main():
    ppf = sys.argv[1]
    img = bytearray(max(images(ppf)[0]) + 1)
    for a, v in images(ppf)[0].items(): img[a] = v
    nm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " +
          os.path.join(os.path.dirname(os.path.abspath(__file__)), "fx.elf")) if len(l.split()) == 3}
    dev = Dev(img)
    print("Start: usbinit_hook (ua_init + Original-usb_init)")
    dev.call(nm["usbinit_hook"])
    check(dev.r32(0x1FFFD000) == 0x55A0D10A, "Zustand initialisiert (magic)")
    desc = bytes(img[nm["ua_cfg_desc"] - 0:nm["ua_cfg_desc"] + CFG_LEN])
    check(bytes(dev.uc.mem_read(DESC_RAM, CFG_LEN)) == desc, f"Deskriptor im RAM bei 0x{DESC_RAM:08X}")
    pool0 = dev.r32(0x1FFF1DD0)

    print("Enumeration")
    dev.bus_reset()
    check(dev.hw.endpt(0) == 0x0D, "nach Reset ENDPT0 = 0x0D")
    dd = dev.control(setup(0x80, 6, 0x0100, 0, 64))
    check(dd is not None and dd[:4] == bytes.fromhex("12011001") and dd[8:12] == bytes.fromhex("d016a00e"), f"Device-Deskriptor {dd.hex() if dd else None}")
    cd = dev.control(setup(0x80, 6, 0x0200, 0, 9))
    check(cd is not None and cd[2] | cd[3] << 8 == CFG_LEN and cd[4] == 6, f"Config-Kopf: wTotalLength {cd[2] | cd[3] << 8 if cd else '-'}, {cd[4] if cd else '-'} Interfaces")
    cd = dev.control(setup(0x80, 6, 0x0200, 0, 255))
    check(cd == desc[:255], f"Config-Deskriptor mit wLength 255 (Windows) → {len(cd) if cd else 0} Byte")
    cd = dev.control(setup(0x80, 6, 0x0200, 0, 0xFFFF))
    check(cd == desc, f"vollstaendiger Config-Deskriptor ({len(cd) if cd else 0} Byte) = ua_cfg_desc")
    dev.control(setup(0x00, 5, 7, 0, 0))               # SET_ADDRESS 7
    check(dev.hw.r[REG["ADDR"]] == 7, "SET_ADDRESS")
    dev.control(setup(0x00, 9, 1, 0, 0))               # SET_CONFIGURATION 1
    check(dev.r32(0x20000D67) & 0xFF == 1, "usb_configuration = 1")
    check([dev.hw.endpt(n) for n in range(1, 8)] == [0x15, 0x19, 0x15, 0x15, 0x19, 0x15, 0x19], "ENDPT1..7 wie Original")
    check(dev.control(setup(0x01, 0x0B, 0, 0, 0)) is None, "SET_INTERFACE IF0 → STALL wie bisher")
    check(dev.control(setup(0x21, 0x0A, 0, 3, 0)) is None, "HID SET_IDLE an IF3 → STALL")
    check(dev.control(setup(0x01, 0x0B, 0, 4, 0)) == b"", "SET_INTERFACE AS alt 0 → Status-ZLP")
    alt = dev.control(setup(0x81, 0x0A, 0, 4, 1))
    check(alt == b"\x00", f"GET_INTERFACE → {alt}")
    lc = dev.control(setup(0x21, 0x20, 0, 0, 7), bytes.fromhex("00c20100000008"))
    check(lc == b"" and dev.r32(0x20000A78) == 115200, "CDC SET_LINE_CODING 115200 funktioniert weiter")
    r = dev.control(setup(0x22, 0x01, 0x0100, 0x86, 3), bytes.fromhex("44ac00"))
    check(r == b"", "SET_CUR Abtastrate (mit Datenstufe)")
    r = dev.control(setup(0xA2, 0x81, 0x0100, 0x86, 3))
    check(r == bytes.fromhex("44ac00"), f"GET_CUR Abtastrate → {r.hex() if r else None}")

    print("Streaming")
    check(dev.control(setup(0x01, 0x0B, 1, 4, 0)) == b"", "SET_INTERFACE AS alt 1")
    check(dev.hw.endpt(6) == 0x14, f"ENDPT6 = 0x{dev.hw.endpt(6):02x} (isochron, ohne Handshake)")
    # Audio: L/R-Puffer des DMA-Treibers (0x20000134 / 0x20000334) mit Zaehlermuster, audio_tap wie aus audio_dma_isr
    s = 0
    def audio_block():
        nonlocal s
        L = b"".join(struct.pack("<h", ((s + k) & 0x7FFF)) for k in range(128))
        R = b"".join(struct.pack("<h", (-((s + k) & 0x7FFF))) for k in range(128))
        dev.uc.mem_write(0x20000134, L); dev.uc.mem_write(0x20000334, R)
        dev.call(nm["audio_tap"], 0x20000134, 0x20000334)
        s += 128
    recv, sizes, nak = [], [], 0
    t_audio = 0.0
    for ms in range(400):
        while t_audio <= ms:
            audio_block(); t_audio += 128 / 44117.647 * 1000
        p = dev.token(6, 1, 0x9)
        if p is None: nak += 1; continue
        sizes.append(len(p)); recv += [struct.unpack("<hh", p[i:i + 4]) for i in range(0, len(p), 4)]
    st = [dev.r32(0x1FFFD000 + 4 * k) for k in range(18)]
    check(nak == 0, f"jedes IN-Token bedient ({nak} NAK)")
    nz = [x for x in sizes if x]
    check(len(nz) > 300 and set(nz) <= {176, 180}, f"Paketgroessen {sorted(set(sizes))} (0 nur beim Vorfuellen)")
    ok = all(r[0] == ((recv[0][0] + i) & 0x7FFF) and r[1] == -r[0] for i, r in enumerate(recv))
    check(ok, f"{len(recv)} Frames lueckenlos, L/R richtig verschraenkt")
    check(st[4] == 2 and st[8] == 0 and st[6] == 0, f"Zustand LAEUFT, Unterlaeufe {st[8]}, verworfen {st[6]}, Fuellstand {(st[1] - st[2]) & 0xFFFFFFFF}")
    # CDC weiterhin: OUT-Paket auf EP2 landet in der Teensy-RX-Warteschlange
    q0 = dev.r32(0x20000CE8 + 4 * 1)
    dev.token(2, 0, 0x1, b"PING\n")
    q1 = dev.r32(0x20000CE8 + 4 * 1)
    check(q0 == 0 and q1 != 0 and struct.unpack("<H", dev.uc.mem_read(q1, 2))[0] == 5, "CDC-RX (EP2) laeuft weiter ueber den Original-Pfad")
    # MIDI-RX (EP5)
    dev.token(5, 0, 0x1, bytes.fromhex("09903c64"))
    check(dev.r32(0x20000CE8 + 4 * 4) != 0, "MIDI-RX (EP5) laeuft weiter")
    # CDC-TX (EP3): usb_malloc + usb_tx aus dem Hauptprogramm, Host holt per IN ab → Abschluss ueber ep_hook-Normalpfad
    free0 = bin(dev.r32(0x1FFF1DD0) >> 2).count("1")
    pk = dev.call(0x11F20)
    dev.uc.mem_write(pk, struct.pack("<HH", 4, 0)); dev.uc.mem_write(pk + 8, b"OK\r\n")
    dev.call(0x12B14, 3, pk)
    got = dev.token(3, 1, 0x9)
    free1 = bin(dev.r32(0x1FFF1DD0) >> 2).count("1")
    check(got == b"OK\r\n" and free1 == free0, f"CDC-TX (EP3): {got}, Paket zurueck im Pool ({free0}→{free1})")
    check(dev.control(setup(0x01, 0x0B, 0, 4, 0)) == b"", "SET_INTERFACE alt 0 (Stopp)")
    check(dev.token(6, 1, 0x9) is None and dev.token(6, 1, 0x9) is None, "nach Stopp NAK auf EP6")
    # erneute Konfiguration: Original gibt BDT-Puffer frei – unsere (ausserhalb des Pools) duerfen nicht in den Pool
    dev.control(setup(0x01, 0x0B, 1, 4, 0))
    dev.control(setup(0x00, 9, 1, 0, 0))
    bm = dev.r32(0x1FFF1DD0)
    # Teensy: Bitmap startet mit 0xFFFFFFFF (Bits 1..0 = Index 30/31, nie vergeben). Nach SET_CONFIGURATION genau
    # 6 Pakete belegt (EP2, EP5, EP7 je 2 RX-Puffer), alle Warteschlangen geleert → kein Leck, keine Fremdpuffer im Pool
    free = bin(bm >> 2).count("1")
    check(free == 24, f"Paket-Pool nach SET_CONFIGURATION: {free}/30 frei (erwartet 24), Bitmap 0x{bm:08x}")
    alt = dev.control(setup(0x81, 0x0A, 0, 4, 1))
    check(alt == b"\x00", "nach SET_CONFIGURATION meldet GET_INTERFACE alt 0")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
