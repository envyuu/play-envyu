/* Patch 023 – Waveform-Ansicht fuer Slicer-/Granular-Spuren, Polyend Play Rev A, Firmware 1.6.0
 *
 * Grafik der Firmware (siehe Vault Firmware/Display_und_UI.md): Grafikchip FT81x ("EVE") an SPI2, Panel 240×320 im
 * RGB-Modus, logisch 320×240. Die UI besteht aus Widgets im Widget-Manager (HAL+0xB8C, vtable 0x9AA1C, 50 Plaetze).
 * Jedes Widget schreibt beim Zeichnen (vt[0]) Koprozessor-Befehle; das Ergebnis (Display-Liste) wird ins RAM_G kopiert
 * (vt[2] = FUN_00008750) und beim Bildaufbau per CMD_APPEND eingehaengt (vt[1] = FUN_0000877C).
 *
 * Patch 023 haengt sich an GENAU EINE Stelle: 0x2FEC8 in FUN_0002FD54 (Step-Parameter-Seite, Klasse 0xA0888),
 * "bl FUN_0002F36C" (Seite mit zwei Parametern fuellen) → wv_view_hook. Danach:
 *   - Stehen Sample Start (10) oder Sample End (11) auf der Seite und ist die Spur des Steps im Menue "Engines" auf
 *     Slicer/Granular, verbirgt wv_view_hook die 6 Original-Widgets der Seite und zeigt ein EIGENES Widget (Objekt im
 *     eigenen RAM, in einen freien Manager-Platz eingetragen): Waveform, Slice-Grenzen, aktueller Slice, Gate bzw.
 *     Position/Grain-Fenster/Spray, oben die Texte der Formatter ("Slice 3/16", "Gate 75%").
 *   - Sonst bleibt alles Original (das eigene Widget wird verborgen).
 * Die Waveform (Min/Max je Spalte) rechnet wv_poll in der Hauptschleife (loop_hook → sl_poll) in kleinen Stuecken,
 * einmal je Sample, und laedt sie als 1-Bit-Bitmap (304×120) nach RAM_G 0xF0000 (frei, siehe Vault). Gelesen wird
 * das Sample aus dem SDRAM (nur lesen). Gezeichnet wird nur aus der Hauptschleife (Widget-Manager, UI-Ereignisse).
 *
 * Quelle des Samples: wie der Original-Formatter FmtSampleRange_format 0x2AD38: step->vt[6](step, 9, &idx) →
 * Sample-Liste App+0x2514 (+4 = 255 Eintraege zu 0x38 Byte: {Datenzeiger-Vektor begin/end, cap, Frames, Rate(float)}).
 *
 * RAM: wv_t bei 0x1FFF9B00 (bis 0x1FFF9FFF frei hinter Patch 021).
 */
#include <stdint.h>
#include "slicer.h"
#include "wave.h"
#include "slview.h"
int sv_steps_held_now(void);              /* slview.c (17.11.) */

#ifdef HOST_TEST
extern uint8_t host_wv_ram[];
#define WV ((volatile wv_t *)(void *)host_wv_ram)
#else
#define WV ((volatile wv_t *)0x1FFF9B00u)
#define SLR ((volatile sl_t *)0x1FFF9800u)

/* EVE-Grundfunktionen der Firmware (Befehle in den RAM_CMD-Ring, siehe Vault) */
#define F(a, T) ((T)(a))
#define CO_BEGIN()      F(0x00000809u, void (*)(void))()               /* CS low, Schreibadresse RAM_CMD+wp */
#define CO_END()        F(0x00000829u, void (*)(void))()               /* CS high, REG_CMD_WRITE = wp */
#define CMD_DLSTART()   F(0x00000DD5u, void (*)(void))()
#define CMD_TEXT(x, y, f, o, s) F(0x00000C71u, void (*)(uint32_t, int32_t, uint32_t, int32_t, const char *))((x), (y), (f), (o), (s))
#define DL_COLOR(c)     F(0x00000971u, void (*)(uint32_t))(c)
#define DL_V2F(x, y)    F(0x00000991u, void (*)(uint32_t, uint32_t))((x), (y))
#define DL_BEGIN(p)     F(0x00000B9Du, void (*)(uint32_t))(p)
#define DL_END()        F(0x00000BBDu, void (*)(void))()
#define DL_SAVE()       F(0x00000BD9u, void (*)(void))()
#define DL_RESTORE()    F(0x00000BF5u, void (*)(void))()
#define DL_LINEW(w)     F(0x00000B25u, void (*)(uint32_t))(w)
#define DL_HANDLE(h)    F(0x000009FDu, void (*)(uint32_t))(h)
#define DL_SOURCE(a)    F(0x00000A1Du, void (*)(uint32_t))(a)
#define DL_LAYOUT(f, s, h) F(0x00000A3Du, void (*)(uint32_t, uint32_t, uint32_t))((f), (s), (h))
#define DL_SIZE(fi, wx, wy, w, h) F(0x00000A71u, void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))((fi), (wx), (wy), (w), (h))
#define DL_SCXY(x, y)   F(0x00000B45u, void (*)(uint32_t, uint32_t))((x), (y))
#define DL_SCSZ(w, h)   F(0x00000B71u, void (*)(uint32_t, uint32_t))((w), (h))
#define RAMG_WRITE(src, n, a) F(0x00000869u, void (*)(const void *, uint32_t, uint32_t))((src), (n), (a))
#define ORIG_SHOW2      ((void (*)(void *, void *, void *, void *, void *, void *, void *))0x0002F36Du)
#define APP_SAMPLES     0x1FFF69BCu                       /* App 0x1FFF44A8 + 0x2514 (FUN_0001A1C8) */
#define FONTTAB         0x0009A924u                       /* 5 Fonts × 0x1C: [0] Metrik im Flash, +8 Handle */
#define FIN_FN          0x00008751u                       /* Widget-vt[2]: REG_CMD_DL lesen, CMD_MEMCPY nach RAM_G */
#define APP_FN          0x0000877Du                       /* Widget-vt[1]: CMD_APPEND */
#define SDRAM_LO        0x70000000u
#define SDRAM_HI        0x72000000u
#define RAM_OK(p)       ((p) >= 0x1FFF0000u && (p) < 0x2003FF00u && ((p) & 3u) == 0u)
#endif

#define OPT_CENTER   0x600
#define OPT_RIGHTX   0x800
#define OPT_CENTERX  0x200     /* FT81x: nur waagrecht zentriert */
#define PRIM_BITMAPS 1
#define PRIM_LINES   3
#define PRIM_RECTS   9
#define BMP_HANDLE   6                                    /* frei: Firmware nutzt 0 (Bilder) und 8…12 (Fonts) */
#define RAMG_BMP     0x000F0000u                          /* frei: Fonts/Bilder bis 0x4ADA8, Widget-Listen 0x7A120…0xDE120 */
#define COLS_PER_POLL 16u
#define ROWS_PER_POLL 15u   /* 045: L4 = 4× Daten je Zeile → halbe Zeilenzahl je Durchlauf */
#define SAMPLES_PER_COL 64u
#define C_BG     0x111111u                                /* Farben aus der Theme-Tabelle 0xA42EC */
#define C_HL     0x303030u
#define C_SPRAY  0x1E1E1Eu
#define C_DIM    0x5C5C5Cu   /* 13.11.: ausserhalb des gespielten Bereichs dunkler (kein heller Kasten mehr) */
#define C_LINE   0x4A4A4Au
#define C_GRID   0x34507Au   /* 045: Slice-Grenzen schieferblau, abgesetzt von der grauen 4-Bit-Waveform */
#define C_TEXT   0xFFFFFFu
#define C_INFO   0x666666u
#define C_ACC    0xFF5540u
#define C_LATCH  0x00CC66u   /* 045: View eingerastet (Gruen der Farbtabelle; Gelb ist der Play-Marker) */
#define C_ENV    0x9A9A9Au
#define C_TINT   0x1C1C1Cu   /* 13.11.: Grundton (in der Huellkurve bzw. im gespielten Bereich), dezent */
#define C_PLAY   0xFFD64Du                                /* Akzent Gelb der Theme-Tabelle */
#define C_DRONE  0xC81EFFu                                /* 044: Drone an: wie das Pad */
#define C_DRONE0 0x2E2438u                                /* 044: Drone aus: ganz blass */
static int32_t step_param_ext(uint32_t idx);
static int16_t step_param(uint32_t step, uint32_t idx, int16_t dflt);

/* ------------------------------------------------------------------ reine Rechnung (Host-Test) */
void wv_column_range(uint32_t frames, uint32_t c, uint32_t *s0, uint32_t *s1) {
    uint32_t q = frames / WV_W, r = frames % WV_W;
    *s0 = c * q + (c * r) / WV_W;
    *s1 = (c + 1u) * q + ((c + 1u) * r) / WV_W;
    if (*s1 <= *s0) *s1 = *s0 + 1u;
    if (*s1 > frames) *s1 = frames;
}

