#!/usr/bin/env python3
"""Create a release manifest from the stock firmware and a locally built patched .ppf.

usage: make_manifest.py STOCK.ppf BUILT.ppf PAYLOAD.bin OUT.json --version V

The manifest holds only our compiled code and the bytes we overwrite. It holds no stock
firmware beyond those replacement bytes, and fails if image 2 differs in any way.
"""
import argparse
import base64
import hashlib
import json

FIRMWARE_VERSION = "1.6.0"
PAYLOAD_ADDR = 0xB069C


def images(path):
    """Return one {address: byte} dict per Intel-HEX image in the file."""
    result, current, base = [], {}, 0
    for line in open(path, "rb").read().split(b"\n"):
        body = line.rstrip(b"\r")
        if not body.startswith(b":"):
            continue
        rec = bytes.fromhex(body[1:].decode())
        n, addr, typ = rec[0], (rec[1] << 8) | rec[2], rec[3]
        if typ == 0:
            for k in range(n):
                current[base + addr + k] = rec[4 + k]
        elif typ == 2:
            base = int.from_bytes(rec[4:6], "big") << 4
        elif typ == 4:
            base = int.from_bytes(rec[4:6], "big") << 16
        elif typ == 1:
            result.append(current)
            current, base = {}, 0
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stock")
    ap.add_argument("built")
    ap.add_argument("payload")
    ap.add_argument("out")
    ap.add_argument("--version", required=True)
    a = ap.parse_args()

    stock, built = images(a.stock), images(a.built)
    assert len(stock) == len(built) == 2, "expected two images"
    assert stock[1] == built[1], "image 2 (Rev B) must stay untouched"

    payload = open(a.payload, "rb").read()
    new = {x: v for x, v in built[0].items() if x not in stock[0]}
    assert sorted(new) == list(range(PAYLOAD_ADDR, PAYLOAD_ADDR + len(payload))), "unexpected appended range"
    assert bytes(new[x] for x in sorted(new)) == payload, "appended data differs from payload"
    assert set(stock[0]) <= set(built[0]), "stock bytes disappeared"

    changed = sorted(x for x, v in stock[0].items() if built[0][x] != v)
    patches, start = [], None
    for i, x in enumerate(changed):
        if start is None:
            start = x
        if i + 1 == len(changed) or changed[i + 1] != x + 1:
            patches.append({"addr": f"{start:x}", "bytes": bytes(built[0][y] for y in range(start, x + 1)).hex()})
            start = None

    manifest = {
        "format": 1,
        "mod_version": a.version,
        "firmware_version": FIRMWARE_VERSION,
        "target": "Rev A (Kinetis K66), image 1 of the .ppf",
        "stock_sha256": hashlib.sha256(open(a.stock, "rb").read()).hexdigest(),
        "output_sha256": hashlib.sha256(open(a.built, "rb").read()).hexdigest(),
        "payload_addr": f"{PAYLOAD_ADDR:x}",
        "payload_base64": base64.b64encode(payload).decode(),
        "patches": patches,
    }
    with open(a.out, "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    print(f"{a.out}: {len(payload)} payload bytes, {len(patches)} patch sites, {len(changed)} changed bytes")


if __name__ == "__main__":
    main()
