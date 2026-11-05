#!/usr/bin/env python3
"""Prueft die gebaute .ppf (Patch 015) gegen Patch 012 und das Original.

usage: check_ppf.py NEU.ppf BASIS_012.ppf firmware.bin fx.bin
 - Rev-B-Image (Teil 2) byte-identisch mit der Basis
 - Rev-A-Image: lueckenlos 0 .. Ende, angehaengter Code == fx.bin ab 0xB069C
 - im Original-Bereich (< 0xB069C) unterscheidet sich NEU von 012 nur an
     * den neuen Patch-015-Stellen (0x158D6, 0x15CBC), die vorher Original waren (keine Kollision),
     * den Patch-012-Stellen (BL-Ziele/Deskriptoreintrag, weil der angehaengte Code gewachsen ist),
     * Namenszeigern der FX-Suite (011) auf verschobene, gleichlautende Texte
 - Flash-Konfiguration 0x400-0x40F unveraendert
"""
import sys

def images(fn):
    imgs, cur, base = [], {}, 0
    for line in open(fn, "rb").read().split(b"\n"):
        line = line.strip()
        if not line.startswith(b":"): continue
        r = bytes.fromhex(line[1:].decode())
        assert sum(r) & 0xFF == 0, "Pruefsumme"
        n, a, t = r[0], (r[1] << 8) | r[2], r[3]
        if t == 0:
            for k in range(n): cur[base + a + k] = r[4 + k]
        elif t == 2: base = int.from_bytes(r[4:6], "big") << 4
        elif t == 4: base = int.from_bytes(r[4:6], "big") << 16
        elif t == 1: imgs.append(cur); cur, base = {}, 0
    if cur: imgs.append(cur)
    return imgs

def ranges(addrs):
    out = []
    for a in sorted(addrs):
        if out and out[-1][1] == a: out[-1][1] = a + 1
        else: out.append([a, a + 1])
    return out

def main():
    new, base = images(sys.argv[1]), images(sys.argv[2])
    fw, fx = open(sys.argv[3], "rb").read(), open(sys.argv[4], "rb").read()
    bad = 0
    def fail(m):
        nonlocal bad; bad += 1; print("  FEHLER", m)
    print(f"  Images: neu {len(new)}, Basis {len(base)}")
    if len(new) < 2 or new[1] != base[1]: fail("Rev-B-Image weicht ab")
    else: print("  ok    Rev-B-Image identisch")
    A = new[0]
    hi = max(A) + 1
    missing = [a for a in range(0, hi) if a not in A]
    if missing: fail(f"Luecken im Rev-A-Image: {ranges(missing)[:5]}")
    else: print(f"  ok    Rev-A-Image lueckenlos 0x0..0x{hi:X}")
    if bytes(A[a] for a in range(0xB069C, hi)) != fx: fail("angehaengter Code != fx.bin")
    else: print(f"  ok    angehaengter Code = fx.bin ({len(fx)} Byte, Ende 0x{hi:X}, frei bis 64-KB-Segmentende: {0xC0000-hi} Byte)")
    if any(A[a] != fw[a] for a in range(0x400, 0x410)): fail("Flash-Konfiguration veraendert!")
    diff012 = [a for a in range(0, 0xB069C) if A[a] != base[0][a]]
    diff011 = [a for a in range(0, 0xB069C) if base[0][a] != fw[a]]
    allowed = set()        # neue 015-Stellen: muessen in der Basis noch Original sein
    for a, n in ((0x158D6, 4), (0x15CBC, 4)): allowed.update(range(a, a + n))
    retarget = set()       # 012-Stellen: duerfen sich aendern (neue Codeadressen, laengerer Deskriptor)
    for a, n in ((0xBD58, 4), (0x13810, 4), (0x12BAC, 4), (0x12DEC, 4), (0x9BAD0, 6)): retarget.update(range(a, a + n))
    # 011-Stellen duerfen sich aendern, wenn sie (verschobene) Zeiger auf denselben Text im angehaengten Code sind
    def cstr(img, p):
        out = bytearray()
        while p in img and img[p] and len(out) < 64: out.append(img[p]); p += 1
        return bytes(out)
    moved = set()
    for w in sorted({a & ~3 for a in diff012 if a not in allowed and a not in retarget and a in set(diff011)}):
        nv = int.from_bytes(bytes(A[w + k] for k in range(4)), "little")
        ov = int.from_bytes(bytes(base[0][w + k] for k in range(4)), "little")
        if 0xB069C <= nv < hi and 0xB069C <= ov and cstr(A, nv) and cstr(A, nv) == cstr(base[0], ov):
            moved.update(range(w, w + 4))
    if moved: print(f"  ok    {len(moved)//4} Namenszeiger (011) auf verschobene, gleichlautende Texte")
    if moved: pass
    extra = [a for a in diff012 if a not in allowed and a not in retarget and a not in moved]
    if extra: fail(f"unerwartete Aenderungen gegenueber 012: {[(hex(x), hex(y)) for x, y in ranges(extra)]}")
    else: print("  ok    gegenueber 012 nur: " + ", ".join(f"0x{x:X}-0x{y-1:X}" for x, y in ranges(sorted(set(diff012) - moved))))
    if set(diff011) & allowed: fail("Patch-015-Stelle kollidiert mit 011/012")
    else: print("  ok    015-Stellen 0x158D6/0x15CBC sind in 012 noch Original")
    for a in sorted(allowed):
        if a not in diff012: fail(f"015-Stelle 0x{a:X} nicht geaendert")
    for a, n, what in ((0x9AD40, 4, "USB-MIDI-Hook 011"), (0x9AA8C, 4, "DIN-Hook 011"), (0x46FFE, 4, "Tick 011"),
                       (0x47010, 4, "Tick 011"), (0x47054, 4, "Tick 011"), (0xBD70, 4, "Loop 011"), (0xAA324, 4, "fremd: MidiTrack_playChord")):
        if any(A[a + k] != base[0][a + k] for k in range(n)): fail(f"{what} 0x{a:X} weicht von 012 ab")
    print("  ok    011-Hooks (0x9AD40, 0x9AA8C, 0x46FFE/0x47010/0x47054, 0xBD70) und 0xAA324 wie in 012")
    print("  info  012 gegenueber Original: " + ", ".join(f"0x{x:X}" + (f"-0x{y-1:X}" if y - x > 1 else "") for x, y in ranges(diff011)))
    print("PPF-PRUEFUNG:", "OK" if not bad else f"{bad} Fehler"); sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