void wv_column(const int16_t *d, uint32_t frames, uint32_t c, int8_t *mn, int8_t *mx) {
    uint32_t s0, s1;
    wv_column_range(frames, c, &s0, &s1);
    int32_t lo = 32767, hi = -32768;
    uint32_t n = s1 > s0 ? s1 - s0 : 0u, st = n / SAMPLES_PER_COL + 1u;
    for (uint32_t i = s0; i < s1; i += st) { int32_t v = d[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (s1 > s0 + 1u) { int32_t v = d[s1 - 1u]; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (!n) lo = hi = 0;
    *mn = (int8_t)(lo >> 8); *mx = (int8_t)(hi >> 8);
}

/* 045: RMS einer Spalte (0…127, gleiche Skala wie Min/Max >> 8), aus denselben Stichproben wie wv_column */
uint8_t wv_column_rms(const int16_t *d, uint32_t frames, uint32_t c) {
    uint32_t s0, s1;
    wv_column_range(frames, c, &s0, &s1);
    uint32_t n = s1 > s0 ? s1 - s0 : 0u, st = n / SAMPLES_PER_COL + 1u, cnt = 0, sum = 0;
    for (uint32_t i = s0; i < s1; i += st) { int32_t v = d[i] >> 8; sum += (uint32_t)(v * v); cnt++; }
    if (!cnt) return 0;
    uint32_t m = sum / cnt, r = 0;                          /* ganzzahlige Wurzel, m <= 16384 */
    for (uint32_t b = 1u << 7; b; b >>= 1) if ((r + b) * (r + b) <= m) r += b;
    return (uint8_t)(r > 127u ? 127u : r);
}

/* 045: Zeile y (0 = oben) der 4-Bit-Bitmap (L4: hohes Nibble = linke Spalte). Koerper (RMS) voll, Spitzen (Min/Max) blass,
 * dazwischen ein Verlauf ueber alle Stufen (12 … 2). Ohne RMS (rms = 0) wie frueher: alles voll. */
#define LV_BODY 15u   /* Koerper (RMS) */
#define LV_RIM  12u   /* 045: Verlauf ausserhalb des RMS: am RMS-Rand 12 … */
#define LV_TIP  2u    /* … an der Spitze 2 (alle Stufen dazwischen) */
void wv_row(const volatile int8_t *mn, const volatile int8_t *mx, const volatile uint8_t *rms, uint32_t y, uint8_t *out) {
    const int32_t half = (int32_t)(WV_H / 2u), amp = half - 1, yy = (int32_t)y;
    for (uint32_t b = 0; b < WV_STRIDE; b++) out[b] = 0;
    for (uint32_t c = 0; c < WV_W; c++) {
        int32_t top = half - 1 - (mx[c] * amp) / 128, bot = half - (mn[c] * amp) / 128;   /* mn<0 → unten */
        if (top > bot) { int32_t t = top; top = bot; bot = t; }
        uint32_t lv = 0;
        if (yy >= top && yy <= bot) {
            if (!rms) lv = LV_BODY;
            else {
                int32_t r = (rms[c] * amp) / 128, rt = half - 1 - r, rb = half + r;
                if (yy >= rt && yy <= rb) lv = LV_BODY;
                else {                                   /* 045: Verlauf vom RMS-Rand (12) bis zur Spitze (2), alle Stufen dazwischen */
                    int32_t span = yy < rt ? rt - top : bot - rb, d = yy < rt ? yy - top : bot - yy;
                    lv = span > 0 ? (uint32_t)(LV_TIP + (d * (LV_RIM - LV_TIP + 1)) / (span + 1)) : LV_TIP;
                    if (lv > LV_RIM) lv = LV_RIM;
                }
            }
        }
        if (lv) out[c >> 1] |= (uint8_t)((c & 1u) ? lv : lv << 4);
    }
}

/* 045: Cache-Schluessel = Adresse + Stichproben des Inhalts. Ein neues Sample an derselben Adresse mit derselben Laenge
 * (Sample-Pool nach Laden/Loeschen) lieferte sonst die alte Waveform. 8 Lesezugriffe, nur beim Seitenaufbau. */
static uint32_t wv_key(volatile wv_t *w) {
    uint32_t k = w->data;
    if (!w->data || w->frames < 16u) return k;
    const volatile int16_t *d = (const volatile int16_t *)(uintptr_t)w->data;
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t x = (uint16_t)d[w->frames / 8u * i + w->frames / 16u];
        k ^= i ? (x << (i * 4u)) | (x >> (32u - i * 4u)) : x;
    }
    return k;
}

/* x-Pixel (relativ) fuer 0…0x7FFF */
uint32_t wv_x(int32_t u) {
    if (u < 0) u = 0;
    if (u > 0x7FFF) u = 0x7FFF;
    return (uint32_t)u * WV_W / 0x7FFFu;
}

/* float-Bitmuster (positiv) → ganze Zahl, ohne FPU/Bibliothek */
uint32_t wv_f2u(uint32_t b) {
    if (b >> 31) return 0;
    int32_t e = (int32_t)((b >> 23) & 0xFFu) - 150;
    uint32_t m = (b & 0x7FFFFFu) | 0x800000u;
    if (((b >> 23) & 0xFFu) == 0) return 0;
    if (e >= 8) return 0xFFFFFFFFu;
    return e >= 0 ? m << e : (e > -32 ? m >> (uint32_t)(-e) : 0u);
}

/* Markierungen aus den Parametern (alles in Pixeln relativ zur Box) */
void wv_layout(volatile wv_t *w) {
    w->hl0 = w->hl1 = w->br0 = w->br1 = w->mk = w->sp0 = w->sp1 = 0xFFFFu;
    uint32_t n = sl_slices(w->mode);
    if (n) {
        int32_t st, en, s2, e2;
        uint32_t k = sl_slice_index(w->v10, n);
        sl_bounds(w->track, k, n, 0x7FFF, &st, &en);
        sl_bounds(w->track, k, n, w->v11, &s2, &e2);
        if (sl_fine(w->v10)) { s2 += sl_fine(w->v10) * (en - st) / 64; if (s2 < 0) s2 = 0; if (s2 >= e2) s2 = e2 - 1; }   /* 045 */
        w->hl0 = (uint16_t)wv_x(st);
        w->hl1 = (uint16_t)(wv_x(en) ? wv_x(en) - 1u : 0u);
        w->br0 = (uint16_t)wv_x(s2);                      /* 045: = Slice-Anfang ohne Feinversatz */
        w->br1 = (uint16_t)(wv_x(e2) > w->br0 ? wv_x(e2) - 1u : w->br0);
        w->mk = (uint16_t)wv_x(e2);
        w->kidx = (uint8_t)k; w->nsl = (uint8_t)n;
    } else if (sl_is_granular(w->mode) && w->frames) {
        gr_par_t p;
        p.len = w->frames; p.inc = 1u << 16;
        gr_params(&p, w->v10, w->v11, (int32_t)gr_cfg(w->track)->win * 100, (int32_t)gr_cfg(w->track)->spray * 100, w->mode - SL_GRAN);   /* 040: v13 ist die Fokus-Anzeige */
        w->v14 = p.fade * 100u / (p.glen ? p.glen : 1u);
        uint32_t cx = wv_x(w->v10);
        uint32_t gw = p.glen * WV_W / w->frames;          /* glen <= 44100, spray <= 22050: kein Ueberlauf */
        uint32_t sw = p.spray * WV_W / w->frames;
        if (gw < 2u) gw = 2u;
        uint32_t a = cx, b = cx + gw;                     /* Grain liest ab der Position glen Frames (rueckwaerts dieselben) */
        if (b > WV_W - 1u) b = WV_W - 1u;
        w->hl0 = (uint16_t)a; w->hl1 = (uint16_t)b; w->br0 = w->hl0; w->br1 = w->hl1;
        w->mk = (uint16_t)cx;
        if (sw) {
            w->sp0 = (uint16_t)(cx > sw ? cx - sw : 0u);
            uint32_t e = b + sw; w->sp1 = (uint16_t)(e > WV_W - 1u ? WV_W - 1u : e);
        }
        w->nsl = 0;
    } else if (w->mode == 0) {                           /* 045: normale Spur: gespielter Bereich Start…Ende hell, Marker am Ende */
        int32_t a = w->v10 < 0 ? 0 : w->v10, b = w->v11 > 0x7FFF ? 0x7FFF : w->v11;
        if (b < a) b = a;
        w->hl0 = (uint16_t)wv_x(a);
        w->hl1 = (uint16_t)(wv_x(b) > w->hl0 ? wv_x(b) - 1u : w->hl0);
        w->br0 = w->hl0; w->br1 = w->hl1;
        w->mk = (uint16_t)wv_x(b);
        w->nsl = 0;
    }
}

#ifndef HOST_TEST
/* ------------------------------------------------------------------ Geraet */
static uint32_t mgr_call(uint32_t mgr, uint32_t off, uint32_t a) {
    const uint32_t *vt = (const uint32_t *)(*(const uint32_t *)mgr);
    return ((uint32_t (*)(uint32_t, uint32_t))vt[off / 4u])(mgr, a);
}
static uint32_t slot_ptr(uint32_t mgr, uint32_t k) {
    uint32_t arr = *(const volatile uint32_t *)(mgr + 8u);
    if (!RAM_OK(arr) || k >= 50u) return 0;
    return ((const volatile uint32_t *)arr)[k];
}
static int vis(uint32_t mgr, uint32_t k) {
    uint32_t o = slot_ptr(mgr, k);
    return RAM_OK(o) && (((const volatile uint32_t *)o)[3] & 1u);
}

static uint32_t text_w(uint32_t fidx, const volatile char *s) {
    const uint8_t *wt = (const uint8_t *)(((const uint32_t *)FONTTAB)[fidx * 7u]);
    uint32_t n = 0;
    while (*s) { uint8_t c = (uint8_t)*s++; if (c < 128u) n += wt[c]; }
    return n;
}
static uint32_t font_h(uint32_t fidx) { return ((const uint16_t *)(FONTTAB + 0x1Cu * fidx))[4]; }

/* vt[0]: Display-Liste schreiben (Aufruf aus dem Widget-Manager, Hauptschleife) */
/* Huellkurve aus dem gehaltenen Step bzw. der Vorlage (Standard-Step): dieselben Werte nutzt das Vorhoeren */
static uint32_t env_val(uint32_t idx) {
    volatile wv_t *w_ = WV;
    /* 045: Step-Seite (view != 1) → Werte des angezeigten Steps; View → Vorlage bzw. gehaltener Step wie bisher */
    int32_t v = (w_->view != 1u && w_->rec_step) ? step_param(w_->rec_step, idx, 0) : step_param_ext(idx);   /* step_param prueft die Adresse (Steps nur 2-Byte-ausgerichtet) */
    return v < 0 ? 0u : v > 10000 ? 10000u : (uint32_t)v;
}
static uint32_t env_a(volatile wv_t *w) { (void)w; return env_val(13u); }
static uint32_t env_d(volatile wv_t *w) { (void)w; return env_val(14u); }
/* 13.11.: Huellkurve je Bereich (normale Spur: Start…Ende, Slicer: jeder Slice): Pixel x0 < xa <= xd < x1 */
typedef struct { uint16_t x0, xa, xd, x1; } wv_env_t;
static uint32_t env_list(volatile wv_t *w, wv_env_t *e, uint32_t max) {
    uint32_t nenv = w->nsl ? w->nsl : (w->mode == 0 ? 1u : 0u), n = 0;
    uint32_t ea = env_a(w), ed = env_d(w), Wd = WV_W;
    if (!nenv || !(ea || ed)) return 0;
    for (uint32_t k = 0; k < nenv && n < max; k++) {
        int32_t a, b;
        if (w->nsl) sl_bounds(w->track, k, w->nsl, w->v11, &a, &b);   /* 045: Huelle ueber den gespielten (gegateten) Teil */
        else { a = w->v10; b = w->v11 > w->v10 ? w->v11 : w->v10; }
        if (a >= 0x7FFE) break;
        uint32_t x0 = ((uint32_t)a * Wd + 0x3FFFu) / 0x7FFFu, x1 = ((uint32_t)b * Wd + 0x3FFFu) / 0x7FFFu;
        if (x1 >= Wd) x1 = Wd - 1u;
        if (x1 <= x0 + 1u) continue;
        uint32_t dx = x1 - x0, xa = x0 + dx * ea / 10000u, xd = x1 - dx * ed / 10000u;
        if (ed && ea && xd < xa) xa = xd = x0 + dx * ea / (ea + ed);
        if (w->nsl) {                                         /* 13.11.: Slicer – Huelle nur am gewaehlten Slice (gilt fuer alle gleich) */
            uint32_t c = w->br0 != 0xFFFFu ? ((uint32_t)w->br0 + w->br1) / 2u : 0xFFFFu;
            if (c == 0xFFFFu ? k != 0u : (c < x0 || c > x1)) continue;
            if (w->br0 != 0xFFFFu) {                          /* 14.11.: vom tatsaechlichen Start (Slice + Feinversatz) bis zum Gate */
                x0 = w->br0; x1 = (uint32_t)w->br1 + 1u > Wd - 1u ? Wd - 1u : (uint32_t)w->br1 + 1u;
                if (x1 <= x0 + 1u) continue;
                dx = x1 - x0; xa = x0 + dx * ea / 10000u; xd = x1 - dx * ed / 10000u;
                if (ed && ea && xd < xa) xa = xd = x0 + dx * ea / (ea + ed);
            }
        }
        e[n].x0 = (uint16_t)x0; e[n].xa = (uint16_t)xa; e[n].xd = (uint16_t)xd; e[n].x1 = (uint16_t)x1; n++;
    }
    return n;
}
static uint32_t wv_draw(void *self) {
    (void)self;
    volatile wv_t *w = WV;
    const uint32_t X = WV_X, Y = WV_Y, Wd = WV_W, Hd = WV_H;
    wv_env_t env[2];                                      /* 13.11.: Huelle nur an einem Bereich → wenig Stack */
    uint32_t nenv = env_list(w, env, 2u);
    CO_BEGIN(); CMD_DLSTART();
    DL_SAVE();
    if (w->sp0 != 0xFFFFu) { DL_LINEW(16); DL_BEGIN(PRIM_RECTS); DL_COLOR(C_SPRAY); DL_V2F(X + w->sp0, Y); DL_V2F(X + w->sp1, Y + Hd - 1u); DL_END(); }
    /* 13.11.: kein heller Kasten. Grundton nur INNERHALB der Huellkurve (unten verankert: Anstieg von unten links, oben gerade,
     * Abfall nach unten rechts) – zeigt "das wird gespielt"; ohne Huelle der ganze gespielte Bereich. Rechtecke statt Bitmap-Streifen
     * (billig fuer den FT81x: vorher 76 Bitmap-Aufrufe je Bild → Ruckeln beim Drehen). */
    {
        DL_LINEW(16); DL_BEGIN(PRIM_RECTS); DL_COLOR(C_TINT);
        uint32_t yb = Y + Hd - 1u;
        if (nenv) {
            for (uint32_t k = 0; k < nenv; k++) {
                uint32_t x0 = env[k].x0, xa = env[k].xa, xd = env[k].xd, x1 = env[k].x1;
                if (xd > xa) { DL_V2F(X + xa, Y); DL_V2F(X + xd, yb); }
                for (uint32_t r = 0; r < 2u; r++) {              /* Treppe fuer Anstieg (r 0) und Abfall (r 1), hoechstens 16 Stufen */
                    uint32_t a = r ? xd : x0, b = r ? x1 : xa;
                    if (b <= a) continue;
                    uint32_t wv = b - a, st = wv / 16u + 1u;
                    for (uint32_t x = a; x < b; x += st) {
                        uint32_t xe = x + st > b ? b : x + st;
                        uint32_t xm = (x + xe) / 2u - a;
                        uint32_t h = r ? (Hd * (wv - xm)) / wv : (Hd * xm) / wv;
                        if (h) { DL_V2F(X + x, Y + Hd - h); DL_V2F(X + xe, yb); }
                    }
                }
            }
        } else if (w->br0 != 0xFFFFu) { DL_V2F(X + w->br0, Y); DL_V2F(X + w->br1, yb); }
        DL_END();
    }
    if (w->upl) {
        DL_HANDLE(BMP_HANDLE); DL_SOURCE(RAMG_BMP); DL_LAYOUT(2u, WV_STRIDE, Hd); DL_SIZE(0, 0, 0, Wd, Hd);
        DL_COLOR(C_DIM); DL_BEGIN(PRIM_BITMAPS); DL_V2F(X, Y); DL_END();
        if (w->br0 != 0xFFFFu) {
            DL_SCXY(X + w->br0, Y); DL_SCSZ((uint32_t)(w->br1 - w->br0) + 1u, Hd);
            DL_COLOR(C_TEXT); DL_BEGIN(PRIM_BITMAPS); DL_V2F(X, Y); DL_END();
        }
    }
    DL_RESTORE();
    if (w->br0 != 0xFFFFu && !w->nsl) {                      /* duenne Linien an Start und Ende (normale Spur) */
        DL_COLOR(C_LINE); DL_LINEW(12); DL_BEGIN(PRIM_LINES);
        DL_V2F(X + w->br0, Y); DL_V2F(X + w->br0, Y + Hd - 1u); DL_V2F(X + w->br1, Y); DL_V2F(X + w->br1, Y + Hd - 1u);
        DL_END();
    }
    if (nenv) {                                               /* Umriss der Huellkurve, unten verankert */
        DL_COLOR(C_ENV); DL_LINEW(12); DL_BEGIN(PRIM_LINES);
        uint32_t yt = Y + 1u, yb = Y + Hd - 2u;
        for (uint32_t k = 0; k < nenv; k++) {
            uint32_t x0 = env[k].x0, xa = env[k].xa, xd = env[k].xd, x1 = env[k].x1;
            if (xa > x0) { DL_V2F(X + x0, yb); DL_V2F(X + xa, yt); }
            if (xd > xa) { DL_V2F(X + xa, yt); DL_V2F(X + xd, yt); }
            if (x1 > xd) { DL_V2F(X + xd, yt); DL_V2F(X + x1, yb); }
        }
        DL_END();
    }
    if (w->nsl > 1u) {
        DL_COLOR(C_GRID); DL_LINEW(8); DL_BEGIN(PRIM_LINES);   /* 13.11.: feinere Slice-Grenzen (halbe Breite, Nutzerwunsch) */
        for (uint32_t k = 0; k < w->nsl; k++) {               /* 031: Linien aus den echten Grenzen (Grid, Breite, Versatz, Beat) */
            int32_t a, b;
            sl_bounds(w->track, k, w->nsl, 0x7FFF, &a, &b);
            if (a >= 0x7FFE) break;                           /* hinter dem Sample-Ende */
            uint32_t px = ((uint32_t)a * Wd + 0x3FFFu) / 0x7FFFu;   /* gerundet: Grenzen fallen auf k·304/N */
            if (a > 0 && px > 0u && px < Wd) { DL_V2F(X + px, Y); DL_V2F(X + px, Y + Hd - 1u); }
            uint32_t pe = ((uint32_t)b * Wd + 0x3FFFu) / 0x7FFFu;
            if (k + 1u == w->nsl && b < 0x7FFF && pe < Wd) { DL_V2F(X + pe, Y); DL_V2F(X + pe, Y + Hd - 1u); }
        }
        DL_END();
    }
    if ((w->v13 == 3 || w->v13 == 4) && sv_focus_knob() == 11u && sv_beat_thr(w->track)) {   /* 036: Schwelle der Sensitivity */
        int32_t half = (int32_t)(Hd / 2u), amp = half - 1, off_ = ((int32_t)(sv_beat_thr(w->track) >> 8) * amp) / 128;
        if (off_ > amp) off_ = amp;
        DL_COLOR(C_ACC); DL_LINEW(12); DL_BEGIN(PRIM_LINES);
        DL_V2F(X, Y + (uint32_t)(half - 1 - off_)); DL_V2F(X + Wd - 1u, Y + (uint32_t)(half - 1 - off_));
        DL_V2F(X, Y + (uint32_t)(half + off_)); DL_V2F(X + Wd - 1u, Y + (uint32_t)(half + off_));
        DL_END();
    }
    if (w->mk != 0xFFFFu) {
        DL_COLOR(C_ACC); DL_LINEW(32); DL_BEGIN(PRIM_LINES);
        uint32_t x = X + (w->mk > Wd - 1u ? Wd - 1u : w->mk);
        DL_V2F(x, Y); DL_V2F(x, Y + Hd - 1u); DL_END();
    }
    {                                                         /* 038: Play-Marker */
        uint32_t pp = sv_play_state()[0];
        if (!w->nsl && pp && w->track < 8u && sl_is_granular(SLR->mode[w->track]) && gr_state(w->track)) {   /* 039: jeder Grain eine Linie */
            gr_t *g = gr_state(w->track);
            uint32_t node = SLR->node[w->track];
            uint32_t len = RAM_OK(node) ? *(const volatile uint32_t *)(node + 0xD8u + 0x24u) : 0u;
            if (len >= 2u) {
                uint32_t sh = 0;
                while ((len >> sh) >= (1u << 22)) sh++;
                DL_COLOR(C_PLAY); DL_LINEW(16); DL_BEGIN(PRIM_LINES);
                for (uint32_t j = 0; j < GR_MAXG; j++) {
                    if (!g->act[j] || g->ipos[j] < 0 || (uint32_t)g->ipos[j] >= len) continue;
                    uint32_t xg = X + (((uint32_t)g->ipos[j] >> sh) * Wd) / (len >> sh);
                    DL_V2F(xg, Y); DL_V2F(xg, Y + Hd - 1u);
                }
                DL_END();
            }
        }
        if (!w->nsl && w->hl0 != 0xFFFFu && w->hl1 > w->hl0 && w->track < 8u && sl_is_granular(SLR->mode[w->track])) {   /* 040: Grain-Fenster (13.11.: nur Granular, zeichnete auf normalen Spuren einen Rahmen) */
            uint32_t x0 = w->hl0, x1 = w->hl1, fw = (x1 - x0) * w->v14 / 100u, yb = Y + Hd - 2u, yt = Y + 1u;
            DL_COLOR(C_ENV); DL_LINEW(12); DL_BEGIN(PRIM_LINES);
            DL_V2F(X + x0, yb); DL_V2F(X + x0 + fw, yt);
            DL_V2F(X + x0 + fw, yt); DL_V2F(X + x1 - fw, yt);
            DL_V2F(X + x1 - fw, yt); DL_V2F(X + x1, yb);
            DL_END();
        }
        if (w->nsl && pp) {
            uint32_t xp = X + pp - 1u;
            DL_COLOR(C_PLAY); DL_LINEW(24); DL_BEGIN(PRIM_LINES);
            DL_V2F(xp, Y); DL_V2F(xp, Y + Hd - 1u); DL_END();
        }
    }
    if (w->view == 1u && !w->nsl && w->track < 8u && sl_is_granular(SLR->mode[w->track])) {   /* 044: Schriftzug */
        DL_COLOR(gr_cfg(w->track)->drone ? C_DRONE : C_DRONE0);
        CMD_TEXT(X + Wd - 4u, Y + 2u, font_h(0), OPT_RIGHTX, "DRONE");
    }
    /* 033: Fokus w->v13 (nach wv_layout): 0 keiner, 1/2 Step-Ansicht oben/unten, 3/4 Slicer-Seite oben/unten. Die zwei Tasten fuer den
     * Fokus liegen links neben dem Display uebereinander, deshalb steht der obere Parameter ueber, der untere unter der Waveform. */
    uint32_t fix4 = (w->mode == 0 && !w->nsl && w->v13 >= 11 && w->v13 <= 14) ? (uint32_t)w->v13 - 10u : 0u;   /* 045 */
    uint32_t foc = fix4 ? 0u : w->v13 > 0 && w->v13 <= 4 ? (uint32_t)w->v13 : 0u;
    uint32_t f0 = font_h(0);
    const volatile char *tl, *tr, *bl, *br;                    /* oben links/rechts, unten links/rechts */
    if (foc == 3u || foc == 4u) { tl = w->i0; tr = w->t1; bl = w->i1; br = w->t0; }
    else if (foc == 1u || foc == 2u) { tl = w->t0; tr = w->i1; bl = w->t1; br = w->i0; }
    else { tl = w->t0; tr = w->t1; bl = w->i0; br = w->i1; }
    /* Ohne Fokus oben gross, wenn es passt (Font 1 = 22×33); mit Fokus beide Parameter gleich gross (Font 0), direkt an der Waveform */
    uint32_t fi = (!foc && !fix4 && text_w(1, tl) + text_w(1, tr) + 12u <= Wd) ? 1u : 0u;
    uint32_t fh = ((const uint16_t *)(FONTTAB + 0x1Cu * fi))[4];
    uint32_t yt = (foc || fix4) ? 40u : WV_TY;
    for (uint32_t i = 0; i < 4u; i++) {
        const volatile char *tx = i == 0u ? tl : i == 1u ? tr : i == 2u ? bl : br;
        if (!tx[0]) continue;
        uint32_t upper = i < 2u, right = i & 1u;
        uint32_t fx = upper ? fi : 0u, y = upper ? yt : WV_IY, tw = text_w(fx, tx);
        uint32_t x = right ? X + Wd : X;
        uint32_t c = (upper && !foc) ? C_TEXT : C_INFO;
        uint32_t act = (foc == 1u || foc == 3u) ? 0u : (foc == 2u || foc == 4u) ? 2u : 9u;   /* aktives Feld */
        if (fix4) {                                       /* 045: feste Felder TL/TR/BL/BR, aktives mit Kasten (auch rechts) */
            uint32_t a4 = fix4 == 1u ? 0u : fix4 == 2u ? 1u : fix4 == 3u ? 2u : 3u;
            c = C_TEXT;
            if (i == a4) {
                uint32_t x0 = right ? X + Wd - tw - 2u : (X > 3u ? X - 3u : 0u), x1 = right ? X + Wd + 3u : X + tw + 2u;
                DL_COLOR(C_ACC); DL_LINEW(16); DL_BEGIN(PRIM_RECTS); DL_V2F(x0, y); DL_V2F(x1, y + 21u); DL_END();
                c = 0x000000u;
            }
            DL_COLOR(c);
            CMD_TEXT(x, y, f0, right ? OPT_RIGHTX : 0, (const char *)tx);
            continue;
        }
        if (foc && i == act) {
            DL_COLOR(C_ACC); DL_LINEW(16); DL_BEGIN(PRIM_RECTS);
            DL_V2F(X > 3u ? X - 3u : 0u, y); DL_V2F(X + tw + 2u, y + 21u); DL_END();
            c = 0x000000u;
        } else if (foc && !right && (i == 0u || i == 2u)) c = C_TEXT;   /* der andere Wert desselben Knobs: lesbar, nicht markiert */
        DL_COLOR(c);
        CMD_TEXT(x, y, upper ? fh : f0, right ? OPT_RIGHTX : 0, (const char *)tx);
    }
    if (w->nsl) {                                        /* 045: View-Rastung unten zwischen den Texten: grau = einrastbar, gruen = eingerastet */
        uint32_t wl = text_w(0, bl), wr = text_w(0, br), wv = text_w(0, "VIEW");
        uint32_t xm = X + Wd / 2u, h = wv / 2u + 8u;      /* fest in der Mitte (Nutzer: sprang mit dem rechten Text) */
        if (X + wl <= xm - h && X + Wd - wr >= xm + h) {  /* nur wenn es keinen Text beruehrt */
            DL_COLOR(w->view == 1u ? C_LATCH : C_LINE);
            CMD_TEXT(xm, WV_IY, f0, OPT_CENTERX, "VIEW");
        }
    }
    if (!w->frames) { DL_COLOR(C_TEXT); CMD_TEXT(X + Wd / 2u, Y + Hd / 2u, f0, OPT_CENTER, "No Sample"); }
    else if (!w->upl) { DL_COLOR(C_INFO); CMD_TEXT(X + Wd / 2u, Y + Hd / 2u, f0, OPT_CENTER, "..."); }
    CO_END();
    return 0;
}
static uint32_t wv_nop(void *self) { (void)self; return 0; }

/* vtable im eigenen RAM wie die Widget-Klassen 0x9AF58 … (10 Methoden): [0] zeichnen, [1] anhaengen (Original
 * 0x877C), [2] abschliessen (Original 0x8750), [3…8] Setter (vom Manager nur fuer eigene Plaetze gerufen → nie),
 * [9] Zerstoeren (Objekt liegt im eigenen RAM → nichts freigeben) */
static uint32_t vt_addr(void) { return (uint32_t)(uintptr_t)&WV->vt[0]; }

static void build_vt(void) {
    volatile wv_t *w = WV;
    for (uint32_t i = 0; i < 10u; i++) w->vt[i] = (uint32_t)(uintptr_t)wv_nop | 1u;
    w->vt[0] = (uint32_t)(uintptr_t)wv_draw | 1u;
    w->vt[1] = APP_FN;
    w->vt[2] = FIN_FN;
}

/* eigenes Widget in einen freien Platz (hoechster freier zuerst: wird zuerst angehaengt = ganz unten) */
static uint32_t ensure_slot(uint32_t mgr) {
    volatile wv_t *w = WV;
    uint32_t me = (uint32_t)(uintptr_t)&w->obj[0];
    if (w->slot < 50u && slot_ptr(mgr, w->slot) == me) return w->slot;
    uint32_t arr = *(const volatile uint32_t *)(mgr + 8u);
    if (!RAM_OK(arr)) return 0xFFFFFFFFu;
    for (uint32_t k = 50u; k-- > 0u;) {
        if (((volatile uint32_t *)arr)[k] == 0u) {
            for (uint32_t i = 0; i < 16u; i++) w->obj[i] = 0;
            build_vt();
            w->obj[0] = vt_addr();
            w->obj[13] = 12u;                            /* +0x34 Typ (eigener, ausserhalb 0…11) */
            ((volatile uint32_t *)arr)[k] = me;
            w->slot = k;
            return k;
        }
    }
    w->slot = 0xFFFFFFFFu;
    return 0xFFFFFFFFu;
}

static void hide_view(uint32_t mgr, uint32_t view) {
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t id = *(const volatile uint32_t *)(view + 0xDCu + 4u * i);
        if (id < 50u) mgr_call(mgr, 0x28u, id);          /* vt+0x28 (0x3968): Sichtbar-Bit loeschen */
    }
}
static void show_view(uint32_t mgr, uint32_t view) {    /* 045: Gegenstueck zu hide_view */
    if (!RAM_OK(view)) return;
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t id = *(const volatile uint32_t *)(view + 0xDCu + 4u * i);
        if (id < 50u && RAM_OK(slot_ptr(mgr, id))) mgr_call(mgr, 0x24u, id);
    }
}
static int view_any_visible(uint32_t mgr, uint32_t view) {
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t id = *(const volatile uint32_t *)(view + 0xDCu + 4u * i);
        if (id < 50u && vis(mgr, id)) return 1;
    }
    return 0;
}
static void redraw_me(uint32_t mgr) {
    volatile wv_t *w = WV;
    if (w->slot >= 50u) return;
    mgr_call(mgr, 0x08u, w->slot);                       /* vt+0x08 (0x3C28): in die Zeichen-Warteschlange */
    mgr_call(mgr, 0x0Cu, 0);                             /* vt+0x0C (0x3A48): Bild nach der Warteschlange */
}

