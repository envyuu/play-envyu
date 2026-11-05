#!/usr/bin/env python3
"""Maschinencode-Test von Patch 015 (Multitrack): fuehrt den ECHTEN Code des gepatchten Rev-A-Images aus –
usbinit_hook → usb_init, usbotg_isr 0x12B9C mit ep_hook/setup_hook, audio_tap → audio_callback 0xBA18 →
(App-Attrappe) → ECHTES Mixer-Update 0x15884 mit den Umleitungen 0x158D6/0x15CBC → mt_track/mt_fx → Ring →
mt_ep_token → BDT EP7 – in einem Cortex-M4-Emulator (unicorn). USB-FS-Hardware wie in emu_isr.py.

Geprueft:
  1. Enumeration: 357-B-Deskriptor, SET/GET_INTERFACE IF5, ENDPT7.
  2. Der Mixer rechnet mit den Hooks BITGLEICH wie ohne (Vergleich mit dem 012-Image: Master-Summen, L/R).
  3. Jeder empfangene Frame jedes Kanals = erwartete Quelle (Spur x Lautstaerke-Rampe, Delay/Reverb-Muster, Master).
  4. Preset-Wechsel waehrend des Streamings, 11-Kanal-Alt, Stereo (IF4) parallel = derselbe Master.
  5. Mixer ausserhalb von audio_tap (WAV-Export) → Hooks schreiben nichts.

usage: emu_mt.py PolyendPlay_v1.6.0_multitrack_ENTWURF.ppf ../012_usb_audio/PolyendPlay_v1.6.0_usbaudio_ENTWURF.ppf
"""
import sys, struct, os, subprocess, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_isr import Dev, setup  # noqa: E402
from check_ppf import images  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
MIX = 0x20020000
POOL = 0x20010000
CNT = 0x20021F00
APP = 0x20021F10
FXO = (0x20021E00, 0x20021E40)
FXVT = 0x20021E80
INQ = MIX + 0x300
INBLK = MIX + 0x400
L_BUF, R_BUF = 0x20000134, 0x20000334
MT = 0x1FFFD400
CFG = 0x1FFFD480
S_ML, S_MR, S_FXL, S_FXR, S_FXM, S_MM = 8, 9, 10, 11, 12, 13
PA10 = [0, 1, 2, 3, 4, 5, 6, 7, S_ML, S_MR]
PB10 = [0, 1, 2, 3, 4, 5, 6, 7, S_FXL, S_FXR]
PA11 = PA10 + [S_FXM]
PB11 = PB10 + [S_MM]

fails = 0; checks = 0
def check(c, m):
    global fails, checks
    checks += 1
    if not c: fails += 1; print("  FEHLER", m)
    else: print("  ok   ", m)


def build_stub():
    d = tempfile.mkdtemp(prefix="emu_stub_")
    elf = os.path.join(d, "stub.elf"); binf = os.path.join(d, "stub.bin")
    subprocess.check_call(["arm-none-eabi-gcc", "-mcpu=cortex-m4", "-mthumb", "-Os", "-ffreestanding", "-nostdlib",
                           "-fno-builtin", "-Wl,-Ttext=0xF0000", "-Wl,-e,app_audio", os.path.join(HERE, "emu_stub.c"), "-o", elf])
    subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", elf, binf])
    nm = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines() if len(l.split()) == 3}
    return open(binf, "rb").read(), nm


def load(ppf):
    im = images(ppf)[0]
    img = bytearray(max(im) + 1)
    for a, v in im.items(): img[a] = v
    return img


def s16(v): v &= 0xFFFF; return v - 0x10000 if v & 0x8000 else v
def sat16(v): return 32767 if v > 32767 else -32768 if v < -32768 else v
def track_sig(i, f):
    if i == 0: return f & 0x7FFF
    if i == 1: return (f >> 15) & 0x7FFF
    return s16((f * (2 * i + 1) * 2654435761 >> 7) + i * 7919)
def fx_sig(fid, f):
    return (((f * 13 + fid * 977) & 0x1FFF) - 0x1000, ((f * 29 + fid * 3331 + 7) & 0x1FFF) - 0x1000)
def tdiv(a, b): q = abs(a) // b; return q if a >= 0 else -q


