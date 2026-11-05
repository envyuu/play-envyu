#!/usr/bin/env python3
"""Zustand des Multitrack-Patches (015) am Geraet lesen – ueber PEEK (usbup, Patch 011).
NUR FUER DEN SPAETEREN GERAETETEST. Liest ausschliesslich RAM, schreibt nichts
(Umschalten der Presets: Befehl "MT <preset> <flags>" ueber ttyACM, siehe README).

  mt_status.py [--port /dev/ttyACM0] [--watch SEK]
"""
import argparse, time

FIELDS = ["magic", "head", "tail", "alt", "state", "nch", "epoch", "blocks", "drops", "packets", "underruns", "frames",
          "favg", "rate", "acc", "last_n", "fill_min", "fill_max", "starts", "setups", "wr_ok", "wr_epoch", "wr_pos",
          "wr_nch", "wmask", "trk_seen", "fx_seen", "hook_calls"]
STATES = {0: "AUS", 1: "VORFUELLEN", 2: "LAEUFT"}
BASE, CFG = 0x1FFFD400, 0x1FFFD480
MOD = 384 << 20
SRC = {**{i: f"Spur{i + 1}" for i in range(8)}, 8: "MasterL", 9: "MasterR", 10: "FX-L", 11: "FX-R", 12: "FX-mono",
       13: "Master-mono", 14: "DelayL", 15: "DelayR", 16: "ReverbL", 17: "ReverbR", 0xFF: "Stille"}

def parse_peek(line):
    p = line.split()
    if len(p) != 5 or p[0] != "OK": raise ValueError(f"unerwartete PEEK-Antwort: {line!r}")
    return [int(x, 16) for x in p[1:]]

def decode(words, cfg):
    v = dict(zip(FIELDS, words))
    s32 = lambda x: x - (1 << 32) if x & 0x80000000 else x
    cb = b"".join(w.to_bytes(4, "little") for w in cfg)
    preset = {0: "A", 1: "B", 255: "eigene Tabelle"}.get(cb[4], cb[4])
    out = [f"magic {'OK' if v['magic'] == 0x4D54A015 else 'FALSCH (%08x) – Patch 015 nicht aktiv?' % v['magic']}, "
           f"Konfiguration {'OK' if int.from_bytes(cb[0:4], 'little') == 0x4D544346 else 'FALSCH'}: Preset {preset}, "
           f"Spur-Lautstaerke {'an' if cb[5] & 1 else 'aus'}",
           f"Zustand {STATES.get(v['state'], v['state'])}, alt {v['alt']} ({v['nch']} Kanaele), Starts {v['starts']}, Setups {v['setups']}",
           f"Fuellstand {(v['head'] - v['tail']) % MOD} Frames (gefiltert {s32(v['favg']) / 256:.1f}, min "
           f"{v['fill_min'] if v['fill_min'] != 0xFFFFFFFF else '-'}, max {v['fill_max']}), Rate {s32(v['rate']) / 65536 * 1000:.1f} Hz",
           f"Bloecke {v['blocks']}, Pakete {v['packets']}, Frames {v['frames']}, Unterlaeufe {v['underruns']}, verworfen {v['drops']}",
           f"letzter Block: Spuren {v['trk_seen']:08b} (Bit i = Spur i+1 lieferte), FX {v['fx_seen']:02b}, "
           f"Hook-Aufrufe ohne Block {v['hook_calls']}",
           "Routing 10: " + " ".join(SRC.get(x, hex(x)) for x in cb[8:18]),
           "Routing 11: " + " ".join(SRC.get(x, hex(x)) for x in cb[18:29])]
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
        cfg = []
        for k in range(0, 8, 4): cfg += peek(CFG + 4 * k)
        print(time.strftime("%H:%M:%S"), "\n" + decode(words[:len(FIELDS)], cfg), flush=True)
        if a.watch <= 0: break
        time.sleep(a.watch)

if __name__ == "__main__":
    main()
