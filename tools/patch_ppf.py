#!/usr/bin/env python3
"""Patch bytes in one image of a Polyend .ppf (concatenated Intel-HEX files).

usage: patch_ppf.py IN.ppf OUT.ppf [--image N] [--append ADDR=FILE.bin] ADDR=HEXBYTES [ADDR=HEXBYTES ...]
  --image N   which HEX image to patch (1 = Rev A / K66, 2 = Rev B / i.MX RT), default 1
  ADDR        absolute address (hex), e.g. a44f4
  HEXBYTES    new bytes, e.g. 4445425547
  --append    add NEW data (e.g. compiled code) at ADDR; must not overlap existing data and
              must lie in the same 64 KiB segment as the image's last data record

Only the data records covering the patched bytes are rewritten (new checksum),
every other byte of the file - including line endings and the other image - stays identical.
Refuses to touch the K66 flash configuration field 0x400-0x40F.
"""
import os, sys

FLASH_CONFIG = range(0x400, 0x410)


def main():
    args = sys.argv[1:]
    image = 1
    if "--image" in args:
        i = args.index("--image")
        image = int(args[i + 1])
        del args[i:i + 2]
    appends = []
    while "--append" in args:
        i = args.index("--append")
        a, f = args[i + 1].split("=")
        appends.append((int(a, 16), open(f, "rb").read()))
        del args[i:i + 2]
    src, dst, specs = args[0], args[1], args[2:]
    patches = {}
    for s in specs:
        a, h = s.split("=")
        a = int(a, 16)
        for k, b in enumerate(bytes.fromhex(h)):
            patches[a + k] = b
    if image == 1 and any(a in FLASH_CONFIG for a in patches):
        sys.exit("REFUSED: patch touches flash configuration 0x400-0x40F")

    raw = open(src, "rb").read()
    lines = raw.split(b"\n")  # keep '\r' inside each line

    # pass 1: collect existing data addresses of the target image (for overlap check)
    existing, cur_image, base, last_base, addr_typ = set(), 1, 0, None, 4
    for line in lines:
        body = line.rstrip(b"\r")
        if not body.startswith(b":"):
            continue
        rec = bytes.fromhex(body[1:].decode())
        n, addr, typ = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if typ == 2:
            base = int.from_bytes(rec[4:6], "big") << 4
            if cur_image == image: addr_typ = 2
        elif typ == 4:
            base = int.from_bytes(rec[4:6], "big") << 16
            if cur_image == image: addr_typ = 4
        elif typ == 0 and cur_image == image:
            existing.update(range(base + addr, base + addr + n))
            last_base = base
        elif typ == 1:
            cur_image, base = cur_image + 1, 0
    append_lines = []
    for a, blob in appends:
        if image == 1 and any(x in FLASH_CONFIG for x in range(a, a + len(blob))):
            sys.exit("REFUSED: append touches flash configuration 0x400-0x40F")
        if existing & set(range(a, a + len(blob))):
            sys.exit(f"REFUSED: append at {a:#x} overlaps existing data")
        if not (last_base <= a < last_base + 0x10000):
            sys.exit(f"REFUSED: append at {a:#x} outside last segment {last_base:#x}")
        # 10.11.2026: Bootloader schreibt auch ueber 0xC0000 (Flash-Dump: 66 484 Byte bis 0xC0A50 bytegleich). Grenze = 1 MB Flash.
        if image == 1 and a + len(blob) > 0x100000:
            sys.exit(f"REFUSED: append ends at {a + len(blob):#x}, beyond 1 MB program flash")
        # 045: ueber die 64-KB-Grenze hinaus wie das Original selbst: Typ-04-Record (Extended Linear Address), Daten lueckenlos
        seg, o = last_base, 0
        while o < len(blob):
            pos = a + o
            if pos >= seg + 0x10000:
                seg = pos & ~0xFFFF       # gleicher Record-Typ wie im Image: Rev A Typ 02 (Segment << 4), Rev B Typ 04
                v = seg >> 4 if addr_typ == 2 else seg >> 16
                ela = bytearray([2, 0, 0, addr_typ, v >> 8 & 0xFF, v & 0xFF])
                ela.append((-sum(ela)) & 0xFF)
                append_lines.append(b":" + ela.hex().upper().encode() + b"\r")
            n = min(16, len(blob) - o, seg + 0x10000 - pos)
            chunk = blob[o:o + n]
            off = pos - seg
            rec = bytearray([len(chunk), off >> 8, off & 0xFF, 0]) + chunk
            rec.append((-sum(rec)) & 0xFF)
            append_lines.append(b":" + rec.hex().upper().encode() + b"\r")
            o += n
        print(f"append {len(blob)} bytes at {a:#x} ({len(append_lines)} records total)")

    out = []
    cur_image, base, applied = 1, 0, set()
    for line in lines:
        body = line.rstrip(b"\r")
        if not body.startswith(b":"):
            out.append(line)
            continue
        if body == b":00000001FF" and cur_image == image and append_lines:
            out.extend(append_lines)  # new data goes right before this image's EOF record
        rec = bytearray(bytes.fromhex(body[1:].decode()))
        assert sum(rec) & 0xFF == 0, f"bad checksum in input: {body!r}"
        n, addr, typ = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if typ == 2:
            base = int.from_bytes(rec[4:6], "big") << 4
        elif typ == 4:
            base = int.from_bytes(rec[4:6], "big") << 16
        changed = False
        if typ == 0 and cur_image == image:
            for k in range(n):
                a = base + addr + k
                if a in patches:
                    rec[4 + k] = patches[a]
                    applied.add(a)
                    changed = True
        if changed:
            rec[-1] = (-sum(rec[:-1])) & 0xFF
            cr = line[len(body):]  # original line ending remainder ('\r' or '')
            line = b":" + rec.hex().upper().encode() + cr
        out.append(line)
        if typ == 1:
            cur_image, base = cur_image + 1, 0
    missing = set(patches) - applied
    if missing:
        sys.exit(f"ERROR: addresses not found in image {image}: {sorted(hex(m) for m in missing)}")
    open(dst, "wb").write(b"\n".join(out))
    print(f"patched {len(applied)} bytes in image {image} -> {dst}")


if __name__ == "__main__":
    main()