class Rig:
    """Geraet + App-/Mixer-Attrappe um den echten Mixer-Code."""
    def __init__(self, img, stub, snm, mt=True):
        self.dev = Dev(img)
        uc = self.dev.uc
        uc.mem_write(0xF0000, stub)
        self.mt = mt
        self.snm = snm
        dev = self.dev
        dev.call(0x17128, POOL, 48)                         # echter Block-Pool (48 Einheiten a 0x108)
        uc.mem_write(MIX, bytes(0x600))
        uc.mem_write(MIX + 0x09, bytes([8]))                # num_inputs (receiveReadOnly)
        dev.w32(MIX + 0x0C, 0)                              # keine Ziele (transmit tut nichts)
        dev.w32(MIX + 0x10, INQ)                            # inputQueue
        dev.w32(MIX + 0x18, 8)
        for k, o in enumerate(FXO):                         # Send 0 = Delay, Send 1 = Reverb
            dev.w32(o, FXVT); dev.w32(o + 4, k)
            dev.w32(MIX + 0x11C + 4 * k, o)
        dev.w32(FXVT, snm["fx_process"] | 1)
        dev.w32(MIX + 0x14C, 0xFFFFFFFF)                    # kein Sidechain-Kanal
        dev.w32(APP, APP + 0x10); dev.w32(APP + 0x10 + 8, snm["app_audio"] | 1)
        dev.w32(0x1FFF373C, APP)                            # App-Zeiger fuer audio_callback
        dev.w32(CNT, 0x00012000)
        self.gc = [[0x10000, 0x10000] for _ in range(8)]
        for i in range(8): self.set_gain(i, 0x10000, 0x10000, 0x6000, 0x3000, first=True)
        self.prev = [g[:] for g in self.gc]

    def set_gain(self, i, gl, gr, s0=None, s1=None, first=False):
        d = self.dev
        d.w32(MIX + 0x1C + 4 * i, gl & 0xFFFFFFFF); d.w32(MIX + 0x3C + 4 * i, gr & 0xFFFFFFFF)
        if s0 is not None: d.w32(MIX + 0x5C + 4 * i, s0); d.w32(MIX + 0x7C + 4 * i, s1)
        if first:
            d.w32(MIX + 0x9C + 4 * i, gl); d.w32(MIX + 0xBC + 4 * i, gr)
            if s0 is not None: d.w32(MIX + 0xDC + 4 * i, s0); d.w32(MIX + 0xFC + 4 * i, s1)
        self.gc[i] = [gl, gr]

    def vary_gains(self, b):
        """Spuren 3..7 aendern Lautstaerke/Pan je Block (Rampen), 0/1 bleiben 1.0 (Framezaehler)."""
        self.prev = [g[:] for g in self.gc]
        for i in range(2, 8):
            vol = 0x10000 - ((b * 1237 + i * 4111) % 0x9000)
            pan = ((b * 7 + i * 3) % 21 - 10) / 10.0
            gl = int(vol * (1.0 if pan <= 0 else 1.0 - pan)); gr = int(vol * (1.0 if pan >= 0 else 1.0 + pan))
            self.set_gain(i, gl, gr)

    def block(self, b, present=0xFF, via_tap=True):
        d = self.dev
        f0 = d.r32(CNT)
        for i in range(8):
            a = INBLK + i * 0x108
            d.uc.mem_write(a, struct.pack("<BBHi", 2, 0, 0xFFFF, 1))     # refcount 2 → release zaehlt nur herunter
            d.uc.mem_write(a + 8, struct.pack("<128h", *[track_sig(i, f0 + k) for k in range(128)]))
            d.w32(INQ + 4 * i, a if (present >> i) & 1 else 0)
        if via_tap: d.call(self.nm["audio_tap"], L_BUF, R_BUF, limit=5_000_000)
        else: d.call(0x15884, MIX, limit=5_000_000)
        L = struct.unpack("<128h", d.uc.mem_read(L_BUF, 256)); R = struct.unpack("<128h", d.uc.mem_read(R_BUF, 256))
        acc = bytes(d.uc.mem_read(0x20001694, 0x400))
        return f0, L, R, acc


