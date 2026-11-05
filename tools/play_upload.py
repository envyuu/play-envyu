#!/usr/bin/env python3
"""Dateien und Firmware ueber USB-Serial auf das Polyend Play laden (braucht Patch 011 "usbup" auf dem Geraet; FLASH erst ab 011, in 010 stuerzt er ab).

  play_upload.py ping
  play_upload.py ls /Firmware
  play_upload.py put LOKAL [/ZIEL/PFAD]       (Standardziel: /Firmware/<name> fuer .ppf, sonst /<name>)
  play_upload.py crc /PFAD
  play_upload.py get /PFAD [LOKAL]             (Datei herunterladen, ab Patch 017)
  play_upload.py cfg [usb|mt|fader|creative WERT]  (Menue "USB Audio" lesen/setzen, ab Patch 017)
  play_upload.py del /PFAD
  play_upload.py flash PolyendPlay_xyz.ppf    (Update wie im Menue; fragt nach)
  play_upload.py rec [start|stop]            (USB-Eingang aufnehmen, Patch 024)
  play_upload.py update LOKAL.ppf             (pruefen → hochladen → CRC → fragt → flashen)

Optionen: --port /dev/ttyACM0, --yes (nicht nachfragen)
Hinweis: Niemals 134 Baud verwenden – das versetzt den Teensy-Kern in den Programmiermodus.
"""
import argparse, os, sys, time, zlib
import serial

PORT = "/dev/serial/by-id/usb-Polyend_Play_4294967295-if00"
BAUD = 115200          # bei USB-CDC ohne Bedeutung, aber NICHT 134
ORIGINAL = "PolyendPlay_v1.6.0.ppf"


def q(path):
    """Pfad fuer einen USB-Befehl: mit Leerzeichen in "..." (ab Patch 025 vom Play verstanden)."""
    return f'"{path}"' if " " in path else path


