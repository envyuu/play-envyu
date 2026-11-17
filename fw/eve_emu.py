#!/usr/bin/env python3
"""Nachbildung des Grafikchips FT81x ("EVE", REG_ID 0x7C) fuer den Unicorn-Emulator (Patch 023).

Die Firmware (Rev A) spricht den Chip ueber SPI2 an (Objekt 0x20000904, Methode vt+0xC = 1 Byte tauschen; CS ueber
FUN_00000748/FUN_00000770). Dieses Modell
  * nimmt die SPI-Bytes entgegen (Adresse 3 Byte, Bit 7 = Schreiben; Lesen mit 1 Blindbyte),
  * bildet RAM_G (1 MB), RAM_DL (8 KB), die Register (0x302000…) und den Befehlsring RAM_CMD (4 KB, 0x308000) nach,
  * fuehrt den Koprozessor fuer die Befehle aus, die die Firmware benutzt (DLSTART, SWAP, TEXT, GRADIENT, MEMCPY,
    APPEND, SETFONT2, SETBITMAP, LOADIDENTITY/TRANSLATE/ROTATE/SETMATRIX, …) und schreibt die Display-Liste nach RAM_DL,
  * rastert die Display-Liste bei CMD_SWAP / REG_DLSWAP in ein 320×240-Bild (numpy, RGBA, ohne Kantenglaettung).

Vereinfachungen (bewusst, fuer den Bildnachweis unerheblich):
  * keine Kantenglaettung, Linien/Punkte/Rechtecke mit harter Kante; Bitmap-Transformationen (Drehen) werden ignoriert;
  * CMD_GRADIENT wird als Pseudo-Befehl 0xFE000000|n in die Display-Liste geschrieben (der echte Chip erzeugt dafuer
    eigene Bitmap-Befehle) und beim Rastern als Verlauf im aktuellen Scissor-Rechteck gemalt;
  * Stencil/Tag werden ignoriert.
"""
import struct
import numpy as np

W, H = 320, 240
RAM_DL = 0x300000
REG = 0x302000
RAM_CMD = 0x308000
REG_ID, REG_CPURESET, REG_DLSWAP = 0x302000, 0x302020, 0x302054
REG_CMD_READ, REG_CMD_WRITE, REG_CMD_DL = 0x3020F8, 0x3020FC, 0x302100
OPT_CENTERX, OPT_CENTERY, OPT_RIGHTX = 0x200, 0x400, 0x800