/* 045: Plaetze, die die Slicer-Seite in View verborgen hat. Beim Verlassen von View wieder einblenden, sonst bleibt die Seite
 * der Firmware leer, bis ein Parameter gewaehlt wird (Nutzer 09.11.: View zweimal druecken → alles leer). */
typedef struct { uint32_t magic, lo, hi; } hid_t;
#define HID ((volatile hid_t *)0x1FFFB7C0u)
_Static_assert(0x1FFFB7C0u + sizeof(hid_t) <= 0x1FFFB900u, "hid_t vor NIS");
#define HID_MAGIC 0x41D00450u
static void hid_note(uint32_t k) {
    volatile hid_t *h = HID;
    if (h->magic != HID_MAGIC) { h->lo = 0; h->hi = 0; h->magic = HID_MAGIC; }
    if (k < 32u) h->lo |= 1u << k; else if (k < 64u) h->hi |= 1u << (k - 32u);
}
static void hid_restore(uint32_t mgr) {
    volatile hid_t *h = HID;
    if (h->magic != HID_MAGIC) return;
    for (uint32_t k = 0; k < 50u; k++) {
        uint32_t on = k < 32u ? (h->lo >> k) & 1u : (h->hi >> (k - 32u)) & 1u;
        if (on && RAM_OK(slot_ptr(mgr, k))) mgr_call(mgr, 0x24u, k);       /* vt+0x24: sichtbar */
    }
    h->lo = 0; h->hi = 0;
}