class Play:
    def __init__(self, port):
        self.s = serial.Serial(port, BAUD, timeout=0.2, write_timeout=10)
        self.s.reset_input_buffer()
        self.buf = b""

    def line(self, timeout):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            if time.time() > end:
                raise TimeoutError("keine Antwort vom Play")
            self.buf += self.s.read(256)
        l, self.buf = self.buf.split(b"\n", 1)
        return l.decode("utf-8", "replace").strip()

    def cmd(self, text, timeout=3.0):
        self.s.write(text.encode() + b"\n")
        return self.line(timeout)

    def ping(self):
        try:
            r = self.cmd("PING", 1.5)
        except TimeoutError:
            sys.exit("Keine Antwort. Laeuft auf dem Play eine Firmware mit Patch 010 (usbup)?")
        if r != "OK USBUP 1":
            sys.exit(f"Unerwartete Antwort: {r!r}")
        return r

    def ls(self, d):
        self.s.write(f"LS {q(d)}\n".encode())
        out = []
        while True:
            l = self.line(5)
            if l.startswith("OK") or l.startswith("ERR"):
                return out, l
            out.append(l)

    def put(self, data, remote):
        crc = zlib.crc32(data) & 0xFFFFFFFF
        r = self.cmd(f"PUT {q(remote)} {len(data)} {crc:08x}")
        if r != "OK READY":
            sys.exit(f"PUT abgelehnt: {r}")
        t0 = time.time(); step = 16384
        for off in range(0, len(data), step):
            self.s.write(data[off:off + step])
            pct = min(100, (off + step) * 100 // max(1, len(data)))
            print(f"\r  senden {pct:3d} %  ({(off + step) / 1024 / max(0.001, time.time() - t0):6.0f} KB/s)", end="", flush=True)
        self.s.flush()
        print(f"\r  gesendet {len(data)} Byte in {time.time() - t0:.1f} s, Play prueft …        ")
        r = self.line(60 + len(data) / 200_000)
        if r != f"OK {crc:08x}":
            sys.exit(f"Upload fehlgeschlagen: {r}")
        return crc

    def read_exact(self, n, timeout):
        end = time.time() + timeout
        while len(self.buf) < n:
            if time.time() > end:
                raise TimeoutError("Download haengt")
            self.buf += self.s.read(max(256, n - len(self.buf)))
        d, self.buf = self.buf[:n], self.buf[n:]
        return d

    def get(self, remote):
        r = self.cmd(f"GET {q(remote)}", 5)
        if r != "OK GET":
            sys.exit(f"GET abgelehnt: {r}")
        out = bytearray(); t0 = time.time()
        while True:
            n = int.from_bytes(self.read_exact(4, 10), "little")
            if n == 0:
                break
            if n == 0xFFFFFFFF:
                sys.exit(f"Lesefehler auf dem Play: {self.line(5)}")
            if n > 4092:
                sys.exit(f"Protokollfehler: Blocklaenge {n}")
            out += self.read_exact(n, 10)
            print(f"\r  empfangen {len(out) / 1024:8.0f} KB ({len(out) / 1024 / max(0.001, time.time() - t0):6.0f} KB/s)", end="", flush=True)
        print()
        r = self.line(5)
        want = f"OK {len(out)} {zlib.crc32(out) & 0xFFFFFFFF:08x}"
        if r != want:
            sys.exit(f"Pruefsumme/Laenge falsch: Play {r!r}, empfangen {want!r}")
        return bytes(out)

    def crc(self, remote):
        return self.cmd(f"CRC {q(remote)}", 60)


def check_ppf(data, name):
    """Lokale .ppf grob pruefen: Intel-HEX, alle Pruefsummen, mindestens zwei Images."""
    if not (name.startswith("PolyendPlay") and name.endswith(".ppf")):
        sys.exit("Dateiname muss PolyendPlay….ppf sein (sonst zeigt das Update-Menue sie nicht).")
    if name == ORIGINAL:
        sys.exit(f"{ORIGINAL} ist das Original und auf dem Geraet geschuetzt – bitte umbenennen.")
    if len(name) > 60:
        sys.exit("Dateiname zu lang (max. 60 Zeichen).")
    images = 0
    for n, raw in enumerate(data.splitlines(), 1):
        l = raw.strip()
        if not l:
            continue
        if not l.startswith(b":"):
            sys.exit(f"Zeile {n}: kein Intel-HEX")
        b = bytes.fromhex(l[1:].decode())
        if sum(b) & 0xFF:
            sys.exit(f"Zeile {n}: Pruefsumme falsch")
        if b[3] == 1:
            images += 1
    if images < 2:
        sys.exit(f"Nur {images} HEX-Image(s) – eine .ppf enthaelt Rev A und Rev B.")


def ask(q, yes):
    if yes:
        return True
    return input(q + " [j/N] ").strip().lower() in ("j", "ja", "y", "yes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=PORT if os.path.exists(PORT) else "/dev/ttyACM0")
    ap.add_argument("--yes", action="store_true")
    ap.add_argument("--experimentell", action="store_true", help="(ohne Wirkung, frueher fuer FLASH noetig)")
    ap.add_argument("cmd", choices=["ping", "ls", "put", "get", "cfg", "rec", "crc", "del", "flash", "update"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    p = Play(a.port)
    print(p.ping())

    if a.cmd == "ls":
        items, end = p.ls(a.args[0] if a.args else "/")
        for i in items:
            print(" ", i)
        print(end)
    elif a.cmd == "put":
        local = a.args[0]; name = os.path.basename(local)
        remote = a.args[1] if len(a.args) > 1 else ("/Firmware/" if name.endswith(".ppf") else "/") + name
        data = open(local, "rb").read()
        print(f"OK {p.put(data, remote):08x} → {remote}")
    elif a.cmd == "get":
        remote = a.args[0]
        local = a.args[1] if len(a.args) > 1 else os.path.basename(remote)
        if os.path.exists(local) and not ask(f"{local} existiert – ueberschreiben?", a.yes):
            return
        data = p.get(remote)
        open(local, "wb").write(data)
        print(f"OK {len(data)} Byte, CRC {zlib.crc32(data) & 0xFFFFFFFF:08x} → {local}")
    elif a.cmd == "cfg":
        if a.args:
            if len(a.args) != 2: sys.exit("cfg KEY WERT  (usb 0..3, mt 0..1, fader 0..1, creative 0..1)")
            print(p.cmd(f"CFG {a.args[0]} {a.args[1]}"))
        r = p.cmd("CFG").split()
        if len(r) == 5 and r[0] == "OK":
            N = [("Audio Out", ["Off", "Stereo", "Stereo + Multi", "Stereo + In"]), ("Multitrack 9/10", ["Master", "FX Return"]),
                 ("Track Level", ["Pre Fader", "Post Fader"]), ("FX Mode", ["Classic", "Creative"])]
            for (n, vals), v in zip(N, r[1:]):
                print(f"  {n:16s} {vals[int(v)] if v.isdigit() and int(v) < len(vals) else v}")
        else:
            print(" ".join(r))
    elif a.cmd == "rec":
        # rec [start|stop] – Aufnahme des USB-Eingangs (Patch 024, nur "Stereo + In") nach Samples/USB Rec/Rec NNN.wav
        if a.args:
            v = {"start": "1", "stop": "0", "1": "1", "0": "0"}.get(a.args[0])
            if v is None: sys.exit("rec [start|stop]")
            r = p.cmd(f"REC {v}")
            if not r.startswith("OK"): print(r)
        r = p.cmd("REC").split()
        if len(r) == 6 and r[0] == "OK":
            st = {"0": "bereit", "1": "startet", "2": "NIMMT AUF", "3": "schliesst ab"}.get(r[1], r[1])
            print(f"  Zustand {st}, {int(r[2]) / 10:.1f} s, verloren {r[3]} Samples, Datei Rec_{int(r[4]):03d}.wav (Samples/Live Recordings), Fehler {r[5]}")
        else:
            print(" ".join(r))
    elif a.cmd == "crc":
        print(p.crc(a.args[0]))
    elif a.cmd == "del":
        print(p.cmd(f"DEL {q(a.args[0])}"))
    elif a.cmd in ("flash", "update"):
        if a.cmd == "update":
            local = a.args[0]; name = os.path.basename(local)
            data = open(local, "rb").read()
            check_ppf(data, name)
            print(f"Lokal OK: {name}, {len(data)} Byte, CRC {zlib.crc32(data) & 0xFFFFFFFF:08x}")
            p.put(data, "/Firmware/" + name)
            r = p.crc("/Firmware/" + name)
            if r != f"OK {len(data)} {zlib.crc32(data) & 0xFFFFFFFF:08x}":
                sys.exit(f"Nachpruefung fehlgeschlagen: {r}")
            print(f"Auf der Karte geprueft: /Firmware/{name}")
        else:
            name = a.args[0]
        if not ask(f"Jetzt {name} flashen? Das Play startet neu und flasht ~35 s.", a.yes):
            print("Nicht geflasht. Die Datei liegt im Update-Menue bereit.")
            return
        r = p.cmd(f"FLASH {name}")
        print(r)
        if r == "OK FLASHING":
            print("Das Play flasht jetzt. Nicht ausschalten, bis es wieder hochgefahren ist.")


if __name__ == "__main__":
    main()
