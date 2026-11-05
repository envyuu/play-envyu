#!/usr/bin/env python3
"""Mehrkanal-Aufnahme vom Play (Patch 015) auswerten: je Kanal Pegel, Clipping, Aussetzer (Nullfolgen), aktive Kanaele;
optional Vergleich der Master-Kanaele mit einer gleichzeitigen Stereo-Aufnahme (IF4).

  arecord -D hw:CARD=Play,DEV=1 -f S16_LE -r 44100 -c 10 -d 20 mt.wav
  analyse_mt.py mt.wav [--preset A|B] [--stereo st.wav]
  analyse_mt.py --selbsttest
"""
import sys, wave, array, math

NAMES = {"A": ["Spur1", "Spur2", "Spur3", "Spur4", "Spur5", "Spur6", "Spur7", "Spur8", "MasterL", "MasterR", "FX-mono"],
         "B": ["Spur1", "Spur2", "Spur3", "Spur4", "Spur5", "Spur6", "Spur7", "Spur8", "FX-L", "FX-R", "Master-mono"]}

def load(fn):
    w = wave.open(fn, "rb")
    assert w.getsampwidth() == 2, "16 Bit erwartet"
    ch, n = w.getnchannels(), w.getnframes()
    a = array.array("h", w.readframes(n))
    if sys.byteorder != "little": a.byteswap()
    return w.getframerate(), [a[c::ch] for c in range(ch)]

def stats(x):
    n = len(x)
    peak = max((abs(v) for v in x), default=0)
    rms = math.sqrt(sum(v * v for v in x) / n) if n else 0.0
    clip = sum(1 for v in x if v in (32767, -32768))
    runs, cur = 0, 0
    for v in x:
        if v == 0: cur += 1
        else:
            if cur >= 32: runs += 1
            cur = 0
    return peak, rms, clip, runs

def best_lag(a, b, maxlag=4096, n=20000):
    """Versatz, bei dem b[i+lag] == a[i] am haeufigsten gilt (exakter Vergleich, Master ist bitgleich)."""
    a = a[maxlag:maxlag + n]
    best = (0, None)
    for lag in range(-maxlag, maxlag + 1):
        seg = b[maxlag + lag:maxlag + lag + n]
        if len(seg) < n: continue
        hit = sum(1 for i in range(0, n, 7) if seg[i] == a[i])
        if hit > best[0]: best = (hit, lag)
    return best[1], best[0] / len(range(0, n, 7))

def analyse(fs, chans, preset="A", stereo=None):
    names = NAMES.get(preset, NAMES["A"])
    res = []
    print(f"{len(chans)} Kanaele, {fs} Hz, {len(chans[0]) / fs:.1f} s, Preset {preset}")
    for c, x in enumerate(chans):
        peak, rms, clip, runs = stats(x)
        db = 20 * math.log10(rms / 32768) if rms > 0 else -150
        nm = names[c] if c < len(names) else f"K{c + 1}"
        print(f"  K{c + 1:2d} {nm:12s} Spitze {peak:6d}  RMS {db:7.1f} dBFS  Clip {clip:5d}  Nullfolgen>=32 {runs:4d}  {'aktiv' if peak > 64 else 'still'}")
        res.append((peak, rms, clip, runs))
    if stereo is not None and len(chans) >= 10 and preset == "A":
        lag, q = best_lag(stereo[0], chans[8])
        print(f"  Master K9 gegen Stereo-L: Versatz {lag} Frames (Mehrkanal[i+Versatz] = Stereo[i]), bitgleich in {100 * q:.1f} % der Stichproben")
        res.append(("lag", lag, q))
    return res

def selftest():
    import random
    random.seed(1)
    n = 44100
    chans = []
    for c in range(10):
        if c == 3: chans.append(array.array("h", [0] * n)); continue
        x = array.array("h", [int(8000 * math.sin(2 * math.pi * (100 + 50 * c) * i / 44100)) + random.randint(-500, 500) for i in range(n)])
        if c == 5:
            for i in range(20000, 20100): x[i] = 0                  # Aussetzer
        chans.append(x)
    st = [array.array("h", [0] * 300) + chans[8][:-300], array.array("h", [0] * 300) + chans[9][:-300]]
    r = analyse(44100, chans, "A", stereo=st)
    ok = r[3][0] == 0 and r[5][3] >= 1 and r[0][3] == 0 and r[-1][0] == "lag" and r[-1][1] == -300 and r[-1][2] > 0.99
    print("SELBSTTEST:", "OK" if ok else "FEHLER")
    return 0 if ok else 1

if __name__ == "__main__":
    if sys.argv[1:] == ["--selbsttest"]: sys.exit(selftest())
    a = sys.argv[1:]
    preset = a[a.index("--preset") + 1] if "--preset" in a else "A"
    st = load(a[a.index("--stereo") + 1])[1] if "--stereo" in a else None
    fs, ch = load(a[0])
    analyse(fs, ch, preset, st)