uint32_t sv_fw_screen(void) __attribute__((weak));   /* slview.c (13.11.): aktiver Bildschirm der Firmware */
void sv_fw_redraw(void) __attribute__((weak));      /* slview.c (045): aktiven Bildschirm der Firmware neu zeichnen */
static void off(uint32_t mgr) {
    volatile wv_t *w = WV;
    if (w->on && w->slot < 50u && slot_ptr(mgr, w->slot) == (uint32_t)(uintptr_t)&w->obj[0]) mgr_call(mgr, 0x28u, w->slot);
    w->on = 0;
}

static void copy_txt(volatile char *d, const volatile char *s, uint32_t n) {
    uint32_t i = 0;
    while (i + 1u < n && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static uint32_t put_u(volatile char *b, uint32_t k, uint32_t max, uint32_t v, uint32_t mind) {
    char t[10]; uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while ((v || n < mind) && n < 10u);
    while (n && k + 1u < max) b[k++] = t[--n];
    b[k] = 0;
    return k;
}
static uint32_t put_s(volatile char *b, uint32_t k, uint32_t max, const char *s) {
    while (*s && k + 1u < max) b[k++] = *s++;
    b[k] = 0;
    return k;
}

#define MEM_SD(p) ((p) >= SDRAM_LO && (p) < SDRAM_HI - 0x40u && ((p) & 3u) == 0u)

static int16_t step_param(uint32_t step, uint32_t idx, int16_t dflt) {
    /* Steps sind 42 Byte gross → nur 2-Byte-ausgerichtet (z. B. 0x200027DA) */
    if (step < 0x1FFF0000u || step >= 0x2003FF00u || (step & 1u)) return dflt;
    uint32_t vt = *(const volatile uint32_t *)step;
    if (vt < 0x9A000u || vt >= 0xAE000u) return dflt;    /* vtable im Flash */
    uint16_t v = 0;
    uint32_t ok = ((uint32_t (*)(uint32_t, uint32_t, uint16_t *))(((const uint32_t *)vt)[6]))(step, idx, &v);
    return ok ? (int16_t)v : dflt;
}

/* Sample des Steps wie im Original-Formatter: Parameter 9 → Sample-Liste */
static int32_t find_sample(uint32_t step, uint32_t t) {
    volatile wv_t *w = WV;
    int16_t si = step_param(step, 9u, 0);
    if (t < 8u && (sl_slices(SLR->mode[t]) || sl_is_granular(SLR->mode[t])) && sv_sample(t) >= 0) si = (int16_t)sv_sample(t);   /* 029/041: Sample der Spur */
    w->data = 0; w->frames = 0; w->rate = 44100u;
    uint32_t base = *(const volatile uint32_t *)(APP_SAMPLES + 4u);
    if (si < 0 || si >= 255 || !(RAM_OK(base) || MEM_SD(base))) return -1;   /* Liste liegt am Geraet im SDRAM (0x704D52B4) */
    const volatile uint32_t *rec = (const volatile uint32_t *)(base + 0x38u * (uint32_t)si);
    uint32_t b = rec[0], e = rec[1], fr = rec[3];
    if (b == e || !(RAM_OK(b) || MEM_SD(b)) || (b & 3u)) return -1;
    uint32_t d = *(const volatile uint32_t *)b;
    if (d < SDRAM_LO || (d & 1u) || fr < 2u || fr > (SDRAM_HI - d) / 2u) return -1;
    uint32_t r = wv_f2u(rec[4]);
    w->data = d; w->frames = fr; w->rate = (r >= 4000u && r <= 192000u) ? r : 44100u;
    return si;
}

/* 028: erster gesetzter Step der Spur t im aktiven Pattern (wie FUN_0002c51c: Auswahl+0x5C → vt[0](spur) → vt[0](step),
 * Step an = vt[3]); keiner → Standard-Step (Auswahl+0x7C, wird erst beim Beruehren eines Steps mit dessen Werten gefuellt). */
static uint32_t track_step(uint32_t t) {
    uint32_t dflt = *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu);
    uint32_t pm = *(const volatile uint32_t *)(0x1FFF450Cu + 0x5Cu);
    if (!RAM_OK(pm)) return dflt;
    uint32_t vt = *(const volatile uint32_t *)pm;
    if (vt < 0x9A000u || vt >= 0xAE000u) return dflt;
    uint32_t tr = ((uint32_t (*)(uint32_t, uint32_t))(((const uint32_t *)vt)[0]))(pm, t);
    if (!RAM_OK(tr) || (tr & 1u)) return dflt;
    uint32_t tvt = *(const volatile uint32_t *)tr;
    if (tvt < 0x9A000u || tvt >= 0xAE000u) return dflt;
    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t sp = ((uint32_t (*)(uint32_t, uint32_t))(((const uint32_t *)tvt)[0]))(tr, i);
        if (sp < 0x1FFF0000u || sp >= 0x2003FF00u || (sp & 1u)) continue;
        uint32_t svt = *(const volatile uint32_t *)sp;
        if (svt < 0x9A000u || svt >= 0xAE000u) continue;
        if (((uint32_t (*)(uint32_t))(((const uint32_t *)svt)[3]))(sp) & 0xFFu) return sp;
    }
    return dflt;
}

