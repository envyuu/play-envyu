#!/usr/bin/env python3
"""13.11.2026 – Sicherheitsnetz: Weichen fuer alle Code-Patchstellen erzeugen (nach dem Linken).

usage: veneer.py fx.bin SN_VENEERS SN_HDR firmware.bin site=hex ...   → schreibt fx.bin neu, gibt die neuen ARGS aus

Jede Patchstelle, die in den angehaengten Code springt (bl, b.w) oder auf eine Funktion dort zeigt (vtable-Wort, ungerade),
bekommt eine Weiche (24 Byte) im reservierten Bereich SN_VENEERS:
    ldr.w ip, [pc, #16] ; ldr.w ip, [ip] ; cmp.w ip, #0xA5A5A5A5 ; beq.w ORIGINAL ; b.w UNSER_CODE ; .word 0x1FFF9A38
ORIGINAL = Ziel an derselben Stelle im Original-Image. Datenzeiger (gerade), RAM-Zeiger und Bytewerte bleiben unveraendert.
Danach: Kopf {Magic, Laenge, CRC-32 (Feld als 0), Anzahl Weichen} eintragen.
"""
import struct, sys, zlib

BASE = 0xB069C
FLAG = 0x1FFF9A38
NVEN, VSZ = 52, 24                                 # 17.11.: wie SN_NVEN in guard.c
SKIP = {0xAE80C}                                   # init_array → sn_guard direkt (keine Weiche)


