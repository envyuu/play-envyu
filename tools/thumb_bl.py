#!/usr/bin/env python3
"""Encode a Thumb-2 BL instruction: thumb_bl.py FROM_ADDR TO_ADDR -> 4 bytes hex (little endian halfwords)."""
import sys
def bl(frm, to):
    off = to - (frm + 4)
    assert off % 2 == 0 and -(1 << 24) <= off < (1 << 24)
    off &= (1 << 25) - 1
    s = (off >> 24) & 1; i1 = (off >> 23) & 1; i2 = (off >> 22) & 1
    imm10 = (off >> 12) & 0x3FF; imm11 = (off >> 1) & 0x7FF
    j1 = (~(i1 ^ s)) & 1; j2 = (~(i2 ^ s)) & 1
    h1 = 0xF000 | (s << 10) | imm10
    h2 = 0xD000 | (j1 << 13) | (j2 << 11) | imm11
    return h1.to_bytes(2, "little") + h2.to_bytes(2, "little")
if __name__ == "__main__":
    print(bl(int(sys.argv[1], 16), int(sys.argv[2], 16)).hex())
