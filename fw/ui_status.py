#!/usr/bin/env python3
"""Zustand des USB-Eingangs (Patch 020) am Geraet lesen – ueber den PEEK-Befehl von usbup (Patch 011).
Liest ausschliesslich RAM (PEEK), schreibt nichts.

  ui_status.py [--port /dev/ttyACM0] [--watch SEK]
"""
import argparse, time

FIELDS = ["magic", "mode", "alt", "state", "head", "tail", "rx_pkts", "rx_frames", "rx_odd", "overruns", "underruns",
          "blocks", "favg", "fb", "fb_pkts", "starts", "setups", "send_d", "send_r", "in_valid", "orig_cb", "orig_calls", "peak", "r0"]
STATES = {0: "AUS", 1: "VORFUELLEN", 2: "LAEUFT"}
BASE = 0x1FFF9000

def parse_peek(line):
    p = line.split()
    if len(p) != 5 or p[0] != "OK": raise ValueError(f"unerwartete PEEK-Antwort: {line!r}")
    return [int(x, 16) for x in p[1:]]

def decode(words):
    v = dict(zip(FIELDS, words))
    s32 = lambda x: x - (1 << 32) if x & 0x80000000 else x
    pk = s32(v["peak"])
    return "\n".join([
        f"magic {'OK' if v['magic'] == 0x55A0D120 else 'FALSCH (%08x) – Patch 020 nicht aktiv?' % v['magic']}, "
        f"Modus {'Stereo + In' if v['mode'] else 'aus'}, Zustand {STATES.get(v['state'], v['state'])}, alt {v['alt']}, Starts {v['starts']}",
        f"Fuellstand {(v['head'] - v['tail']) & 0xFFFFFFFF} Frames (gefiltert {s32(v['favg']) / 256:.1f}), "
        f"Rueckmeldung {v['fb'] / 16384:.4f} Frames/ms (~{v['fb'] / 16.384:.1f} Hz), {v['fb_pkts']}x abgeholt",
        f"Pakete {v['rx_pkts']}, Frames {v['rx_frames']}, Bloecke {v['blocks']}, Unterlaeufe {v['underruns']}, "
        f"Ueberlaeufe {v['overruns']}, ungerade {v['rx_odd']}",
        f"Spitzenpegel {pk} ({'%.1f dBFS' % (20 * __import__('math').log10(pk / 32768)) if pk > 0 else '-'}), "
        f"Send Delay {v['send_d'] / 65536:.2f} Reverb {v['send_r'] / 65536:.2f}, Original-Zusatzeingang {v['orig_cb']:08x} ({v['orig_calls']}x)"])

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