def expected_track(i, f0, k, gc, gp, post):
    x = track_sig(i, f0 + k)
    if not post: return x
    c = max(gc); p = max(gp)
    c = min(max(c, 0), 0x10000); p = min(max(p, 0), 0x10000)
    if c == 0x10000 and p == 0x10000: return x
    g = p + tdiv(c - p, 128) * k
    return sat16((x * g) >> 16)


def expected_src(s, i_f0, k, blk):
    f0, L, R, gc, gp, post, present = blk
    if s < 8: return expected_track(s, f0, k, gc[s], gp[s], post) if (present >> s) & 1 else 0
    if s == S_ML: return L[k]
    if s == S_MR: return R[k]
    if s == S_MM: return (L[k] + R[k]) >> 1
    dl, dr = fx_sig(0, f0 + k); rl, rr = fx_sig(1, f0 + k)
    if s == S_FXL: return dl + rl
    if s == S_FXR: return dr + rr
    if s == S_FXM: return ((dl + dr) >> 1) + ((rl + rr) >> 1)
    return 0


def stream(rig, alt, route_of_block, nblocks, presets=None, stereo=False, label="", blocks=None):
    """Streamt nblocks Audioblocks, holt je ms ein Paket von EP7 (und optional EP6). Prueft jeden Frame."""
    dev = rig.dev
    nch = 10 if alt == 1 else 11
    blocks = {} if blocks is None else blocks    # f0 → Blockdaten (ueber Laeufe hinweg: Ringlatenz)
    got, sizes, stereo_frames = [], [], []
    t_audio, b = 0.0, 0
    ms = 0
    while b < nblocks:
        while t_audio <= ms and b < nblocks:
            if presets and b in presets: dev.uc.mem_write(CFG + 4, bytes([presets[b]]))
            rig.vary_gains(b)
            f0, L, R, _ = rig.block(b)
            post = dev.uc.mem_read(CFG + 5, 1)[0] & 1
            blocks[f0] = (f0, L, R, [g[:] for g in rig.gc], [g[:] for g in rig.prev], post, 0xFF)
            b += 1; t_audio += 128 / 44117.647 * 1000
        p = dev.token(7, 1, 0x9)
        if p is not None:
            sizes.append(len(p))
            for o in range(0, len(p), 2 * nch): got.append(struct.unpack(f"<{nch}h", p[o:o + 2 * nch]))
        if stereo:
            q = dev.token(6, 1, 0x9)
            if q: stereo_frames += [struct.unpack("<hh", q[o:o + 4]) for o in range(0, len(q), 4)]
        ms += 1
    # Frames dekodieren und pruefen
    bad, gaps, last = 0, 0, None
    starts = sorted(blocks)
    per_route = {}
    for fr in got:
        f = (fr[0] & 0x7FFF) | ((fr[1] & 0x7FFF) << 15)
        if last is not None and f != last + 1: gaps += 1
        last = f
        f0 = max(s for s in starts if s <= f) if starts and f >= starts[0] else None
        if f0 is None or f - f0 >= 128: bad += 1; continue
        route = route_of_block(f0)
        per_route[tuple(route)] = per_route.get(tuple(route), 0) + 1
        exp = [expected_src(s, f0, f - f0, blocks[f0]) for s in route]
        if list(fr) != exp:
            if bad < 3: print(f"    Frame {f}: erhalten {list(fr)}\n               erwartet {exp}")
            bad += 1
    st = [dev.r32(MT + 4 * k) for k in range(20)]
    print(f"  {label}: {len(got)} Frames, Paketgroessen {sorted(set(sizes))}, Routen {list(per_route.values())}, Zustand {st[4]}, "
          f"Unterlaeufe {st[10]}, verworfen {st[8]}, Fuellstand {st[16]}..{st[17]}")
    check(len(got) > 0.6 * nblocks * 128 and bad == 0, f"{label}: alle {len(got)} Frames x {nch} Kanaele = erwartete Quelle ({bad} Fehler)")
    check(gaps == 0, f"{label}: lueckenlos ({gaps} Spruenge)")
    nz = [x for x in sizes if x]
    check(set(nz) <= {44 * 2 * nch, 45 * 2 * nch}, f"{label}: Paketgroessen {sorted(set(nz))} Byte (44/45 Frames)")
    check(st[10] == 0 and st[8] == 0, f"{label}: keine Unterlaeufe/verworfenen Bloecke")
    if stereo:
        allLR = {}
        for f0, blk in blocks.items():
            for k in range(128): allLR[f0 + k] = (blk[1][k], blk[2][k])
        ok = len(stereo_frames) > 1000
        # Stereo-Ring hat eigene Latenz: Frame-Folge muss als zusammenhaengendes Stueck in L/R vorkommen
        first = stereo_frames[len(stereo_frames) // 2]
        cand = [f for f, v in allLR.items() if v == first]
        ok = ok and any(all(allLR.get(c + j) == stereo_frames[len(stereo_frames) // 2 + j] for j in range(500)) for c in cand)
        check(ok, f"{label}: IF4 (Stereo) liefert parallel denselben Master ({len(stereo_frames)} Frames)")
    return per_route


def main():
    ppf, ref = sys.argv[1], sys.argv[2]
    img, refimg = load(ppf), load(ref)
    nm = {l.split()[2]: int(l.split()[0], 16) for l in os.popen("arm-none-eabi-nm " + os.path.join(HERE, "fx.elf")) if len(l.split()) == 3}
    stub, snm = build_stub()

    print("1. Mixer mit aktiven Hooks bitgleich zum Mixer ohne Hooks (012-Image)")
    rig = Rig(img, stub, snm); rig.nm = nm
    d = rig.dev
    d.call(nm["usbinit_hook"]); d.bus_reset()
    d.control(setup(0x00, 5, 9, 0, 0)); d.control(setup(0x00, 9, 1, 0, 0)); d.control(setup(0x01, 0x0B, 2, 5, 0))
    rref = Rig(refimg, stub, snm, mt=False)
    rref.nm = {"audio_tap": refsym(ref)}
    same, active = True, 0
    for b in range(24):
        for r in (rig, rref): r.vary_gains(b)
        pres = 0xFF if b % 3 else 0xB7
        h0 = d.r32(MT + 4)
        a = rig.block(b, present=pres); c = rref.block(b, present=pres)
        active += d.r32(MT + 4) != h0
        same &= a[1:] == c[1:]
        for _ in range(3): d.token(7, 1, 0x9)
    check(same and active >= 20, f"24 Bloecke (Lautstaerke/Pan wechselnd, fehlende Spuren), davon {active} mit Abgriff: L/R und Master-Summen identisch")
    d.control(setup(0x01, 0x0B, 0, 5, 0))               # IF5 aus: Hooks laufen leer (Zustand bei jedem Start ohne Aufnahme)
    same, hc0 = True, d.r32(MT + 0x6C)
    for b in range(24, 36):
        for r in (rig, rref): r.vary_gains(b)
        a = rig.block(b); c = rref.block(b)
        same &= a[1:] == c[1:]
    check(same and d.r32(MT + 0x6C) - hc0 == 12 * 10, f"12 Bloecke mit IF5 aus: identisch, Hooks {d.r32(MT + 0x6C) - hc0}x ohne Wirkung aufgerufen")

    print("2. Enumeration und IF5")
    rig = Rig(img, stub, snm); rig.nm = nm
    dev = rig.dev
    dev.call(nm["usbinit_hook"])
    check(dev.r32(MT) == 0x4D54A015, "Multitrack-Zustand initialisiert")
    dev.bus_reset()
    desc = bytes(img[nm["ua_cfg_desc"]:nm["ua_cfg_desc"] + 357])
    cd = dev.control(setup(0x80, 6, 0x0200, 0, 0xFFFF))
    check(cd == desc, f"Config-Deskriptor {len(cd) if cd else 0} Byte")
    dev.control(setup(0x00, 5, 9, 0, 0)); dev.control(setup(0x00, 9, 1, 0, 0))
    check(dev.hw.endpt(7) == 0x19, "ENDPT7 nach SET_CONFIGURATION wie Original (0x19)")
    check(dev.control(setup(0x81, 0x0A, 0, 5, 1)) == b"\x00", "GET_INTERFACE IF5 → 0")
    check(dev.control(setup(0x01, 0x0B, 3, 5, 0)) is None, "SET_INTERFACE IF5 alt 3 → STALL")
    check(dev.control(setup(0x01, 0x0B, 1, 5, 0)) == b"", "SET_INTERFACE IF5 alt 1")
    check(dev.hw.endpt(7) == 0x14, f"ENDPT7 = 0x{dev.hw.endpt(7):02x}")
    check(dev.control(setup(0x81, 0x0A, 0, 5, 1)) == b"\x01", "GET_INTERFACE IF5 → 1")
    check(dev.control(setup(0xA2, 0x81, 0x0100, 0x87, 3)) == bytes.fromhex("44ac00"), "GET_CUR Rate EP 0x87")

    print("3. Streaming 10 Kanaele, Preset A → B waehrend des Streamings, Stereo parallel")
    check(dev.control(setup(0x01, 0x0B, 1, 4, 0)) == b"", "SET_INTERFACE IF4 alt 1 (Stereo parallel)")
    switch_b = 60
    f_switch = {}
    def route_ab(f0):
        return PB10 if f0 >= f_switch.get("B", 1 << 40) else PA10
    # Block-Startframe des Umschaltens merken
    orig_block = rig.block
    def blk_hook(b, **kw):
        if b == switch_b: f_switch["B"] = dev.r32(CNT)
        return orig_block(b, **kw)
    rig.block = blk_hook
    stream(rig, 1, route_ab, 150, presets={switch_b: 1}, stereo=True, label="Alt 1")
    rig.block = orig_block

    print("4. Wechsel auf 11 Kanaele (Preset B), Spur-Lautstaerke aus")
    dev.uc.mem_write(CFG + 5, bytes([0]))
    check(dev.control(setup(0x01, 0x0B, 2, 5, 0)) == b"", "SET_INTERFACE IF5 alt 2")
    for _ in range(4): dev.token(7, 1, 0x9)            # alte (leere) Baenke abholen
    shared = {}
    stream(rig, 2, lambda f0: PB11, 120, label="Alt 2", blocks=shared)
    dev.uc.mem_write(CFG + 4, bytes([0]))
    fa = dev.r32(CNT)                                  # erster Block mit Preset A
    stream(rig, 2, lambda f0: PA11 if f0 >= fa else PB11, 60, label="Alt 2 Preset A (Umschalten ohne Neuanmeldung)", blocks=shared)

    print("5. Mixer ausserhalb von audio_tap (WAV-Export im Hauptprogramm)")
    head0, hc0 = dev.r32(MT + 4), dev.r32(MT + 0x6C)
    ring0 = bytes(dev.uc.mem_read(0x1FFFDE00, 0x2100))
    rig.block(999, via_tap=False)
    check(dev.r32(MT + 0x6C) == hc0 + 10 and dev.r32(MT + 4) == head0 and bytes(dev.uc.mem_read(0x1FFFDE00, 0x2100)) == ring0,
          f"10 Hook-Aufrufe ohne Wirkung (hook_calls {hc0}→{dev.r32(MT + 0x6C)}), Ring unveraendert")

    print("6. Stopp und Neukonfiguration")
    check(dev.control(setup(0x01, 0x0B, 0, 5, 0)) == b"", "SET_INTERFACE IF5 alt 0")
    check(dev.token(7, 1, 0x9) is None, "nach Stopp NAK auf EP7")
    dev.control(setup(0x01, 0x0B, 1, 5, 0))
    dev.control(setup(0x00, 9, 1, 0, 0))
    bm = dev.r32(0x1FFF1DD0)
    check(bin(bm >> 2).count("1") == 24, f"Paket-Pool nach SET_CONFIGURATION 24/30 frei (Bitmap 0x{bm:08x})")
    check(dev.control(setup(0x81, 0x0A, 0, 5, 1)) == b"\x00", "nach SET_CONFIGURATION GET_INTERFACE IF5 → 0")
    print(f"{checks} Pruefungen, {fails} Fehler")
    sys.exit(1 if fails else 0)


def refsym(ref):
    nmref = os.path.join(os.path.dirname(os.path.abspath(ref)), "fx.elf")
    for l in os.popen("arm-none-eabi-nm " + nmref):
        p = l.split()
        if len(p) == 3 and p[2] == "audio_tap": return int(p[0], 16)
    raise SystemExit("audio_tap im Referenz-ELF nicht gefunden")


if __name__ == "__main__":
    main()