static void make_info(void) {
    volatile wv_t *w = WV;
    uint32_t k = put_s(w->i0, 0, sizeof(w->i0), sl_mode_name(w->mode));
    (void)k;
    w->i1[0] = 0;
    if (!w->frames) return;
    uint32_t r100 = w->rate / 100u;                       /* Frames <= 2^24: alles 32 Bit */
    uint32_t cs = w->frames / r100;                       /* Hundertstel Sekunden */
    if (w->nsl) {                                        /* Slice-Laenge in ms */
        uint32_t ms = (w->frames / w->nsl) * 10u / r100;
        k = put_u(w->i1, 0, sizeof(w->i1), ms, 1);
        k = put_s(w->i1, k, sizeof(w->i1), " ms / ");
    } else k = 0;
    k = put_u(w->i1, k, sizeof(w->i1), cs / 100u, 1);
    k = put_s(w->i1, k, sizeof(w->i1), ".");
    k = put_u(w->i1, k, sizeof(w->i1), cs % 100u, 2);
    put_s(w->i1, k, sizeof(w->i1), " s");
}

/* 045: normale Spur, vier feste Felder wie die Knobs: links Start/End (Knob Sample Start), rechts Attack/Decay (Knob Attack).
 * t0 oben links, t1 oben rechts, i0 unten links, i1 unten rechts; v13 = 10 + aktives Feld (1 TL, 2 TR, 3 BL, 4 BR). */
static uint32_t put_ms(volatile char *b, uint32_t k, uint32_t max, int32_t v) {
    volatile wv_t *w = WV;
    uint32_t tot = (w->rate >= 1000u) ? w->frames / (w->rate / 1000u) : 0u;   /* Gesamtlaenge in ms */
    uint32_t u = v < 0 ? 0u : v > 0x7FFF ? 0x7FFFu : (uint32_t)v;
    k = put_u(b, k, max, tot * (u >> 3) / 4096u, 1);                           /* tot <= 4·10^5, (u>>3) < 4096: kein Ueberlauf */
    return put_s(b, k, max, " ms");
}
static uint32_t put_pc(volatile char *b, uint32_t k, uint32_t max, uint32_t v) {   /* 0…10000 → "20.0%" */
    k = put_u(b, k, max, v / 100u, 1);
    k = put_s(b, k, max, ".");
    k = put_u(b, k, max, v % 100u / 10u, 1);
    return put_s(b, k, max, "%");
}
/* Bezeichnung voranstellen, wenn der Text mit Zahl/Vorzeichen/~ beginnt (Firmware-Werte ohne Namen) */
static void label_txt(volatile char *t, const char *lab) {
    char c = t[0];
    if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '~')) return;
    char tmp[24]; uint32_t k = 0, j = 0;
    while (lab[k] && k < 23u) { tmp[k] = lab[k]; k++; }
    while (t[j] && k < 23u) tmp[k++] = t[j++];
    tmp[k] = 0;
    for (j = 0; j <= k; j++) t[j] = tmp[j];
}
static void fixed4(volatile wv_t *w, int pg_ad, uint32_t fi_) {
    uint32_t k;
    if (w->rec_have & 0x10u) {                            /* 13.11.: Spur gewaehlt (relativ): Felder der Seite = Text der Firmware, andere "~" */
        char a_[24], b_[24];
        for (k = 0; k < 23u && w->t0[k]; k++) a_[k] = w->t0[k];
        a_[k] = 0;
        for (k = 0; k < 23u && w->t1[k]; k++) b_[k] = w->t1[k];
        b_[k] = 0;
        k = put_s(w->t0, 0, sizeof(w->t0), "Start "); put_s(w->t0, k, sizeof(w->t0), pg_ad ? "~" : a_);
        k = put_s(w->i0, 0, sizeof(w->i0), "End ");   put_s(w->i0, k, sizeof(w->i0), pg_ad ? "~" : b_);
        k = put_s(w->t1, 0, sizeof(w->t1), "Attack "); put_s(w->t1, k, sizeof(w->t1), pg_ad ? a_ : "~");
        k = put_s(w->i1, 0, sizeof(w->i1), "Decay ");  put_s(w->i1, k, sizeof(w->i1), pg_ad ? b_ : "~");
        w->v13 = 10 + (int32_t)(fi_ == 10u ? 1 : fi_ == 13u ? 2 : fi_ == 11u ? 3 : fi_ == 14u ? 4 : (pg_ad ? 2 : 1));
        return;
    }
    k = put_s(w->t0, 0, sizeof(w->t0), "Start "); put_ms(w->t0, k, sizeof(w->t0), w->v10);
    k = put_s(w->i0, 0, sizeof(w->i0), "End ");   put_ms(w->i0, k, sizeof(w->i0), w->v11);
    k = put_s(w->t1, 0, sizeof(w->t1), "Attack "); put_pc(w->t1, k, sizeof(w->t1), env_a(w));
    k = put_s(w->i1, 0, sizeof(w->i1), "Decay ");  put_pc(w->i1, k, sizeof(w->i1), env_d(w));
    w->v13 = 10 + (int32_t)(fi_ == 10u ? 1 : fi_ == 13u ? 2 : fi_ == 11u ? 3 : fi_ == 14u ? 4 : (pg_ad ? 2 : 1));
}