def dec_bl(h1, h2):
    """bl / b.w (T4) → (art, Ziel-Offset) oder None"""
    if h1 >> 11 != 0x1E: return None
    if (h2 & 0xD000) == 0xD000: art = "bl"
    elif (h2 & 0xD000) == 0x9000: art = "bw"
    else: return None
    S = (h1 >> 10) & 1; J1 = (h2 >> 13) & 1; J2 = (h2 >> 11) & 1
    off = (S << 24) | ((1 - (J1 ^ S)) << 23) | ((1 - (J2 ^ S)) << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if S: off -= 1 << 25
    return art, off


def enc_b(src, dst, art):
    """bl / b.w (T4) von src nach dst"""
    off = dst - (src + 4)
    assert -(1 << 24) <= off < (1 << 24) and not off & 1, (hex(src), hex(dst))
    S = (off >> 24) & 1; I1 = (off >> 23) & 1; I2 = (off >> 22) & 1
    J1 = (1 - I1) ^ S; J2 = (1 - I2) ^ S
    h1 = 0xF000 | (S << 10) | ((off >> 12) & 0x3FF)
    h2 = (0xD000 if art == "bl" else 0x9000) | (J1 << 13) | (J2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", h1, h2)


def enc_beq(src, dst):
    """beq.w (T3, ±1 MB)"""
    off = dst - (src + 4)
    assert -(1 << 20) <= off < (1 << 20) and not off & 1, (hex(src), hex(dst))
    S = (off >> 20) & 1; J2 = (off >> 19) & 1; J1 = (off >> 18) & 1
    h1 = 0xF000 | (S << 10) | (0 << 6) | ((off >> 12) & 0x3F)       # cond 0 = EQ
    h2 = 0x8000 | (J1 << 13) | (J2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", h1, h2)


def enc_movwt(rd, val):
    """movw rd,#lo ; movt rd,#hi"""
    def one(op, imm):
        i = (imm >> 11) & 1; imm4 = (imm >> 12) & 0xF; imm3 = (imm >> 8) & 7; imm8 = imm & 0xFF
        return struct.pack("<HH", op | (i << 10) | imm4, (imm3 << 12) | (rd << 8) | imm8)
    return one(0xF240, val & 0xFFFF) + one(0xF2C0, val >> 16)


def stub(site, fw):
    """Original-Befehle der Stelle (4 Byte) ausserhalb des Originals ausfuehren, dann bx lr.
    ldr Rt,[pc,#imm] (T1) wird zu movw/movt mit dem Literalwert; alles andere wird kopiert (PC-relativ → Fehler)."""
    out, k = b"", 0
    while k < 4:
        h = struct.unpack_from("<H", fw, site + k)[0]
        if h >> 11 in (0x1D, 0x1E, 0x1F):              # 32-Bit-Befehl
            h2 = struct.unpack_from("<H", fw, site + k + 2)[0]
            assert not (h == 0xF8DF or (h & 0xFF7F) == 0xF85F), f"{site + k:#x}: PC-relativer 32-Bit-Befehl"
            assert not (h >> 11 == 0x1E and h2 & 0x8000), f"{site + k:#x}: Sprung/Verzweigung"
            out += struct.pack("<HH", h, h2); k += 4
        elif h >> 11 == 0x09:                          # ldr Rt, [pc, #imm8*4]
            rt = (h >> 8) & 7; lit = ((site + k + 4) & ~3) + (h & 0xFF) * 4
            out += enc_movwt(rt, struct.unpack_from("<I", fw, lit)[0]); k += 2
        else:
            assert not (h >> 12 == 0xD or h >> 11 == 0x1C or (h & 0xF500) == 0xB100 or (h & 0xFF00) == 0x4700
                        or h >> 11 == 0x14 or (h & 0xFF78) == 0x4478), f"{site + k:#x}: Sprung oder PC-relativ ({h:#06x})"
            out += struct.pack("<H", h); k += 2
    return out + struct.pack("<H", 0x4770)             # bx lr


def veneer(v, orig, real):
    code = bytes.fromhex("dff810c0")                 # ldr.w ip, [pc, #16]
    code += bytes.fromhex("dcf800c0")                # ldr.w ip, [ip]
    code += bytes.fromhex("bcf1a53f")                # cmp.w ip, #0xA5A5A5A5
    code += enc_beq(v + 12, orig)
    code += enc_b(v + 16, real, "bw")
    code += struct.pack("<I", FLAG)
    assert len(code) == VSZ
    return code


def main():
    fxp, ven, hdr, fwp = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
    fx = bytearray(open(fxp, "rb").read()); fw = open(fwp, "rb").read()
    end = BASE + len(fx)
    out, table, cur = [], {}, [0]
    for a in sys.argv[5:]:
        site_s, val = a.split("="); site = int(site_s, 16); b = bytes.fromhex(val)
        new = None
        if site not in SKIP and len(b) == 4:
            h1, h2 = struct.unpack("<HH", b)
            d = dec_bl(h1, h2)
            if d and BASE <= site + 4 + d[1] < end:
                o1, o2 = struct.unpack_from("<HH", fw, site); od = dec_bl(o1, o2)
                if od and od[0] == d[0]: key = (site + 4 + d[1], site + 4 + od[1])
                else:                                   # Hook ersetzt Original-Befehle (kein Aufruf): Stub fuehrt sie aus
                    assert d[0] == "bl", f"{site:#x}: b.w ueber Nicht-Sprung"
                    key = (site + 4 + d[1], ("stub", site))
                new = ("b", d[0], key)
            else:
                w = struct.unpack("<I", b)[0]
                if w & 1 and BASE <= w < end:
                    ow = struct.unpack_from("<I", fw, site)[0]
                    assert ow & 1 and ow < BASE, f"{site:#x}: Original {ow:#x} ist kein Funktionszeiger"
                    new = ("p", None, (w & ~1, ow & ~1))
        if not new: out.append(a); continue
        kind, art, key = new
        if key not in table:
            v = ven + cur[0]
            orig = key[1]
            if isinstance(orig, tuple):                 # Stub direkt hinter der Weiche
                sb = stub(orig[1], fw); sa = v + VSZ
                fx[sa - BASE:sa - BASE + len(sb)] = sb
                orig = sa; size = VSZ + ((len(sb) + 3) & ~3)
            else: size = VSZ
            assert cur[0] + size <= NVEN * VSZ, "Weichenbereich zu klein"
            table[key] = v; cur[0] += size
            fx[v - BASE:v - BASE + VSZ] = veneer(v, orig, key[0])
        v = table[key]
        out.append(f"{site_s}={(enc_b(site, v, art) if kind == 'b' else struct.pack('<I', v | 1)).hex()}")
    h = hdr - BASE
    struct.pack_into("<IIII", fx, h, 0x564E4553, len(fx), 0, len(table))
    crc = zlib.crc32(bytes(fx)) & 0xFFFFFFFF          # Feld crc ist hier noch 0
    struct.pack_into("<I", fx, h + 8, crc)
    open(fxp, "wb").write(fx)
    print(" ".join(out))
    sys.stderr.write(f"veneer.py: {len(table)} Weichen, Kopf Laenge {len(fx)}, CRC {crc:08x}\n")


if __name__ == "__main__":
    main()


def follow(img, addr):
    """Fuer Tests: zeigt addr auf eine Weiche, das echte Ziel (b.w am Weichen-Offset 16) zurueckgeben, sonst addr."""
    a = addr & ~1
    try: head = bytes(img[a + i] for i in range(12))
    except (KeyError, IndexError): return addr
    if head != bytes.fromhex("dff810c0dcf800c0bcf1a53f"): return addr
    h1, h2 = (img[a + 16] | img[a + 17] << 8), (img[a + 18] | img[a + 19] << 8)
    d = dec_bl(h1, h2)
    return (a + 20 + d[1]) | (addr & 1) if d else addr