class Eve:
    def __init__(self):
        self.mem = bytearray(0x400000)
        self.w32(REG_ID, 0x7C)
        self.cs = False
        self.tx = []
        self.addr = None
        self.write = False
        self.rd_skip = 0
        self.rp = 0                 # Koprozessor-Lesezeiger im Ring
        self.dlp = 0                # Schreibzeiger in RAM_DL (Koprozessor)
        self.fonts = {}             # handle -> (metric_addr, firstchar)
        self.grad = []              # Pseudo-Befehle fuer CMD_GRADIENT
        self.romtext = []           # Pseudo-Befehle fuer Text in ROM-Fonts 16…31 (angenaehert)
        self.frames = []            # gerenderte Bilder (numpy uint8 HxWx3)
        self.handles = {}           # Bitmap-Handle-Zustand bleibt ueber Frames erhalten (wie im Chip)
        self.cmd_log = []           # (code, args) aller Koprozessor-Befehle (fuer Pruefungen)
        self.spi_bytes = 0

    # ------------------------------------------------------------------ Speicher
    def r32(self, a): return struct.unpack_from("<I", self.mem, a)[0]
    def w32(self, a, v): struct.pack_into("<I", self.mem, a, v & 0xFFFFFFFF)

    # ------------------------------------------------------------------ SPI
    def cs_low(self):
        self.cs = True; self.tx = []; self.addr = None
    def cs_high(self):
        if not self.cs: return
        self.cs = False
        if self.addr is not None and self.write and self.last_reg_writes:
            if REG_CMD_WRITE in self.last_reg_writes: self.run_copro()
            if REG_DLSWAP in self.last_reg_writes and self.r32(REG_DLSWAP) & 3:
                self.render(); self.w32(REG_DLSWAP, 0)
        self.last_reg_writes = set()
    last_reg_writes = set()

    def xfer(self, b):
        """Ein Byte tauschen; Rueckgabe = gelesenes Byte."""
        self.spi_bytes += 1
        if not self.cs: return 0                      # anderes Geraet am Bus (z. B. Panel-Init des IPS-TFT)
        if self.addr is None:
            self.tx.append(b & 0xFF)
            if len(self.tx) == 3:
                a = ((self.tx[0] & 0x3F) << 16) | (self.tx[1] << 8) | self.tx[2]
                if self.tx[0] & 0xC0 == 0x80: self.write = True; self.addr = a
                elif self.tx[0] & 0xC0 == 0x00: self.write = False; self.addr = a; self.rd_skip = 1
                else: self.addr = None; self.tx = [0, 0, 0, 0]      # Host-Befehl, ignorieren
            return 0
        if self.write:
            a = self.addr
            self.mem[a] = b & 0xFF
            if REG <= a < REG + 0x1000: self.last_reg_writes.add(a & ~3)
            a += 1
            if a == RAM_CMD + 0x1000: a = RAM_CMD                  # Ring
            self.addr = a
            return 0
        if self.rd_skip: self.rd_skip -= 1; return 0
        if (self.addr & ~3) == REG_CMD_READ: self.w32(REG_CMD_READ, self.rp)     # sofort fertig
        if (self.addr & ~3) == REG_CMD_DL: self.w32(REG_CMD_DL, self.dlp)
        v = self.mem[self.addr]; self.addr += 1
        return v

    # ------------------------------------------------------------------ Koprozessor
    def cmd_word(self):
        v = self.r32(RAM_CMD + self.rp); self.rp = (self.rp + 4) & 0xFFF; return v
    def dl(self, w):
        if self.dlp < 0x2000: self.w32(RAM_DL + self.dlp, w)
        self.dlp += 4
    def run_copro(self):
        wp = self.r32(REG_CMD_WRITE) & 0xFFF
        guard = 0
        while self.rp != wp and guard < 100000:
            guard += 1
            w = self.cmd_word()
            if w >> 24 != 0xFF: self.dl(w); continue
            c = w & 0xFF
            if c == 0x00: self.dlp = 0; self.log(c)                               # DLSTART
            elif c == 0x01: self.log(c); self.render()                            # SWAP
            elif c == 0x0C:                                                       # TEXT
                xy = self.cmd_word(); fo = self.cmd_word()
                x = (xy & 0xFFFF); x = x - 0x10000 if x & 0x8000 else x
                y = xy >> 16; y = y - 0x10000 if y & 0x8000 else y
                font, opt = fo & 0xFFFF, fo >> 16
                s = bytearray()
                while True:
                    b4 = struct.pack("<I", self.cmd_word()); s += b4
                    if 0 in b4: break
                s = bytes(s).split(b"\0")[0]
                self.log(c, x, y, font, opt, s.decode("latin1"))
                self.text(x, y, font, opt, s)
            elif c == 0x0B:                                                       # GRADIENT
                a = [self.cmd_word() for _ in range(4)]
                self.grad.append(a); self.dl(0xFE000000 | (len(self.grad) - 1)); self.log(c, *a)
            elif c == 0x1D:                                                       # MEMCPY
                d, s_, n = (self.cmd_word() for _ in range(3))
                self.mem[d:d + n] = self.mem[s_:s_ + n]; self.log(c, d, s_, n)
            elif c == 0x1E:                                                       # APPEND
                p, n = self.cmd_word(), self.cmd_word()
                for i in range(0, n, 4): self.dl(self.r32(p + i))
                self.log(c, p, n)
            elif c == 0x3B:                                                       # SETFONT2
                f, p, first = (self.cmd_word() for _ in range(3))
                self.fonts[f] = (p, first); self.log(c, f, p, first)
                fmt, stride, fw, fh, ptr = struct.unpack_from("<5I", self.mem, p + 128)
                self.dl(0x05000000 | f)
                self.dl(0x01000000 | ((ptr - first * stride * fh) & 0x3FFFFF))
            elif c == 0x43:                                                       # SETBITMAP
                addr, fw_, hh = (self.cmd_word() for _ in range(3))
                fmt, w = fw_ & 0xFFFF, fw_ >> 16
                stride = {1: (w + 7) // 8, 2: (w + 1) // 2, 3: w, 4: w, 7: 2 * w, 6: 2 * w, 0: 2 * w}.get(fmt, w)
                self.dl(0x01000000 | (addr & 0x3FFFFF))
                self.dl(0x07000000 | (fmt << 19) | ((stride & 0x3FF) << 9) | (hh & 0x1FF))
                self.dl(0x08000000 | ((w & 0x1FF) << 9) | (hh & 0x1FF))
                self.log(c, addr, fmt, w, hh)
            elif c in (0x26, 0x2A): self.log(c)                                   # LOADIDENTITY / SETMATRIX
            elif c in (0x27, 0x28): self.log(c, self.cmd_word(), self.cmd_word())  # TRANSLATE / SCALE
            elif c == 0x29: self.log(c, self.cmd_word())                          # ROTATE
            elif c in (0x09, 0x0A): self.log(c, self.cmd_word())                  # BG/FGCOLOR
            else: raise RuntimeError(f"EVE-Modell: unbekannter Koprozessor-Befehl 0xFFFFFF{c:02X}")
        self.w32(REG_CMD_READ, self.rp)
        self.w32(REG_CMD_DL, self.dlp)
    def log(self, c, *a): self.cmd_log.append((c,) + a)

    def font_metrics(self, font):
        p, first = self.fonts[font]
        widths = self.mem[p:p + 128]
        fmt, stride, fw, fh, ptr = struct.unpack_from("<5I", self.mem, p + 128)
        return widths, fh, first
    def text_width(self, font, s):
        widths, fh, first = self.font_metrics(font)
        return sum(widths[ch] for ch in s if ch < 128)
    def text(self, x, y, font, opt, s):
        if font not in self.fonts:
            if 16 <= font <= 31:                      # ROM-Font (Daten nicht im Firmware-Image): angenaehert
                self.romtext.append((x, y, font, opt, s.decode("latin1"))); self.dl(0xFD000000 | (len(self.romtext) - 1)); return
            raise RuntimeError(f"EVE-Modell: Font {font} nicht gesetzt")
        widths, fh, first = self.font_metrics(font)
        tw = sum(widths[ch] for ch in s if ch < 128)
        if opt & OPT_RIGHTX: x -= tw
        elif opt & OPT_CENTERX: x -= tw // 2
        if opt & OPT_CENTERY: y -= fh // 2
        self.dl(0x22000000)                       # SAVE_CONTEXT
        self.dl(0x05000000 | font)                # BITMAP_HANDLE
        self.dl(0x1F000001)                       # BEGIN BITMAPS
        for ch in s:
            if ch >= 128: continue
            if 0 <= x < 512 and 0 <= y < 512:
                self.dl(0x80000000 | ((x & 0x1FF) << 21) | ((y & 0x1FF) << 12) | ((font & 0x1F) << 7) | (ch & 0x7F))
            x += widths[ch]
        self.dl(0x21000000)                       # END
        self.dl(0x23000000)                       # RESTORE_CONTEXT

    # ------------------------------------------------------------------ Rastern
    def render(self):
        fb = np.zeros((H, W, 4), np.float32)
        ctx = dict(color=(1.0, 1.0, 1.0), a=1.0, lw=16, ps=16, sc=(0, 0, W, H), bs=2, bd=4, handle=0, cell=0, vf=4,
                   cc=(0.0, 0.0, 0.0), ca=0.0)
        stack = []; prim = 0; verts = []
        self.stats = dict(words=0, prims={})
        def blend(mask, src_rgb, src_a):
            x0, y0, w, h = ctx["sc"]
            m = np.zeros((H, W), bool); m[max(0, y0):min(H, y0 + h), max(0, x0):min(W, x0 + w)] = True
            mask = mask & m
            if not mask.any(): return
            sa = np.broadcast_to(np.asarray(src_a, np.float32), (H, W))[mask]
            sr = np.broadcast_to(np.asarray(src_rgb, np.float32), (H, W, 3))[mask]
            src = np.concatenate([sr, sa[:, None]], 1)
            dst = fb[mask]
            def f(code, s_, d_):
                return {0: 0.0, 1: 1.0, 2: s_[:, 3:4], 3: d_[:, 3:4], 4: 1 - s_[:, 3:4], 5: 1 - d_[:, 3:4]}[code]
            out = src * f(ctx["bs"], src, dst) + dst * f(ctx["bd"], src, dst)
            fb[mask] = np.clip(out, 0, 1)
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
        def draw(vs):
            """Jeder Vertex traegt den Zustand zum Zeitpunkt seiner Ausgabe (Farbe, Alpha, Breiten, Scissor, Blend)."""
            self.stats["prims"][prim] = self.stats["prims"].get(prim, 0) + 1
            def use(st):
                ctx.update(sc=st["sc"], bs=st["bs"], bd=st["bd"])
                return np.array(st["color"], np.float32), st["a"]
            keep = dict(sc=ctx["sc"], bs=ctx["bs"], bd=ctx["bd"])
            if prim == 9:                                                      # RECTS
                for (x0, y0, *_), (x1, y1, _h, _c, st) in zip(vs[0::2], vs[1::2]):
                    col, a = use(st); r = st["lw"] / 16.0
                    xa, xb = min(x0, x1), max(x0, x1); ya, yb = min(y0, y1), max(y0, y1)
                    if r <= 1.0:
                        m = (xx >= xa) & (xx <= xb) & (yy >= ya) & (yy <= yb)
                    else:
                        dx = np.maximum(np.maximum(xa - xx, xx - xb), 0); dy = np.maximum(np.maximum(ya - yy, yy - yb), 0)
                        m = dx * dx + dy * dy <= (r - 1) ** 2 + 0.5
                    blend(m, col, a)
            elif prim in (3, 4):                                               # LINES / LINE_STRIP
                segs = list(zip(vs[0::2], vs[1::2])) if prim == 3 else list(zip(vs[:-1], vs[1:]))
                for (x0, y0, *_), (x1, y1, _h, _c, st) in segs:
                    col, a = use(st); hw = max(st["lw"] / 16.0, 0.5)
                    dx, dy = x1 - x0, y1 - y0; L2 = dx * dx + dy * dy
                    t = np.clip(((xx - x0) * dx + (yy - y0) * dy) / L2, 0, 1) if L2 else 0
                    px, py = x0 + t * dx, y0 + t * dy
                    blend((xx - px) ** 2 + (yy - py) ** 2 <= hw * hw * 0.25 + 0.25, col, a)
            elif prim == 2:                                                    # POINTS
                for (x0, y0, _h, _c, st) in vs:
                    col, a = use(st); r = st["ps"] / 16.0
                    blend((xx - x0) ** 2 + (yy - y0) ** 2 <= r * r, col, a)
            elif prim == 1:                                                    # BITMAPS
                for (x0, y0, hd, cl, st) in vs:
                    col, a = use(st); self.blit(fb, blend, int(x0), int(y0), hd, cl, col, a)
            ctx.update(keep)
        def vstate():
            return dict(color=ctx["color"], a=ctx["a"], lw=ctx["lw"], ps=ctx["ps"], sc=ctx["sc"], bs=ctx["bs"], bd=ctx["bd"])
        words = []
        p = 0
        while p < 0x2000:
            w = self.r32(RAM_DL + p); p += 4; words.append(w)
            self.stats["words"] += 1
            top = w >> 30
            if top == 1:                                                       # VERTEX2F
                x = (w >> 15) & 0x7FFF; y = w & 0x7FFF
                x = x - 0x8000 if x & 0x4000 else x; y = y - 0x8000 if y & 0x4000 else y
                sc = 1 << ctx["vf"]; verts.append((x / sc, y / sc, ctx["handle"], ctx["cell"], vstate())); continue
            if top == 2:                                                       # VERTEX2II
                verts.append(((w >> 21) & 0x1FF, (w >> 12) & 0x1FF, (w >> 7) & 0x1F, w & 0x7F, vstate())); continue
            op = w >> 24
            if op == 0x00: break                                               # DISPLAY
            elif op == 0x02: ctx["cc"] = ((w >> 16 & 255) / 255, (w >> 8 & 255) / 255, (w & 255) / 255)
            elif op == 0x0F: ctx["ca"] = (w & 255) / 255
            elif op == 0x26:
                if w & 4:
                    x0, y0, sw, sh = ctx["sc"]
                    fb[max(0, y0):y0 + sh, max(0, x0):x0 + sw, :3] = ctx["cc"]; fb[max(0, y0):y0 + sh, max(0, x0):x0 + sw, 3] = ctx["ca"]
            elif op == 0x04: ctx["color"] = ((w >> 16 & 255) / 255, (w >> 8 & 255) / 255, (w & 255) / 255)
            elif op == 0x10: ctx["a"] = (w & 255) / 255
            elif op == 0x05: ctx["handle"] = w & 0x1F
            elif op == 0x06: ctx["cell"] = w & 0x7F
            elif op == 0x01: self.handles.setdefault(ctx["handle"], {})["src"] = w & 0x3FFFFF
            elif op == 0x07: self.handles.setdefault(ctx["handle"], {}).update(fmt=(w >> 19) & 0x1F, stride=(w >> 9) & 0x3FF, lh=w & 0x1FF)
            elif op == 0x08: self.handles.setdefault(ctx["handle"], {}).update(w=(w >> 9) & 0x1FF, h=w & 0x1FF)
            elif op == 0x0B: ctx["bs"], ctx["bd"] = (w >> 3) & 7, w & 7
            elif op == 0x0D: ctx["ps"] = w & 0x1FFF
            elif op == 0x0E: ctx["lw"] = w & 0xFFF
            elif op == 0x1B: sc = ctx["sc"]; ctx["sc"] = ((w >> 11) & 0x7FF, w & 0x7FF, sc[2], sc[3])
            elif op == 0x1C: sc = ctx["sc"]; ctx["sc"] = (sc[0], sc[1], (w >> 12) & 0xFFF, w & 0xFFF)
            elif op == 0x1F: prim = w & 0xF; verts = []
            elif op == 0x21: draw(verts); verts = []; prim = 0
            elif op == 0x22: stack.append(dict(ctx))
            elif op == 0x23:
                if stack: ctx = stack.pop()
            elif op == 0x27: ctx["vf"] = w & 7
            elif op == 0xFE: self.gradient(fb, blend, ctx, self.grad[w & 0xFFFFFF])
            elif op == 0xFD: self.rom_text(fb, blend, ctx, self.romtext[w & 0xFFFFFF])
            elif op in (0x03, 0x09, 0x0A, 0x0C, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x20, 0x2D): pass
            else: raise RuntimeError(f"EVE-Modell: unbekannter DL-Befehl {w:08X} bei {p - 4:#x}")
        if verts and prim: draw(verts)
        img = (fb[:, :, :3] * 255 + 0.5).astype(np.uint8)
        self.frames.append(img)
        self.last_dl = words
        return img

    def gradient(self, fb, blend, ctx, g):
        xy0, rgb0, xy1, rgb1 = g
        def sxy(v):
            x = v & 0xFFFF; y = v >> 16
            return (x - 0x10000 if x & 0x8000 else x), (y - 0x10000 if y & 0x8000 else y)
        (x0, y0), (x1, y1) = sxy(xy0), sxy(xy1)
        c0 = np.array([(rgb0 >> 16 & 255), (rgb0 >> 8 & 255), rgb0 & 255], np.float32) / 255
        c1 = np.array([(rgb1 >> 16 & 255), (rgb1 >> 8 & 255), rgb1 & 255], np.float32) / 255
        yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
        dx, dy = x1 - x0, y1 - y0; L2 = dx * dx + dy * dy or 1
        t = np.clip(((xx - x0) * dx + (yy - y0) * dy) / L2, 0, 1)[:, :, None]
        rgb = c0 * (1 - t) + c1 * t
        blend(np.ones((H, W), bool), rgb, ctx["a"])

    def rom_text(self, fb, blend, ctx, t):
        from PIL import Image, ImageDraw, ImageFont
        x, y, font, opt, s = t
        size = {16: 8, 17: 8, 18: 16, 19: 16, 20: 8, 21: 13, 22: 16, 23: 20, 24: 25, 25: 32, 26: 12, 27: 15, 28: 18, 29: 22, 30: 28, 31: 36}[font]
        try: f = ImageFont.load_default(size=size)
        except TypeError: f = ImageFont.load_default()
        im = Image.new("L", (W, H), 0); dr = ImageDraw.Draw(im)
        tw = dr.textlength(s, font=f)
        if opt & OPT_RIGHTX: x -= tw
        elif opt & OPT_CENTERX: x -= tw / 2
        if opt & OPT_CENTERY: y -= size / 2
        dr.text((x, y), s, fill=255, font=f)
        al = np.asarray(im, np.float32) / 255
        blend(al > 0, np.array(ctx["color"], np.float32), al * ctx["a"])

    def blit(self, fb, blend, x0, y0, hd, cell, col, a):
        h = self.handles.get(hd)
        if not h or "src" not in h or "fmt" not in h: return
        fmt, stride, lh, bw, bh = h["fmt"], h["stride"], h["lh"], h.get("w", 0), h.get("h", 0)
        base = (h["src"] + cell * stride * lh) & 0x3FFFFF         # 22-Bit-Adressen (SETFONT2 setzt Quelle "vor" das 1. Zeichen)
        raw = np.frombuffer(bytes(self.mem[base:base + stride * lh]), np.uint8).reshape(lh, stride)
        if fmt == 1: pix = np.unpackbits(raw, axis=1).astype(np.float32)                         # L1
        elif fmt == 2: pix = np.stack([raw >> 4, raw & 15], 2).reshape(lh, stride * 2).astype(np.float32) / 15   # L4
        elif fmt == 17: pix = np.stack([raw >> 6, raw >> 4 & 3, raw >> 2 & 3, raw & 3], 2).reshape(lh, stride * 4).astype(np.float32) / 3
        elif fmt == 3: pix = raw.astype(np.float32) / 255                                       # L8
        elif fmt == 4:                                                                          # RGB332
            r = (raw >> 5) / 7.0; g = (raw >> 2 & 7) / 7.0; b = (raw & 3) / 3.0
            rgbp = np.stack([r, g, b], 2).astype(np.float32); pix = None
        else: return
        bw = min(bw or stride, W); bh = min(bh or lh, lh)
        rgb_full = np.zeros((H, W, 3), np.float32); al = np.zeros((H, W), np.float32); m = np.zeros((H, W), bool)
        for yy in range(bh):
            Y = y0 + yy
            if not 0 <= Y < H: continue
            xs = slice(max(0, x0), min(W, x0 + bw))
            if xs.start >= xs.stop: continue
            sx = slice(xs.start - x0, xs.stop - x0)
            if pix is not None:
                row = pix[yy, sx] if yy < pix.shape[0] else 0
                al[Y, xs] = row * a; rgb_full[Y, xs] = col; m[Y, xs] = True
            else:
                rgb_full[Y, xs] = rgbp[yy, sx] * col; al[Y, xs] = a; m[Y, xs] = True
        blend(m, rgb_full, al)


def save_png(img, path, scale=2):
    from PIL import Image
    im = Image.fromarray(img, "RGB")
    if scale != 1: im = im.resize((img.shape[1] * scale, img.shape[0] * scale), Image.NEAREST)
    im.save(path)