/* 045: Sample-/Ordner-Seite einer normalen Spur: t0 = Name (Seitentext), i0 = Ordner (Seitentext), t1 = Laenge, i1 = Position */
static void fixed4_sample(volatile wv_t *w, uint32_t fi_) {
    char nm_[24], fo_[24]; uint32_t k;
    for (k = 0; k < 23u && w->t0[k]; k++) nm_[k] = w->t0[k];
    nm_[k] = 0;
    for (k = 0; k < 23u && w->t1[k]; k++) fo_[k] = w->t1[k];
    fo_[k] = 0;
    copy_txt(w->t0, nm_, sizeof(w->t0));
    copy_txt(w->i0, fo_, sizeof(w->i0));
    uint32_t r100 = w->rate / 100u, cs = r100 ? w->frames / r100 : 0u;
    k = put_u(w->t1, 0, sizeof(w->t1), cs / 100u, 1); k = put_s(w->t1, k, sizeof(w->t1), ".");
    k = put_u(w->t1, k, sizeof(w->t1), cs % 100u, 2); put_s(w->t1, k, sizeof(w->t1), " s");
    uint8_t cls[255];
    wv_sample_classes(cls);
    int32_t cur = step_param(w->rec_step, 9u, -1);
    uint32_t n = 0, pos = 0;
    for (uint32_t i = 0; i < 255u; i++) if (cls[i]) { n++; if ((int32_t)i == cur) pos = n; }
    k = put_s(w->i1, 0, sizeof(w->i1), "Sample ");
    if (pos) { k = put_u(w->i1, k, sizeof(w->i1), pos, 1); k = put_s(w->i1, k, sizeof(w->i1), "/"); put_u(w->i1, k, sizeof(w->i1), n, 1); }
    else put_s(w->i1, k, sizeof(w->i1), "-");
    w->v13 = 10 + (fi_ == 15u ? 3 : 1);
}

/* ersetzt bei 0x2FEC8 "bl FUN_0002F36C" (FUN_0002FD54, Step-Parameter-Seite) */
static void wv_view_apply(void *view);
__attribute__((used, section(".text.entry_wvview")))
void wv_view_hook(void *view, void *names, void *vals, void *prog, void *max, void *flags, void *styles) {
    ORIG_SHOW2(view, names, vals, prog, max, flags, styles);
    wv_view_apply(view);
}
/* 13.11.: Ansicht auf eine (schon formatierte) Step-Seite legen – aus dem Hook und beim Verlassen von View */
static void wv_view_apply(void *view) {
    volatile wv_t *w = WV;
    if (w->magic != WV_MAGIC || SLR->magic != SL_MAGIC) return;
    uint32_t v = (uint32_t)(uintptr_t)view;
    if (!RAM_OK(v)) return;
    uint32_t mgr = *(const volatile uint32_t *)(v + 4u);
    if (!RAM_OK(mgr) || *(const volatile uint32_t *)mgr != 0x0009AA1Cu) return;
    w->seq++;
    /* 030: In View auf einer Slicer-Spur bleibt die Slicer-Seite stehen (wv_kb), sonst blockiert die Step-Ansicht die
     * Slice-Anzeige, nachdem ein Step beruehrt wurde. Die Werte gehen in die Fokuszeile. */
    if (sv_page_track() < 8u) {
        sv_capture(*(const volatile uint8_t *)(v + 0x104u), *(const volatile uint8_t *)(v + 0x105u),
                   (const volatile char *)(v + 0x74u), (const volatile char *)(v + 0x32u),
                   (const volatile char *)(v + 0x95u), (const volatile char *)(v + 0x53u),
                   *(const volatile uint32_t *)(v + 200u));
        w->rec_ok = 0;
        hide_view(mgr, v);
        redraw_me(mgr);
        return;
    }
    uint8_t i0 = *(const volatile uint8_t *)(v + 0x104u), i1 = *(const volatile uint8_t *)(v + 0x105u);
    int want = 0;
    uint32_t t = w->rec_t;
    int pg_se = i0 == 10u || i0 == 11u || i1 == 10u || i1 == 11u;
    int pg_ad = i0 == 13u || i0 == 14u || i1 == 13u || i1 == 14u;   /* 045: Attack/Decay */
    int pg_sf = i0 == 9u || i0 == 15u || i1 == 9u || i1 == 15u;     /* 045: Sample/Folder (nur normale Spuren) */
    if (!w->rec_ok && (pg_se || pg_ad || pg_sf) && !sv_multi_held()) {
        /* 13.11.: kein Formatter-Hinweis (View verlassen, leere Spur, Spur ohne Steps): gewaehlte Spur, sonst zuletzt benutzte;
         * Bezug = erster Step der Spur (gewaehlt, relativ) bzw. die Vorlage (nichts gewaehlt, so wie die Firmware-Seite) */
        uint32_t mk = *(const volatile uint32_t *)0x1FFF4518u & 0xFFFFu, tf = 8u;
        if (mk & 0xFFu) { tf = 0; while (!((mk >> tf) & 1u)) tf++; }   /* erste gewaehlte Audio-Spur */
        else if (!mk) tf = sv_cur_track();
        if (tf < 8u) {
            w->rec_ok = 1; w->rec_t = tf; t = tf;
            w->rec_step = mk ? track_step(tf) : *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu);
            w->rec_have = mk ? 0x10u : 0u;
        }
    }
    if (w->rec_ok && t < 8u && (pg_se || pg_ad || pg_sf) && !sv_multi_held()) {   /* 045: mehrere gehaltene Steps → keine */
        int32_t m = SLR->mode[t];
        if (m >= 0 && m < SL_NMODES && (!pg_sf || m == 0 || sl_slices(m))) want = 1;  /* 045: normale Spuren; 12.11.: Sample/Folder auch auf Slicer-Spuren ohne Anheften */
    }
    w->rec_ok = 0;
    if (!want) { if (w->on) off(mgr); return; }
    /* Werte: angezeigter Wert (Formatter) vor dem Step */
    uint32_t st = w->rec_step;
    w->mode = SLR->mode[t]; w->track = t;
    w->v10 = (w->rec_have & 1u) ? w->rec_v10 : step_param(st, 10u, 0);
    w->v11 = (w->rec_have & 2u) ? w->rec_v11 : step_param(st, 11u, 0x7FFF);
    w->v13 = (int32_t)gr_cfg(t)->win * 100;              /* 039: Window, Spray je Spur */
    w->v14 = (int32_t)gr_cfg(t)->spray * 100;
    find_sample(st, t);
    if (wv_key(w) != w->c_data || w->frames != w->c_frames) {
        w->c_data = wv_key(w); w->c_frames = w->frames; w->col = 0; w->row = 0; w->upl = 0;
    }
    /* Texte der Seite (nach FUN_0002F36C): Wert Block 0 bei +0x32, Block 1 bei +0x53 */
    uint32_t a0 = pg_ad ? 13u : pg_sf ? 9u : 10u, a1 = pg_ad ? 14u : pg_sf ? 15u : 11u;   /* 045: Texte der Seite */
    uint32_t o10 = i0 == a0 ? 0x32u : i1 == a0 ? 0x53u : 0u, o11 = i0 == a1 ? 0x32u : i1 == a1 ? 0x53u : 0u;
    if (o10) copy_txt(w->t0, (const volatile char *)(v + o10), sizeof(w->t0)); else w->t0[0] = 0;
    if (o11) copy_txt(w->t1, (const volatile char *)(v + o11), sizeof(w->t1)); else w->t1[0] = 0;
    if (w->mode != 0 && !pg_sf) {                         /* 13.11.: Slicer – Bezeichnung vor nackte Werte ("20.0%" → "Attack 20.0%") */
        label_txt(w->t0, pg_ad ? "Attack " : "Start ");
        label_txt(w->t1, pg_ad ? "Decay " : "Gate ");
    }
    wv_layout(w);
    make_info();
    /* 030: Fokus wie die Firmware (Seite+200 = gewaehlter Block, Block 0 = i0) */
    {
        uint32_t bsel = *(const volatile uint32_t *)(v + 200u);
        uint32_t fi_ = bsel ? i1 : i0;
        w->v13 = (fi_ == 10u || fi_ == 13u) ? 1 : (fi_ == 11u || fi_ == 14u) ? 2 : 0;
        if (pg_sf && (w->mode == 0 || sl_slices(w->mode))) fixed4_sample(w, fi_); /* 045: Sample suchen: Name/Ordner links, Laenge/Position rechts (12.11.: auch Slicer) */
        else if (w->mode == 0) fixed4(w, pg_ad, fi_);     /* 045: normale Spur: vier feste Felder */
    }
    if (ensure_slot(mgr) >= 50u) { w->on = 0; return; }
    w->view = v; w->mgr = mgr;
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t id = *(const volatile uint32_t *)(v + 0xDCu + 4u * i);
        w->vobj[i] = id < 50u ? slot_ptr(mgr, id) : 0u;
    }
    hide_view(mgr, v);
    mgr_call(mgr, 0x24u, w->slot);                       /* vt+0x24 (0x3950): sichtbar */
    w->on = 1;
    w->todo = 1;
    redraw_me(mgr);
}

