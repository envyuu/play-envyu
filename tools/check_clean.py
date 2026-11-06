#!/usr/bin/env python3
"""Pre-release check: report files that would be published and contain stock firmware data.

usage: check_clean.py STOCK_FIRMWARE.bin

Scans files that git would track (tracked plus untracked, not ignored). Base64 payloads in
manifests are decoded first. Exit code 1 on forbidden files or on overlaps over MAX_RUN bytes.
Filler runs (one or two repeating values) are not counted as overlap.
"""
import base64
import json
import os
import subprocess
import sys

WINDOW = 16
MAX_RUN = 256
FORBIDDEN_EXT = (".ppf", ".ppf_", ".hex", ".elf", ".o", ".wav", ".rtf", ".zip", ".gpr")
FORBIDDEN_NAME = ("decompiled.c", "firmware.bin", "names_k66.tsv")


def overlaps(data, firmware, windows):
    i, found = 0, []
    while i <= len(data) - WINDOW:
        if data[i:i + WINDOW] not in windows or len(set(data[i:i + WINDOW])) <= 2:
            i += 1
            continue
        end = i + WINDOW
        while end < len(data) and firmware.find(data[i:end + 1]) >= 0:
            end += 1
        found.append((i, end - i, data[i:end]))
        i = end
    return found


def main():
    firmware = open(sys.argv[1], "rb").read()
    windows = {firmware[i:i + WINDOW] for i in range(len(firmware) - WINDOW)}
    files = subprocess.run(["git", "ls-files", "-co", "--exclude-standard"], capture_output=True, text=True, check=True).stdout.split("\n")
    bad = 0
    for path in filter(None, files):
        name = os.path.basename(path)
        if name.lower().endswith(FORBIDDEN_EXT) or name in FORBIDDEN_NAME:
            print(f"FORBIDDEN {path}")
            bad += 1
            continue
        data = open(path, "rb").read()
        if path in ("manifests/latest.json", "docs/manifest.json"):
            m = json.loads(data)
            data = base64.b64decode(m["payload_base64"])
        found = overlaps(data, firmware, windows)
        total = sum(f[1] for f in found)
        if found:
            print(f"{path}: {len(found)} overlaps, {total} bytes")
            for off, n, chunk in sorted(found, key=lambda f: -f[1])[:6]:
                print(f"    +{off:#x} {n} bytes {chunk[:32]!r}")
        if any(f[1] > MAX_RUN for f in found):
            print(f"TOO LONG {path}")
            bad += 1
    print(f"{len(list(filter(None, files)))} files checked, {bad} problems")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
