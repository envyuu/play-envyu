#!/usr/bin/env python3
"""Aufnahme vom Play (USB-Audio) auswerten: Pegel, Clipping, Aussetzer (Nullfolgen/Spruenge), Grundfrequenz.

  arecord -D hw:CARD=Play,DEV=0 -f S16_LE -r 44100 -c 2 -d 30 test.wav
  analyse_aufnahme.py test.wav [--ton 440]    (--ton: erwartete Frequenz eines Testtons auf dem Play)
  analyse_aufnahme.py --selbsttest
"""
import sys, wave, struct, math, array

def load(fn):
    w = wave.open(fn, "rb")
    assert w.getsampwidth() == 2, "16 Bit erwartet"
    ch, fs, n = w.getnchannels(), w.getframerate(), w.getnframes()
    a = array.array("h", w.readframes(n))
    if sys.byteorder != "little": a.byteswap()
    return fs, [a[c::ch] for c in range(ch)]

def freq_zero_cross(x, fs):
    zc = [i for i in range(1, len(x)) if x[i - 1] < 0 <= x[i]]
    if len(zc) < 3: return None
    # lineare Interpolation der Nulldurchgaenge
    t = [i - 1 + (-x[i - 1]) / (x[i] - x[i - 1]) for i in zc]
    return (len(t) - 1) * fs / (t[-1] - t[0])

def analyse(fs, chans, ton=None):
    res = {}
    for c, x in enumerate(chans):
        n = len(x)
        peak = max(abs(v) for v in x) if n else 0
        rms = math.sqrt(sum(v * v for v in x) / n) if n else 0
        clip = sum(1 for v in x if v in (32767, -32768))
        # Aussetzer: >= 32 exakte Nullen in Folge mitten in einem Signal
        runs, cur = 0, 0
        for v in x:
            if v == 0: cur += 1
            else:
                if cur >= 32: runs += 1
                cur = 0
        # Spruenge: zweite Differenz deutlich ueber dem typischen Wert (Hinweis auf fehlende/doppelte Bloecke)
        d2 = [abs(x[i + 1] - 2 * x[i] + x[i - 1]) for i in range(1, n - 1)]
        typ = sorted(d2)[len(d2) // 2] if d2 else 0
        jumps = sum(1 for v in d2 if v > max(2000, 20 * typ))
        r = dict(peak=peak, rms=round(rms, 1), clip=clip, null_runs=runs, jumps=jumps)
        if ton:
            f = freq_zero_cross(x, fs)
            r["freq"] = round(f, 3) if f else None
            if f: r["abw_cent"] = round(1200 * math.log2(f / ton), 2)
        res[f"Kanal {c}"] = r
    return res

def selbsttest():
    fs, n, f0 = 44100, 44100 * 2, 440.0
    true_fs = 44117.647                      # Play liefert etwas schneller als 44100
    x = [int(12000 * math.sin(2 * math.pi * f0 * i / true_fs)) for i in range(n)]
    ok = analyse(fs, [x], ton=f0)["Kanal 0"]
    y = x[:]; y[50000:50000 + 128] = [0] * 128      # Aussetzer
    y = y[:70000] + y[70045:]                        # fehlendes Paket (45 Frames)
    bad = analyse(fs, [y], ton=f0)["Kanal 0"]
    print("sauber:", ok); print("gestoert:", bad)
    exp_cent = 1200 * math.log2(fs / true_fs)   # Ton erscheint tiefer
    good = ok["null_runs"] == 0 and ok["jumps"] == 0 and abs(ok["abw_cent"] - exp_cent) < 0.2 and bad["null_runs"] >= 1 and bad["jumps"] >= 1
    print(f"erwartete Tonhoehenabweichung durch 44117,6 statt 44100 Hz (Wiedergabe mit 44100): {exp_cent:+.2f} Cent")
    print("SELBSTTEST:", "OK" if good else "FEHLER"); return 0 if good else 1

if __name__ == "__main__":
    if sys.argv[1:] == ["--selbsttest"]: sys.exit(selbsttest())
    ton = float(sys.argv[sys.argv.index("--ton") + 1]) if "--ton" in sys.argv else None
    fs, ch = load(sys.argv[1])
    print(f"{sys.argv[1]}: {fs} Hz, {len(ch)} Kanaele, {len(ch[0]) / fs:.1f} s")
    for k, v in analyse(fs, ch, ton).items(): print(" ", k, v)