/* 038: Play-Marker. Position der klingenden Stimme, hoechstens alle 20 ms neu zeichnen (das Display schafft rund 50 Bilder/s). */
#define PLAY_MS 20u
#ifdef HOST_TEST
extern uint32_t host_millis;
#define WV_NOW() host_millis
#else
#define WV_NOW() (*(const volatile uint32_t *)0x20000D7Cu)
#endif
static void play_tick(uint32_t mgr) {
    volatile wv_t *w = WV;
    volatile uint32_t *ps = sv_play_state();
    uint32_t px = 0u;
    uint32_t node = w->track < 8u ? SLR->node[w->track] : 0u;
    if (!w->nsl && w->track < 8u && sl_is_granular(SLR->mode[w->track]) && RAM_OK(node) && *(const volatile uint32_t *)node == 0x0009C59Cu) {
        const volatile uint8_t *st = (const volatile uint8_t *)(node + 0xD8u);
        gr_t *g = gr_state(w->track);
        uint32_t len = *(const volatile uint32_t *)(st + 0x24);
        if (g && st[0x18] && len >= 2u) {                     /* 039: laufend neu, solange die Wolke klingt */
            uint32_t now = WV_NOW();
            if (now - ps[1] < PLAY_MS) return;
            uint32_t sh = 0;
            while ((len >> sh) >= (1u << 22)) sh++;
            uint32_t cx = ((g->cshow >> sh) * WV_W) / (len >> sh);
            wv_layout(w);                                     /* 040: Breite frisch, sonst schrumpft sie am rechten Rand */
            uint32_t gw = (uint32_t)w->hl1 - (uint32_t)w->hl0;
            uint32_t sw = ((g->sshow >> sh) * WV_W) / (len >> sh);
            w->mk = (uint16_t)cx; w->hl0 = (uint16_t)cx; w->hl1 = (uint16_t)(cx + gw > WV_W - 1u ? WV_W - 1u : cx + gw);
            w->br0 = w->hl0; w->br1 = w->hl1;
            if (sw) { w->sp0 = (uint16_t)(cx > sw ? cx - sw : 0u); w->sp1 = (uint16_t)(w->hl1 + sw > WV_W - 1u ? WV_W - 1u : w->hl1 + sw); } else w->sp0 = w->sp1 = 0xFFFFu;
            ps[0] = 1u; ps[1] = now;
            redraw_me(mgr);
            return;
        }
        if (ps[0]) { ps[0] = 0u; ps[1] = WV_NOW(); wv_layout(w); redraw_me(mgr); }
        return;
    }
    if (w->nsl && RAM_OK(node) && *(const volatile uint32_t *)node == 0x0009C59Cu) {
        const volatile uint8_t *st = (const volatile uint8_t *)(node + 0xD8u);
        uint32_t len = *(const volatile uint32_t *)(st + 0x24), pos = *(const volatile uint32_t *)(st + 0x44);
        if (st[0x18] && len >= 2u && pos < len) {
            uint32_t sh = 0;
            while ((len >> sh) >= (1u << 22)) sh++;           /* pos × 304 passt in 32 Bit */
            px = ((pos >> sh) * WV_W) / (len >> sh) + 1u;
        }
    }
    if (px == ps[0]) return;
    uint32_t now = WV_NOW();
    if (px && ps[0] && now - ps[1] < PLAY_MS) return;
    ps[0] = px; ps[1] = now;
    redraw_me(mgr);
}

/* Hauptschleife (sl_poll): Waveform rechnen/hochladen, Seite beobachten */
void wv_poll(void) {
    volatile wv_t *w = WV;
    if (w->magic != WV_MAGIC) return;
    if (!w->on) { w->todo = 0; return; }
    play_tick(w->mgr);
    uint32_t mgr = w->mgr, v = w->view;
    if (v == 1u) goto compute;                           /* 026: View-Modus (wv_kb), keine Seite zu beobachten */
    if (!RAM_OK(mgr) || !RAM_OK(v)) { w->on = 0; return; }
    /* Seite abgebaut (vt[2] 0x2F14C setzt die Widget-Nummern auf −1) oder Plaetze neu vergeben → aus */
    int gone = 0;
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t id = *(const volatile uint32_t *)(v + 0xDCu + 4u * i);
        if (id >= 50u || slot_ptr(mgr, id) != w->vobj[i]) gone = 1;
    }
    if (gone || ensure_slot(mgr) >= 50u) { off(mgr); w->todo = 0; return; }
    if (view_any_visible(mgr, v)) { hide_view(mgr, v); redraw_me(mgr); }      /* Seite hat sich neu gezeichnet */
compute:
    if (!w->frames || !w->data) { w->todo = 0; return; }
    if (w->col < WV_W) {
        const int16_t *d = (const int16_t *)(uintptr_t)w->data;
        sv_x_t *xr = sv_xget(1);
        for (uint32_t n = 0; n < COLS_PER_POLL && w->col < WV_W; n++, w->col++) {
            int8_t a, b;
            wv_column(d, w->frames, w->col, &a, &b);
            w->mn[w->col] = a; w->mx[w->col] = b;
            if (xr) xr->rms[w->col] = wv_column_rms(d, w->frames, w->col);   /* 045 */
        }
        w->todo = 1;
        return;
    }
    if (!w->upl) {
        if (*(const volatile uint32_t *)(mgr + 4u) != 1u) return;      /* EVE noch nicht initialisiert */
        uint8_t row[WV_STRIDE];
        sv_x_t *xr_ = sv_xget(0);
        for (uint32_t n = 0; n < ROWS_PER_POLL && w->row < WV_H; n++, w->row++) {
            wv_row(w->mn, w->mx, xr_ ? (const volatile uint8_t *)xr_->rms : 0, w->row, row);
            RAMG_WRITE(row, WV_STRIDE, RAMG_BMP + WV_STRIDE * w->row);
        }
        if (w->row >= WV_H) { w->upl = 1; redraw_me(mgr); }
        w->todo = 1;
        return;
    }
    w->todo = 0;
}

/* 026: View/Tastatur auf einer Slicer-Spur – Waveform dauerhaft in der freien Bildmitte (View belegt nur Kopf- und
 * Fusszeile, Plaetze 10–18). k = gewaehlter Slice (oder −1), gate = Gate des Slices (0x7FFF = ganz), on = 0 → aus.
 * Aufruf jede Hauptschleife aus slview.c; neu gezeichnet wird nur bei Aenderung. */
static int txt_same(const volatile char *a, const char *b) {
    for (uint32_t i = 0; i < 23u; i++) { if (a[i] != b[i]) return 0; if (!b[i]) return 1; }
    return 1;
}
void wv_kb(int on, uint32_t t, int32_t k, int32_t gate, const char *txt0, const char *ileft, const char *txt1, uint32_t foc) {   /* 030: oben rechts, Infozeile links/rechts, Fokus */
    volatile wv_t *w = WV;
    if (w->magic != WV_MAGIC || SLR->magic != SL_MAGIC) return;
    uint32_t mgr = 0x1FFF42CCu;
    if (*(const volatile uint32_t *)mgr != 0x0009AA1Cu) return;
    if (!on || t >= 8u) {
        if (w->on && w->view == 1u) {
            off(mgr); hid_restore(mgr); redraw_me(mgr); w->view = 0; if (sv_fw_redraw) sv_fw_redraw();   /* 045 */
            /* 13.11.: steht danach die Step-Seite (Klasse 0xA0888, z. B. Sample Start), gleich unsere Ansicht darauf statt der alten */
            uint32_t scr = sv_fw_screen ? sv_fw_screen() : 0u;
            if (scr && *(const volatile uint32_t *)scr == 0x000A0888u) wv_view_apply((void *)(uintptr_t)scr);
        }
        return;
    }
    if (w->on && w->view != 1u) {                         /* 030: Slicer-Seite hat in View Vorrang vor der Step-Ansicht */
        uint32_t pg = w->view;
        off(mgr); show_view(mgr, pg); redraw_me(mgr);     /* 045: Felder der Step-Seite freigeben (sonst nach View leer) */
    }
    uint32_t st = track_step(t);                      /* 028: Sample aus einem Step DIESER Spur (Standard-Step erst nach Step-Beruehrung gueltig) */
    int32_t v10 = k < 0 ? 0 : k * 64 + 32;
    if (k >= 0 && sl_slices(SLR->mode[t]) && sv_steps_held_now()) {   /* 17.11.: gehaltener Step in View: dessen Feinversatz zeigen (wie ausserhalb von View) */
        int32_t pv_ = wv_touched_param(10u);
        if (pv_ != -32768 && sl_slice_index(pv_, sl_slices(SLR->mode[t])) == (uint32_t)k) v10 = pv_;
    }
    else if (k >= 0 && sl_slices(SLR->mode[t])) {        /* 17.11.: ohne Halten: Feinversatz der Vorlage am gewaehlten Slice zeigen */
        int32_t pv_ = wv_touched_param(10u);
        if (pv_ != -32768) v10 = k * 64 + (pv_ & 63);
    }
    uint32_t key = ((uint32_t)sl_fine(v10) * 2246822519u) ^ (((uint32_t)k & 0xFFu) | ((uint32_t)gate << 8)) ^ (t << 28) ^ ((uint32_t)SLR->mode[t] << 24) ^ ((uint32_t)(sv_off(t) & 0xFF) << 16) ^ (sv_grid_gen(t) * 2654435761u) ^ ((uint32_t)env_a(w) * 40503u) ^ ((uint32_t)env_d(w) * 7919u)
        ^ (sl_is_granular(SLR->mode[t]) ? (uint32_t)wv_touched_param(10u) * 31u + (uint32_t)wv_touched_param(11u) * 131u + gr_cfg(t)->win * 977u + gr_cfg(t)->spray * 8191u + gr_cfg(t)->drone * 524287u : 0u);
    int changed = !w->on || w->view != 1u || w->track != t || w->mode != SLR->mode[t] || (uint32_t)w->rec_t != key;
    int32_t si = find_sample(st, t);
    if (wv_key(w) != w->c_data || w->frames != w->c_frames) {
        w->c_data = wv_key(w); w->c_frames = w->frames; w->col = 0; w->row = 0; w->upl = 0; changed = 1;
    }
    if ((uint32_t)w->v13 != foc) changed = 1;
    if (!txt_same(w->t1, txt0) || !txt_same(w->i0, ileft) || !txt_same(w->i1, txt1)) changed = 1;
    /* View blendet beim Spielen eigene Anzeigen in der Bildmitte ein (Sample-Name, Balken): solange die Waveform steht,
     * alles Sichtbare mit y in 24…210 ausblenden – ausser uns und den Meldungen (Plaetze 7–9, Toast). */
    if (w->on && w->view == 1u) {
        int hid = 0;
        for (uint32_t k2 = 0; k2 < 50u; k2++) {
            if (k2 == w->slot || (k2 >= 7u && k2 <= 9u)) continue;
            uint32_t o = slot_ptr(mgr, k2);
            if (!RAM_OK(o) || !(((const volatile uint32_t *)o)[3] & 1u)) continue;
            int32_t y = ((const volatile int32_t *)o)[8];          /* +0x20 */
            if (y >= 24 && y <= 210) { mgr_call(mgr, 0x28u, k2); hid = 1; hid_note(k2); }
        }
        if (hid) redraw_me(mgr);
    }
    if (!changed) return;
    w->rec_t = key;
    w->mode = SLR->mode[t]; w->track = t;
    w->v10 = v10; w->v11 = gate; w->v13 = 0; w->v14 = 0;
    if (sl_is_granular(SLR->mode[t])) {                  /* 039: Vorlage bzw. gehaltener Step */
        int32_t pv_ = wv_touched_param(10u), sz_ = wv_touched_param(11u);
        w->v10 = pv_ == -32768 ? 0 : pv_; w->v11 = sz_ == -32768 ? 0x7FFF : sz_;
        w->v13 = (int32_t)gr_cfg(t)->win * 100; w->v14 = (int32_t)gr_cfg(t)->spray * 100;
    }
    copy_txt(w->t1, txt0, sizeof(w->t1));
    wv_layout(w);
    {                                                     /* 030: oben Sample-Name + Slice, Infozeile von slview.c */
        uint32_t base = *(const volatile uint32_t *)(APP_SAMPLES + 4u);
        w->t0[0] = 0;
        if (si >= 0) copy_txt(w->t0, (const volatile char *)(base + 0x38u * (uint32_t)si + 0x14u), 18u);
        copy_txt(w->i0, ileft, sizeof(w->i0));
        copy_txt(w->i1, txt1, sizeof(w->i1));
        w->v13 = (int32_t)foc;
    }
    if (ensure_slot(mgr) >= 50u) { w->on = 0; return; }
    w->mgr = mgr; w->view = 1u;
    mgr_call(mgr, 0x24u, w->slot);
    w->on = 1; w->todo = 1;
    redraw_me(mgr);
}

