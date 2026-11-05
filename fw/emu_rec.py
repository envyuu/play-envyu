#!/usr/bin/env python3
"""Maschinencode-Test der Aufnahme (Patch 024/025): fuehrt den ECHTEN uebersetzten Code von usbrec.c (aus dem Image)
im Cortex-M4-Emulator aus und faengt alle Aufrufe in die FatFs-Funktionen der Firmware ab. Geprueft wird die
Aufrufkonvention (Register) jedes Aufrufs – genau die Fehlerklasse von 024 (f_lseek bekam 32 statt 64 Bit → Freeze):
  f_mkdir(path) · f_open(fil, path, mode) · f_write(fil, buf, n, *bw) · f_lseek(fil, ofs64 in r2:r3) · f_close(fil)
Dazu: Dateiinhalt (Kopf + Samples) wird aus den abgefangenen Schreibaufrufen zusammengesetzt und geprueft.

usage: emu_rec.py PolyendPlay_v1.6.0_rec_fix.ppf
"""
import sys, struct, os
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_LR
from unicorn import UC_HOOK_CODE
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_isr import Dev  # noqa: E402
from check_ppf import images  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIL, REC, RING = 0x1FFFD630, 0x1FFFD880, 0x1FFFDE00
UI = 0x1FFF9000
MILLIS = 0x20000D7C
BUF = 0x20010000
F = {"f_open": 0xFB38, "f_write": 0x10088, "f_lseek": 0x10570, "f_close": 0x104F4, "f_mkdir": 0x10A78}

fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


def cstr(dev, a):
    b = bytes(dev.uc.mem_read(a, 128)); return b[:b.index(0)].decode(errors="replace")


def main():
    ppf = sys.argv[1]
    im = images(ppf)[0]
    img = bytearray(max(im) + 1)
    for a, v in im.items(): img[a] = v
    nm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(HERE, "fx.elf")) if len(l.split()) == 3}
    dev = Dev(img); uc = dev.uc
    calls = []
    files = {}                     # Pfad → bytearray
    state = {"open": None, "pos": 0, "exist": {"Samples/Live Recordings/Rec_001.wav"}}

    def ret(v):
        uc.reg_write(UC_ARM_REG_R0, v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def hook(name):
        def h(uc_, addr, size, ud):
            r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
            calls.append((name, r))
            if name == "f_mkdir": return ret(8)                              # FR_EXIST
            if name == "f_open":
                p = cstr(dev, r[1])
                if p in state["exist"]: return ret(8)
                files[p] = bytearray(); state["open"] = p; state["pos"] = 0; return ret(0)
            if name == "f_write":
                d = bytes(uc.mem_read(r[1], r[2])); f = files[state["open"]]; p = state["pos"]
                if len(f) < p + len(d): f.extend(bytes(p + len(d) - len(f)))
                f[p:p + len(d)] = d; state["pos"] = p + len(d)
                dev.w32(r[3], r[2]); return ret(0)
            if name == "f_lseek":
                ofs = r[2] | (r[3] << 32)
                if ofs > len(files[state["open"]]): return ret(1)             # wuerde die Datei verlaengern
                state["pos"] = ofs; return ret(0)
            if name == "f_close": state["open"] = None; return ret(0)
        return h
    for n, a in F.items(): uc.hook_add(UC_HOOK_CODE, hook(n), begin=a, end=a)

    print("Start: rec_init, Variante 'Stereo + In'")
    dev.call(nm["rec_init"])
    dev.w32(UI, 0x55A0D120); dev.w32(UI + 4, 1)
    check(dev.r32(REC) == 0x52454324, "rec_t initialisiert")
    check(dev.call(nm["rec_request"], 1) == 1, "rec_request(1) angenommen")
    dev.w32(MILLIS, 1000)
    dev.call(nm["rec_poll"])
    opens = [r for n, r in calls if n == "f_open"]
    check(len(opens) == 2 and cstr(dev, opens[-1][1]) == "Samples/Live Recordings/Rec_002.wav" and opens[-1][0] == FIL and opens[-1][2] == 6,
          f"f_open(FIL, 'Samples/Live Recordings/Rec_002.wav', CREATE_NEW|WRITE) nach belegtem 001 ({[cstr(dev, r[1]) for r in opens]})")
    check(dev.r32(REC + 4) == 2 and dev.r32(REC + 0x14) == 2, "nimmt auf, Nummer 2")
    # 40 Bloecke einspeisen (L = k, R = 3k), dabei schreiben lassen
    seq = 0
    for b in range(40):
        uc.mem_write(BUF, struct.pack("<128h", *[((seq + k) * 7) % 30000 - 15000 for k in range(128)]))
        uc.mem_write(BUF + 0x100, struct.pack("<128h", *[((seq + k) * 11) % 30000 - 15000 for k in range(128)]))
        dev.call(nm["rec_capture"], BUF, BUF + 0x100)
        seq += 128
        if b % 4 == 3: dev.call(nm["rec_poll"])
    check(dev.call(nm["rec_request"], 0) == 1, "rec_request(0) angenommen")
    dev.call(nm["rec_poll"])
    check(dev.r32(REC + 4) == 0 and dev.r32(REC + 0x20) == 1 and struct.unpack("<i", uc.mem_read(REC + 0x18, 4))[0] == 0,
          f"fertig: Zustand 0, 1 Datei, Fehler {struct.unpack('<i', uc.mem_read(REC + 0x18, 4))[0]}")
    ls = [r for n, r in calls if n == "f_lseek"]
    check(len(ls) == 1 and ls[0][0] == FIL and ls[0][2] == 0 and ls[0][3] == 0,
          f"f_lseek(FIL, 0): r0 0x{ls[0][0]:08x}, Offset r2:r3 = 0x{ls[0][3]:08x}{ls[0][2]:08x}" if ls else "f_lseek aufgerufen")
    ws = [r for n, r in calls if n == "f_write"]
    check(all(r[0] == FIL and RING <= r[1] < RING + 0x2000 or r[2] == 44 for r in ws), f"f_write: FIL, Puffer im Ring bzw. Kopf ({len(ws)} Aufrufe)")
    check(sum(1 for n, r in calls if n == "f_close") == 1, "f_close genau einmal")
    d = files["Samples/Live Recordings/Rec_002.wav"]
    n = (len(d) - 44) // 2
    hdr_ok = d[:4] == b"RIFF" and struct.unpack_from("<I", d, 4)[0] == 36 + 2 * n and struct.unpack_from("<I", d, 40)[0] == 2 * n
    exp = [(((k * 7) % 30000 - 15000) + ((k * 11) % 30000 - 15000)) >> 1 for k in range(40 * 128)]
    got = list(struct.unpack_from(f"<{n}h", d, 44))
    check(hdr_ok and n == 40 * 128 and got == exp, f"Datei: Kopf passt, {n} Samples = (L+R)/2 lueckenlos")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
