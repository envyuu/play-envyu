#!/usr/bin/env python3
"""Apply the mod to your own copy of the Polyend Play firmware. Python 3 standard library only.

usage: play_patch.py STOCK.ppf [-o OUT.ppf] [-m MANIFEST.json]

Nothing is written unless the input is the exact stock file this manifest was made for and
the result matches the hash of the tested build.
"""
import argparse
import base64
import hashlib
import json
import os
import sys

FLASH_CONFIG = range(0x400, 0x410)
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MANIFEST = os.path.join(HERE, "..", "manifests", "latest.json")


def fail(msg):
    sys.exit(f"ERROR: {msg}")


def record(addr, data):
    rec = bytearray([len(data), addr >> 8, addr & 0xFF, 0]) + data
    rec.append(-sum(rec) & 0xFF)
    return b":" + rec.hex().upper().encode()


def apply(raw, patches, payload_addr, payload):
    """Patch image 1 in place and append the payload. All other bytes stay identical."""
    lines, out, image, base, last_base, applied, addr_typ = raw.split(b"\n"), [], 1, 0, None, set(), 4

    for line in lines:
        body = line.rstrip(b"\r")
        if not body.startswith(b":"):
            continue
        rec = bytes.fromhex(body[1:].decode())
        if rec[3] == 2:
            base = int.from_bytes(rec[4:6], "big") << 4
            addr_typ = 2
        elif rec[3] == 4:
            base = int.from_bytes(rec[4:6], "big") << 16
            addr_typ = 4
        elif rec[3] == 0 and image == 1:
            last_base = base
        elif rec[3] == 1:
            break

    if not last_base <= payload_addr < last_base + 0x10000:
        fail("payload does not start in the last firmware segment")
    if payload_addr + len(payload) > 0x100000:
        fail("payload ends beyond the 1 MB program flash")
    # Data stays gapless. When it crosses a 64 KB boundary, a segment record of the same type as the stock
    # image (Rev A: type 02) starts the next segment, as in the stock file itself.
    appended, seg, o = [], last_base, 0
    while o < len(payload):
        pos = payload_addr + o
        if pos >= seg + 0x10000:
            seg = pos & ~0xFFFF
            v = seg >> 4 if addr_typ == 2 else seg >> 16
            ela = bytearray([2, 0, 0, addr_typ, v >> 8 & 0xFF, v & 0xFF])
            ela.append(-sum(ela) & 0xFF)
            appended.append(b":" + ela.hex().upper().encode() + b"\r")
        n = min(16, len(payload) - o, seg + 0x10000 - pos)
        appended.append(record(pos - seg, payload[o:o + n]) + b"\r")
        o += n

    image, base = 1, 0
    for line in lines:
        body = line.rstrip(b"\r")
        if not body.startswith(b":"):
            out.append(line)
            continue
        if body == b":00000001FF" and image == 1:
            out.extend(appended)
        rec = bytearray(bytes.fromhex(body[1:].decode()))
        if sum(rec) & 0xFF:
            fail("bad checksum in input file")
        n, addr, typ = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if typ == 2:
            base = int.from_bytes(rec[4:6], "big") << 4
        elif typ == 4:
            base = int.from_bytes(rec[4:6], "big") << 16
        changed = False
        if typ == 0 and image == 1:
            for k in range(n):
                a = base + addr + k
                if a in patches:
                    rec[4 + k] = patches[a]
                    applied.add(a)
                    changed = True
        if changed:
            rec[-1] = -sum(rec[:-1]) & 0xFF
            line = b":" + rec.hex().upper().encode() + line[len(body):]
        out.append(line)
        if typ == 1:
            image, base = image + 1, 0

    if set(patches) - applied:
        fail("some patch addresses were not found in the firmware")
    return b"\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="Apply the mod to your own stock Polyend Play firmware.")
    ap.add_argument("stock", help="official firmware .ppf, downloaded from Polyend")
    ap.add_argument("-o", "--out", default="PolyendPlay_mod.ppf", help="output file")
    ap.add_argument("-m", "--manifest", default=DEFAULT_MANIFEST)
    a = ap.parse_args()

    manifest = json.load(open(a.manifest))
    if manifest.get("format") != 1:
        fail("unsupported manifest format")

    raw = open(a.stock, "rb").read()
    if hashlib.sha256(raw).hexdigest() != manifest["stock_sha256"]:
        fail(f"this is not the stock firmware {manifest['firmware_version']} this mod was built for; nothing written")

    patches = {}
    for p in manifest["patches"]:
        addr = int(p["addr"], 16)
        for k, b in enumerate(bytes.fromhex(p["bytes"])):
            patches[addr + k] = b
    payload_addr = int(manifest["payload_addr"], 16)
    payload = base64.b64decode(manifest["payload_base64"])
    if any(x in FLASH_CONFIG for x in patches) or payload_addr in FLASH_CONFIG:
        fail("manifest touches the flash configuration field; refusing")

    result = apply(raw, patches, payload_addr, payload)
    if hashlib.sha256(result).hexdigest() != manifest["output_sha256"]:
        fail("result does not match the tested build; nothing written")
    if os.path.exists(a.out):
        fail(f"{a.out} already exists; choose another name with -o")

    with open(a.out, "wb") as f:
        f.write(result)
    print(f"OK: {a.out} (mod {manifest['mod_version']}, firmware {manifest['firmware_version']})")
    print(f"Target: {manifest['target']}. Read the README before flashing; you do this at your own risk.")


if __name__ == "__main__":
    main()