/* 030: Standard-Step = zuletzt beruehrter Step (FUN_0002c644) */
static uint32_t touched_step(void) { return *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu); }
int32_t wv_touched_slice(uint32_t n) {
    int16_t v = step_param(touched_step(), 10u, 0);
    return n ? (int32_t)sl_slice_index(v, n) : -1;
}
void wv_cur_name(char *d, uint32_t max) {
    volatile wv_t *w = WV; uint32_t i = 0;
    for (; i + 1u < max && w->t0[i]; i++) d[i] = w->t0[i];
    d[i] = 0;
}
int32_t wv_touched_gate(void) { return step_param(touched_step(), 11u, 0x7FFF); }
static int32_t step_param_ext(uint32_t idx) { return step_param(touched_step(), idx, 0); }
int32_t wv_touched_note(void) { int16_t n = step_param(touched_step(), 6u, 60); return n < 1 ? 60 : n; }
int32_t wv_touched_param(uint32_t idx) { return step_param(touched_step(), idx, -32768); }   /* -32768 = im Standard-Step nicht gesetzt */
uint32_t wv_off_ms(uint32_t t, uint32_t n) {
    volatile wv_t *w = WV;
    int32_t o = sv_off(t);
    if (!n || w->rate < 1000u) return 0u;
    uint32_t sl = (w->frames / n) / 1024u * (uint32_t)(1024 + sv_wid(t));  /* Slice in Frames */
    uint32_t fr = sl / 64u * (uint32_t)(o < 0 ? -o : o);
    return fr / (w->rate / 1000u);
}
uint32_t wv_bpm10(uint32_t t, uint32_t n) {
    volatile wv_t *w = WV;
    if (!n || !w->frames) return 0u;
    uint32_t sl = (uint32_t)(((uint64_t)w->frames * (uint32_t)(1024 + sv_wid(t))) >> 10) / n;   /* >> statt Division: keine Bibliothek */
    if (!sl) return 0u;
    uint32_t bpm = 150u * w->rate / sl;                  /* 60 s × 10 / (4 Slices je Schlag) */
    while (bpm >= 1800u) bpm /= 2u;
    while (bpm && bpm < 700u) bpm *= 2u;
    return bpm;
}
int wv_sample_data(uint32_t t, uint32_t *data, uint32_t *frames, uint32_t *rate) {
    int32_t si = sv_sample(t);
    if (si < 0) si = step_param(track_step(t), 9u, -1);
    uint32_t base = *(const volatile uint32_t *)(APP_SAMPLES + 4u);
    if (si < 0 || si >= 255 || !(RAM_OK(base) || MEM_SD(base))) return 0;
    const volatile uint32_t *rec = (const volatile uint32_t *)(base + 0x38u * (uint32_t)si);
    uint32_t b = rec[0], e = rec[1], fr = rec[3];
    if (b == e || !(RAM_OK(b) || MEM_SD(b)) || (b & 3u)) return 0;
    uint32_t d = *(const volatile uint32_t *)b;
    if (d < SDRAM_LO || (d & 1u) || fr < 2u || fr > (SDRAM_HI - d) / 2u) return 0;
    uint32_t r = wv_f2u(rec[4]);
    *data = d; *frames = fr; *rate = (r >= 4000u && r <= 192000u) ? r : 44100u;
    return 1;
}

/* 029: Klassen aller Samples (usbrec.c: Liste+8 → {u32 ordner[256]; u32 anzahl @0x400; char name[20][17] @0x404}) */
int wv_sample_classes(uint8_t *cls) {
    uint32_t base = *(const volatile uint32_t *)(APP_SAMPLES + 4u);
    uint32_t meta = *(const volatile uint32_t *)(APP_SAMPLES + 8u);
    static const char LR_NAME[] = "Live Recordings";
    uint32_t lr = 0xFFFFFFFFu;
    if (RAM_OK(meta) || MEM_SD(meta)) {
        uint32_t cnt = *(const volatile uint32_t *)(meta + 0x400u);
        for (uint32_t i = 0; i < cnt && i < 20u; i++) {
            const volatile char *nm = (const volatile char *)(meta + 0x404u + 17u * i);
            uint32_t k = 0; while (k < 16u && LR_NAME[k] && nm[k] == LR_NAME[k]) k++;
            if (!LR_NAME[k] && !nm[k]) { lr = i; break; }
        }
    } else meta = 0;
    int any = 0;
    for (uint32_t i = 0; i < 255u; i++) {
        cls[i] = 0;
        if (!(RAM_OK(base) || MEM_SD(base))) continue;
        const volatile uint32_t *rec = (const volatile uint32_t *)(base + 0x38u * i);
        if (rec[0] == rec[1] || rec[3] < 2u) continue;
        cls[i] = 1u;
        if (meta && *(const volatile uint32_t *)(meta + 4u * i) == lr) { cls[i] = 2u; any = 1; }
    }
    return any;
}
int32_t wv_first_param(uint32_t t, uint32_t idx) {   /* 13.11.: fuer slicer.c fmt_bulk */
    uint32_t st = track_step(t);
    return st ? step_param(st, idx, -1) : -1;
}
int32_t wv_track_sample(uint32_t t) {
    int16_t si = step_param(track_step(t), 9u, -1);
    return si;
}

/* aus fmt_any (slicer.c): welcher Step/Spur/Wert gerade formatiert wird */
void wv_note_fmt(uint32_t idx, int32_t value, int32_t rel, uintptr_t step) {
    volatile wv_t *w = WV;
    if (w->magic != WV_MAGIC) return;
    uint32_t dflt = *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu);
    {   /* 13.11.: genau EINE Audio-Spur gewaehlt (Bulk: Werte relativ, oder ohne Step) → diese Spur zeigen, Bezug = ihr erster
         * gesetzter Step (sonst Vorlage); Texte der Seite sind dann relativ ("Slices +2", "+5.0 %"). Vorher: keine Ansicht. */
        uint32_t m = *(const volatile uint32_t *)0x1FFF4518u & 0xFFFFu;
        if ((rel || !step) && (m & 0xFFu)) {               /* 13.11.: auch mehrere Spuren → die erste gewaehlte Audio-Spur */
            uint32_t t1 = 0; while (!((m >> t1) & 1u)) t1++;
            uint32_t st = track_step(t1);
            if (!w->rec_ok || w->rec_step != st) w->rec_have = 0;
            w->rec_ok = 1; w->rec_t = t1; w->rec_step = st;
            w->rec_have = (uint8_t)((w->rec_have & 3u) | (rel ? 0x10u : 0u));   /* 0x10 = relativ (Bulk) */
            return;
        }
    }
    if (rel) { w->rec_ok = 0; return; }                   /* mehrere Spuren/Steps (relativ): keine eindeutige Waveform */
    if (!step) step = dflt;                               /* 045: ohne Step gilt die Vorlage (das Sample, das gerade gesetzt wuerde) */
    uint32_t t = step ? sl_step_track(step) : 8u;
    if (t >= 8u && step && step == dflt) t = sv_cur_track();   /* 045: Vorlage: zuletzt gewaehlte Spur */
    if (!step || t >= 8u) { w->rec_ok = 0; return; }
    if (!w->rec_ok || w->rec_step != (uint32_t)step) { w->rec_have = 0; }
    w->rec_have &= (uint8_t)~0x10u;
    w->rec_ok = 1; w->rec_t = t; w->rec_step = (uint32_t)step;
    if (idx == 10u) { w->rec_v10 = value; w->rec_have |= 1u; }
    else if (idx == 11u) { w->rec_v11 = value; w->rec_have |= 2u; }
}
#endif

void wv_reset(void) {
    volatile wv_t *w = WV;
    volatile uint32_t *z = (volatile uint32_t *)(void *)w;
    for (uint32_t i = 0; i < sizeof(wv_t) / 4u; i++) z[i] = 0;
    w->slot = 0xFFFFFFFFu;
    w->magic = WV_MAGIC;
#ifndef HOST_TEST
    HID->magic = 0;                                       /* 045 */
#endif
}
