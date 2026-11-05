#!/usr/bin/env python3
"""Formale Pruefung eines USB-Konfigurationsdeskriptors (USB 2.0 Kap. 9, IAD-ECN, USB Audio Class 1.0, USB-MIDI 1.0).

usage: check_desc.py DESC.bin [--orig firmware.bin] [--orig-off 0xAED68 --orig-len 181]
  DESC.bin   Konfigurationsdeskriptor (z. B. von ua_test geschrieben oder per extract aus fx.bin)
  --orig     Original-Image: prueft zusaetzlich, dass CDC und MIDI byte-identisch uebernommen sind
Exit 0 = alles OK.
"""
import sys

errors, notes = [], []
def err(m): errors.append(m)
def ok(m): notes.append(m)

def u16(b, o): return b[o] | (b[o + 1] << 8)

def parse(d):
    out, p = [], 0
    while p < len(d):
        if p + 2 > len(d): err(f"@{p}: abgeschnittener Deskriptor"); break
        l, t = d[p], d[p + 1]
        if l < 2 or p + l > len(d): err(f"@{p}: ungueltige bLength {l}"); break
        out.append((p, t, d[p:p + l])); p += l
    return out

def main():
    a = sys.argv[1:]
    d = open(a[0], "rb").read()
    orig = None
    if "--orig" in a:
        img = open(a[a.index("--orig") + 1], "rb").read()
        off = int(a[a.index("--orig-off") + 1], 16) if "--orig-off" in a else 0xAED68
        ln = int(a[a.index("--orig-len") + 1], 0) if "--orig-len" in a else 181
        orig = img[off:off + ln]
    ds = parse(d)
    if not ds or ds[0][1] != 2 or len(ds[0][2]) != 9: err("erster Deskriptor ist kein 9-Byte-CONFIGURATION"); report(); return
    cfg = ds[0][2]
    if u16(cfg, 2) != len(d): err(f"wTotalLength {u16(cfg,2)} != Laenge {len(d)}")
    else: ok(f"wTotalLength = {len(d)}")
    if not (cfg[7] & 0x80): err("bmAttributes Bit 7 muss gesetzt sein")

    # Interfaces/Endpunkte einsammeln
    ifs = {}            # (num, alt) -> dict
    cur = None; iad = []
    for p, t, b in ds[1:]:
        if t == 0x0B:
            if len(b) != 8: err(f"@{p}: IAD bLength {len(b)}")
            iad.append(b)
        elif t == 0x04:
            if len(b) != 9: err(f"@{p}: INTERFACE bLength {len(b)}")
            key = (b[2], b[3])
            if key in ifs: err(f"@{p}: Interface {key} doppelt")
            cur = {"off": p, "d": b, "eps": [], "cs": []}; ifs[key] = cur
        elif t == 0x05:
            if cur is None: err(f"@{p}: Endpunkt ausserhalb eines Interfaces"); continue
            cur["eps"].append({"off": p, "d": b, "cs": []})
        elif t in (0x24, 0x21):
            if cur is None: err(f"@{p}: Klassen-Deskriptor ausserhalb eines Interfaces"); continue
            cur["cs"].append((p, b))
        elif t == 0x25:
            if cur is None or not cur["eps"]: err(f"@{p}: CS_ENDPOINT ohne Endpunkt"); continue
            cur["eps"][-1]["cs"].append((p, b))
        else:
            err(f"@{p}: unerwarteter Deskriptortyp 0x{t:02x}")
    nums = sorted({k[0] for k in ifs})
    if nums != list(range(len(nums))): err(f"Interfacenummern nicht lueckenlos: {nums}")
    if cfg[4] != len(nums): err(f"bNumInterfaces {cfg[4]} != {len(nums)}")
    else: ok(f"bNumInterfaces = {cfg[4]}")
    for n in nums:
        alts = sorted(k[1] for k in ifs if k[0] == n)
        if alts != list(range(len(alts))): err(f"Interface {n}: Alternate Settings nicht lueckenlos {alts}")
    for k, v in ifs.items():
        if v["d"][4] != len(v["eps"]): err(f"Interface {k}: bNumEndpoints {v['d'][4]} != {len(v['eps'])}")
    for b in iad:
        first, cnt = b[2], b[3]
        if first + cnt > len(nums) or cnt < 1: err(f"IAD {first}+{cnt} ausserhalb")
    # Endpunktadressen: innerhalb einer Konfiguration eindeutig ueber verschiedene Interfaces
    owner, periodic = {}, {}
    for (n, alt), v in ifs.items():
        for e in v["eps"]:
            b = e["d"]; addr, attr, mps, ival = b[2], b[3], u16(b, 4), b[6]
            if addr & 0x70: err(f"EP 0x{addr:02x}: reservierte Bits")
            if (addr & 0x0F) == 0: err("Endpunkt 0 im Konfigurationsdeskriptor")
            if (addr & 0x0F) > 15: err("Endpunktnummer > 15")
            if addr in owner and owner[addr] != n: err(f"EP 0x{addr:02x} in Interface {owner[addr]} und {n}")
            owner[addr] = n
            typ = attr & 3
            if typ == 1:
                if mps > 1023: err(f"EP 0x{addr:02x}: isochron FS max 1023, ist {mps}")
                if ival != 1: err(f"EP 0x{addr:02x}: isochron FS bInterval muss 1 sein")
                if alt == 0: err(f"EP 0x{addr:02x}: isochroner Endpunkt in Alt 0 (Zero-Bandwidth-Regel)")
            elif typ == 2:
                if mps not in (8, 16, 32, 64): err(f"EP 0x{addr:02x}: Bulk FS wMaxPacketSize {mps}")
            elif typ == 3:
                if mps > 64: err(f"EP 0x{addr:02x}: Interrupt FS > 64")
                if ival < 1: err(f"EP 0x{addr:02x}: Interrupt bInterval 0")
            if typ in (1, 3):
                periodic.setdefault(n, {}).setdefault(alt, 0)
                periodic[n][alt] += mps if typ == 1 else mps / max(ival, 1)
    # Bandbreite: schlimmster Fall = je Interface das teuerste Alt Setting
    worst = sum(max(a.values()) for a in periodic.values())
    if worst > 1500 * 0.9: err(f"periodische Bandbreite {worst:.0f} B/Frame > 90 % von FS")
    else: ok(f"periodische Nutzlast im schlimmsten Fall {worst:.0f} B/Frame (FS-Grenze 1350)")

    # ---------------- Audio Class 1.0 ----------------
    ac = [(k, v) for k, v in ifs.items() if v["d"][5] == 1 and v["d"][6] == 1]
    ms = [(k, v) for k, v in ifs.items() if v["d"][5] == 1 and v["d"][6] == 3]
    asx = [(k, v) for k, v in ifs.items() if v["d"][5] == 1 and v["d"][6] == 2]
    if len(ac) != 1: err(f"{len(ac)} AudioControl-Interfaces (erwartet 1)")
    terms = {}
    usb_terms = set()
    if ac:
        (acn, acalt), v = ac[0]
        if v["d"][4] not in (0, 1): err("AC-Interface: 0 oder 1 Endpunkt (Interrupt) erlaubt")
        cs = v["cs"]
        if not cs or cs[0][1][2] != 0x01: err("AC: erster CS-Deskriptor muss HEADER sein")
        else:
            h = cs[0][1]
            if u16(h, 3) != 0x0100: err(f"AC-Header bcdADC 0x{u16(h,3):04x} != 0x0100")
            tot = sum(len(b) for _, b in cs)
            if u16(h, 5) != tot: err(f"AC-Header wTotalLength {u16(h,5)} != {tot}")
            n = h[7]
            if len(h) != 8 + n: err(f"AC-Header bLength {len(h)} != 8+{n}")
            coll = list(h[8:8 + n])
            for i in coll:
                if not any(k[0] == i and x["d"][5] == 1 and x["d"][6] in (2, 3) for k, x in ifs.items()):
                    err(f"AC-Header listet Interface {i}, das kein AS/MS-Interface ist")
            for (k, x) in asx:
                if k[0] not in coll: err(f"AS-Interface {k[0]} fehlt in baInterfaceNr")
            ok(f"AC-Header: bcdADC 1.00, wTotalLength {tot}, Sammlung {coll}")
        for p, b in cs[1:]:
            st = b[2]
            if st in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08):
                tid = b[3]
                if tid == 0 or tid in terms: err(f"@{p}: Entity-ID {tid} ungueltig/doppelt")
                terms[tid] = (st, b)
            if st == 0x02:
                if len(b) != 12: err(f"@{p}: INPUT_TERMINAL bLength {len(b)}")
                if u16(b, 4) == 0x0101: usb_terms.add(b[3])
                if b[7] < 1: err(f"@{p}: IT ohne Kanaele")
            elif st == 0x03:
                if len(b) != 9: err(f"@{p}: OUTPUT_TERMINAL bLength {len(b)}")
                if u16(b, 4) == 0x0101: usb_terms.add(b[3])
        for tid, (st, b) in terms.items():
            if st == 0x03 and b[7] not in terms: err(f"OT {tid}: bSourceID {b[7]} unbekannt")
            if st in (0x02, 0x03) and b[6] and b[6] not in terms: err(f"Terminal {tid}: bAssocTerminal unbekannt")
            if st in (0x02, 0x03) and u16(b, 4) == 0x0100: err(f"Terminal {tid}: Typ 0x0100 (undefined) nicht erlaubt")
        # USB-Streaming-Terminal darf nicht direkt mit einem anderen USB-Streaming-Terminal verbunden sein
        for tid, (st, b) in terms.items():
            if st == 0x03 and u16(b, 4) == 0x0101 and b[7] in terms and terms[b[7]][0] == 0x02 and u16(terms[b[7]][1], 4) == 0x0101:
                err(f"OT {tid}: USB-Streaming → USB-Streaming")

    for (n, alt), v in sorted(asx):
        if alt == 0:
            if v["d"][4] != 0: err(f"AS {n} Alt 0 muss 0 Endpunkte haben")
            continue
        cs = v["cs"]
        gen = [b for _, b in cs if b[2] == 0x01]
        fmt = [b for _, b in cs if b[2] == 0x02]
        if len(gen) != 1 or len(gen[0]) != 7: err(f"AS {n}/{alt}: AS_GENERAL fehlt/falsch"); continue
        g = gen[0]
        link = g[3]
        if link not in usb_terms: err(f"AS {n}/{alt}: bTerminalLink {link} ist kein USB-Streaming-Terminal")
        if u16(g, 5) != 0x0001: notes.append(f"AS {n}/{alt}: wFormatTag 0x{u16(g,5):04x}")
        if len(fmt) != 1: err(f"AS {n}/{alt}: FORMAT_TYPE fehlt"); continue
        f = fmt[0]
        if f[3] != 1: err(f"AS {n}/{alt}: nur FORMAT_TYPE_I geprueft")
        ch, sub, bits, nfreq = f[4], f[5], f[6], f[7]
        if sub not in (1, 2, 3, 4) or bits > 8 * sub: err(f"AS {n}/{alt}: Subframe {sub} / {bits} Bit")
        if nfreq == 0:
            if len(f) != 14: err(f"AS {n}/{alt}: kontinuierlicher Bereich: bLength 14 erwartet")
            freqs = [f[8] | f[9] << 8 | f[10] << 16, f[11] | f[12] << 8 | f[13] << 16]
        else:
            if len(f) != 8 + 3 * nfreq: err(f"AS {n}/{alt}: bLength {len(f)} != 8+3*{nfreq}")
            freqs = [f[8 + 3 * i] | f[9 + 3 * i] << 8 | f[10 + 3 * i] << 16 for i in range(nfreq)]
        # Kanalzahl gegen Terminal
        if link in terms:
            st, tb = terms[link]
            src = tb if st == 0x02 else terms.get(tb[7], (None, None))[1]
            # fuer OT (Geraet → Host) den Kanalsatz am Quell-IT pruefen
            if st == 0x03:
                s = tb[7]
                while s in terms and terms[s][0] != 0x02: s = terms[s][1][4] if terms[s][0] in (0x06,) else None
                if s in terms and terms[s][1][7] != ch: err(f"AS {n}/{alt}: {ch} Kanaele, Quell-IT hat {terms[s][1][7]}")
            elif tb[7] != ch: err(f"AS {n}/{alt}: {ch} Kanaele, IT hat {tb[7]}")
        if len(v["eps"]) < 1: err(f"AS {n}/{alt}: kein Endpunkt"); continue
        e = v["eps"][0]; b = e["d"]
        if len(b) != 9: err(f"AS {n}/{alt}: Audio-Endpunkt braucht 9-Byte-Deskriptor (bRefresh/bSynchAddress)")
        if (b[3] & 3) != 1: err(f"AS {n}/{alt}: Datenendpunkt nicht isochron")
        sync = (b[3] >> 2) & 3
        dir_in = bool(b[2] & 0x80)
        if link in terms:
            st = terms[link][0]
            if dir_in and st != 0x03: err(f"AS {n}/{alt}: IN-Endpunkt muss an Output Terminal haengen")
            if not dir_in and st != 0x02: err(f"AS {n}/{alt}: OUT-Endpunkt muss an Input Terminal haengen")
        need = (max(freqs) + 999) // 1000 * ch * sub
        if sync == 1:   # asynchron: Geraet darf +1 Frame schicken
            need = ((max(freqs) + 999) // 1000 + 1) * ch * sub
        if u16(b, 4) < need: err(f"AS {n}/{alt}: wMaxPacketSize {u16(b,4)} < benoetigt {need}")
        if u16(b, 4) % (ch * sub): err(f"AS {n}/{alt}: wMaxPacketSize kein Vielfaches der Framegroesse")
        if b[7] != 0: err(f"AS {n}/{alt}: bRefresh muss 0 sein (Datenendpunkt)")
        if sync == 1 and not dir_in and b[8] == 0: err(f"AS {n}/{alt}: asynchroner OUT-Endpunkt braucht Feedback (bSynchAddress)")
        if sync == 1 and dir_in and b[8] != 0: err(f"AS {n}/{alt}: asynchroner IN-Endpunkt hat keinen Sync-Endpunkt")
        csep = [x for _, x in e["cs"] if x[2] == 0x01]
        if len(csep) != 1 or len(csep[0]) != 7: err(f"AS {n}/{alt}: CS_ENDPOINT EP_GENERAL fehlt/falsch")
        elif csep[0][3] & 0x01: notes.append(f"AS {n}/{alt}: Sampling Frequency Control gemeldet → Host sendet SET_CUR")
        ok(f"AS {n}/{alt}: EP 0x{b[2]:02x} {'IN' if dir_in else 'OUT'} isochron {['none','async','adaptive','sync'][sync]}, "
           f"{ch} Kan., {bits} Bit, {freqs} Hz, wMaxPacketSize {u16(b,4)} (min {need})")

    for (n, alt), v in ms:
        cs = v["cs"]
        if not cs or cs[0][1][2] != 0x01: err(f"MS {n}: MS_HEADER fehlt")
        else:
            h = cs[0][1]
            tot = sum(len(b) for _, b in cs)
            if u16(h, 5) != tot: notes.append(f"MS {n}: wTotalLength {u16(h,5)} (CS-Teil {tot}, wie Original Teensy)")
        for e in v["eps"]:
            if len(e["d"]) != 9: err(f"MS {n}: Endpunkt 0x{e['d'][2]:02x} braucht 9-Byte-Deskriptor")
            if not any(x[2] == 0x01 for _, x in e["cs"]): err(f"MS {n}: CS-Endpunkt fehlt")

    if orig is not None:
        o = bytearray(orig); nn = bytearray(d[:len(orig)])
        # gemeinsamer Teil: alles vor dem HID-Interface
        hid = None
        for p, t, b in parse(orig):
            if t == 4 and b[5] == 3: hid = p; break
        if hid is None: err("Original: HID-Interface nicht gefunden")
        else:
            a_, b_ = bytes(o[9:hid]), bytes(d[9:hid])
            if a_ != b_: err("CDC/MIDI-Teil weicht vom Original ab")
            else: ok(f"Bytes 9..{hid} (IAD, CDC, MIDI) identisch mit dem Original")
            if bytes(o[5:9]) != bytes(d[5:9]): err("bConfigurationValue/iConfiguration/bmAttributes/bMaxPower geaendert")
    report()

def report():
    for m in notes: print("  ok  ", m)
    for m in errors: print("  FEHLER", m)
    print("ERGEBNIS:", "OK" if not errors else f"{len(errors)} Fehler")
    sys.exit(1 if errors else 0)

if __name__ == "__main__":
    main()
