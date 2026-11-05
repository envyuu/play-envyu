#!/usr/bin/env python3
"""Zustand des USB-Audio-Patches (012) am Geraet lesen – ueber den PEEK-Befehl von usbup (Patch 011).
NUR FUER DEN SPAETEREN GERAETETEST. Liest ausschliesslich RAM (PEEK), schreibt nichts.

  ua_status.py [--port /dev/ttyACM0] [--watch SEK]
"""
import argparse, sys, time

FIELDS = ["magic", "head", "tail", "alt", "state", "blocks", "drops", "packets", "underruns", "frames",
          "favg", "rate", "acc", "last_n", "fill_min", "fill_max", "starts", "setups"]
STATES = {0: "AUS", 1: "VORFUELLEN", 2: "LAEUFT"}
BASE = 0x1FFFD000

def parse_peek(line):
    p = line.split()
    if len(p) != 5 or p[0] != "OK": raise ValueError(f"unerwartete PEEK-Antwort: {line!r}")
    return [int(x, 16) for x in p[1:]]

def decode(words):
    v = dict(zip(FIELDS, words))
    s32 = lambda x: x - (1 << 32) if x & 0x80000000 else x
    out = [f"magic {'OK' if v['magic'] == 0x55A0D10A else 'FALSCH (%08x) – Patch 012 nicht aktiv?' % v['magic']}",
           f"Zustand {STATES.get(v['state'], v['state'])}, alt {v['alt']}, Starts {v['starts']}, Setup-Antworten {v['setups']}",
           f"Fuellstand {(v['head'] - v['tail']) & 0xFFFFFFFF} Frames (gefiltert {s32(v['favg']) / 256:.1f}, min {v['fill_min'] if v['fill_min'] != 0xFFFFFFFF else '-'}, max {v['fill_max']})",
           f"Rate {s32(v['rate']) / 65536:.4f} Frames/ms (~{s32(v['rate']) / 65536 * 1000:.1f} Hz), letztes Paket {v['last_n']} Frames",
           f"Bloecke {v['blocks']}, Pakete {v['packets']}, Frames {v['frames']}, Unterlaeufe {v['underruns']}, verworfen {v['drops']}"]
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--watch", type=float, default=0)
    a = ap.parse_args()
    import serial
    s = serial.Serial(a.port, 115200, timeout=1.5)     # NIE 134 Baud
    s.reset_input_buffer()
    def peek(addr):
        s.write(f"PEEK {addr:08X}\n".encode())
        return parse_peek(s.readline().decode(errors="replace").strip())
    while True:
        words = []
        for k in range(0, len(FIELDS), 4): words += peek(BASE + 4 * k)
        print(time.strftime("%H:%M:%S"), "\n" + decode(words[:len(FIELDS)]), flush=True)
        if a.watch <= 0: break
        time.sleep(a.watch)

if __name__ == "__main__":
    main()
