/* Patch 026/029 – Slicer in View. Polyend Play Rev A, Firmware 1.6.0. ENTWURF – nur am PC getestet.
 *
 * View/Tastatur: Auf einer Slicer-Spur werden die 28 Tastatur-Pads zu Slices.
 *   Pad                 → Slice waehlen + vorhoeren
 *   Step(s) halten + Pad → Slice auf die gehaltenen Steps
 *   Shift + Pad Reihe 1 → Seite (Slices 1–28, 29–32). 045: Shift gehalten → Seiten-Pads leuchten gruen (aktuelle hell).
 * Regler (Regeln des Nutzers, 033):
 *   Step gehalten: nur dieser Step (Original).
 *   In View ohne Step: vorhandene Steps bleiben unberuehrt. Klangregler stellen den Standard-Step ein: gilt fuers Vorhoeren und
 *   fuer neue Steps. Ist die Spur gewaehlt, wuerde die Firmware alle ihre Steps relativ aendern; dafuer wird die Auswahl fuer diesen
 *   Aufruf ausgeblendet. Vorbereitung des Samples: Sample, Track Length (Fokus unten: Grid/Beat), Move, Micromove (Breite).
 *   Ausserhalb von View: unveraendert Firmware. Spur gewaehlt + Regler = alle Steps relativ (Anzeige "~", "+x"), mit Undo.
 * Granular (039/040), View ohne Step: Pads = Position (halten = Wolke klingt, Loslassen = Decay). Gruenes Pad 3 = Drone: Pad rastet ein,
 * anderes Pad wandert ohne Neustart, dasselbe Pad beendet. Track Length/Play Mode = Grains/Direction,
 * Move/Micromove = Scan/Spray, Repeat Type/Grid = Window/Pitch Spray, Chance/Action = LFO Rate/Depth, Shift + Chance = LFO-Ziel.
 * Fokus (oberer/unterer Parameter eines Knobs) waehlen die zwei linken Tasten (Seite+200, FUN_0001ff7c), nicht Shift.
 * Die Einstellungen je Slice aus 026 sind entfernt. RAM: sv_t 0x1FFFB940.
 */
#include <stdint.h>
#include "slicer.h"
#include "slview.h"
#include "wave.h"
#include "tap.h"

#ifdef HOST_TEST
#include <stdlib.h>
__attribute__((weak)) void *host_alloc(uint32_t n) { return calloc(1, n); }
#define ALLOC(n)    host_alloc(n)
__attribute__((weak)) uint8_t host_sv_ram[0xC0];
#define SV          ((volatile sv_t *)(void *)host_sv_ram)
#else
#define SV          ((volatile sv_t *)0x1FFFB940u)
#define ALLOC(n)    (((void *(*)(uint32_t))0x00056F5Du)(n))
#define SL          ((volatile sl_t *)0x1FFF9800u)
#define EDITOR      (*(volatile uint32_t *)0x1FFF6994u)
#define SEL_MASK    (*(volatile uint32_t *)0x1FFF4518u)          /* gewaehlte Spuren, Bit t (am Geraet bestaetigt) */
#define SELOBJ      0x1FFF450Cu                                  /* Editor+8: Auswahl */
#define ED_VT       0x000A5A54u
#define ORIG_KB     ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))0x0003A1A9u)
#define ORIG_CHG    ((void (*)(uint32_t, int, int))0x0003B089u)
#define KB_PLAY     ((void (*)(uint32_t, int, uint32_t, int))0x00023335u)   /* FUN_00023334(tastatur, note, vel, an) */
#define UI_REFRESH  ((void (*)(uint32_t, uint32_t))0x0002141Du)          /* FUN_0002141c(seqctl, art) */
#define TOAST(t)    do { if (*(const volatile uint32_t *)0x1FFF6CF8u == 0x1FFF42CCu) ((void (*)(uint32_t, const char *))0x0002A395u)(0x1FFF6CF8u, (t)); } while (0)
#define NODE_SLOT(n,i) (((void *(*)(void *, uint32_t))(((const uint32_t *)(*(const uint32_t *)(n)))[6]))((n),(i)))
#define SLOT_SET    ((void (*)(void *, uint32_t, int32_t))0x00014601u)
#define ORIG_PADLED ((void (*)(uint32_t, uint32_t, uint32_t, int, uint32_t))0x00023151u)  /* FUN_00023150(tastatur, reihe, spalte, note, seite) */
#define COL_OFF     0x0009DC70u                                  /* Farben je 3 Byte (2 Varianten: + (seite^1)*3) */
#define COL_NORMAL  0x0009DC74u
#define COL_BRIGHT  0x0009DC9Cu
#define ORIG_GRID   ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))0x0003BC21u)  /* Editor vt+0x14: Grid-Pad */
#define STEP_ON(sel,t,st) (((uint32_t (*)(uint32_t, uint32_t, uint32_t))0x0002C51Du)((sel),(t),(st)) & 0xFFu)  /* FUN_0002c51c */
#endif

#define SV_MAGIC    0x5356E029u
#ifndef HOST_TEST
typedef struct { uint32_t magic, key, t_ms; char a[18], b[18]; } sfc_t;   /* Zwischenspeicher 0x1FFFB780 (frei) */
#define SFC ((volatile sfc_t *)0x1FFFB780u)
_Static_assert(0x1FFFB780u + sizeof(sfc_t) <= 0x1FFFB900u, "sfc_t vor NIS");
#endif
#define PADS        28u
#define AUD_NOTE    60

void sv_reset(void) {
    volatile uint32_t *z = (volatile uint32_t *)(void *)SV;
    for (uint32_t i = 0; i < sizeof(sv_t) / 4u; i++) z[i] = 0;
    SV->magic = SV_MAGIC;
#ifndef HOST_TEST
    SFC->magic = 0;                                       /* 045: Fokus-Zwischenspeicher Sample/Folder */
    *(volatile uint32_t *)0x1FFFB800u = 0;                /* 045: tpl_t (Vorlagen-Schutz) */
#endif
}

sv_x_t *sv_xget(int create) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC) return 0;
    if (!v->x && create) {
        volatile uint8_t *p = (volatile uint8_t *)ALLOC(sizeof(sv_x_t));
        if (!p) return 0;
        for (uint32_t i = 0; i < sizeof(sv_x_t); i++) p[i] = 0;
        ((sv_x_t *)(uintptr_t)p)->sc_t = 8u;
        v->x = (uint32_t)(uintptr_t)p;
    }
    return (sv_x_t *)(uintptr_t)v->x;
}

void sv_clear_track(uint32_t t) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || t >= 8u) return;
    v->sample[t] = 0; v->off[t] = 0;
    gr_cfg_default(t);
    sv_x_t *x = sv_xget(0);
    if (x) {
        x->wid[t] = 0; x->smode[t] = 0; x->gen[t]++; x->fine[t] = 0; x->gmode[t] = 0;
        x->b[t].data = 0; x->b[t].frames = 0; x->b[t].done = 0; x->b[t].nt = 0; x->b[t].npk = 0;
        if (x->sc_t == t) x->sc_t = 8u;
        x->ckey[t] = 0;                                   /* 12.11.: Handschnitte */
        for (uint32_t k = 0; k < 32u; k++) x->cut[t][k] = 0;
        x->hold[t] = 0;
    }
    v->sel[t] = 0; v->page[t] = 0; v->kmode[t] = 0;
}

#ifdef HOST_TEST                                          /* 12.11.: Host-Tests ohne wave.c / slicer.c (beat_test) */
__attribute__((weak)) int wv_sample_data(uint32_t t, uint32_t *d, uint32_t *f, uint32_t *r) { (void)t; (void)d; (void)f; (void)r; return 0; }
__attribute__((weak)) void sl_bounds(uint32_t t, uint32_t k, uint32_t n, int32_t g, int32_t *a, int32_t *b) { (void)t; (void)k; (void)n; (void)g; *a = 0; *b = 1; }
#endif
/* ---------- 12.11.: Handschnitte (gruenes Pad 3 "Cut": Sample Start verschiebt den Anfang des gewaehlten Slices) ----------
 * Gelten nur fuer das Raster, auf dem sie gemacht wurden: Schluessel = Slice-Zahl | Grid/Beat | Sample-Laenge (nicht die Adresse,
 * die aendert sich beim Laden). Andert sich eins davon, sind die Schnitte weg (das alte Raster passt nicht mehr). */
uint32_t sv_cut_key(uint32_t t, uint32_t n) {
    uint32_t d = 0, fr = 0, rate = 44100u;
    if (t >= 8u || !wv_sample_data(t, &d, &fr, &rate)) fr = 0;
    sv_x_t *x = sv_xget(0);
    uint32_t sm = (x && t < 8u) ? x->smode[t] : 0u;
    return ((n & 0x7Fu) | 0x80u) | (sm << 8) | (((fr ^ (fr >> 16)) & 0xFFFFu) << 16);
}
int32_t sv_cut(uint32_t t, uint32_t k, uint32_t n) {
    sv_x_t *x = sv_xget(0);
    if (!x || t >= 8u || k >= 32u || !x->ckey[t] || !x->cut[t][k]) return 0;
    return x->ckey[t] == sv_cut_key(t, n) ? x->cut[t][k] : 0;
}
int32_t sv_cut_any(uint32_t t, uint32_t n) {
    sv_x_t *x = sv_xget(0);
    if (!x || t >= 8u || !x->ckey[t] || x->ckey[t] != sv_cut_key(t, n)) return 0;
    for (uint32_t k = 0; k < 32u; k++) if (x->cut[t][k]) return 1;
    return 0;
}
void sv_cut_set(uint32_t t, uint32_t key, uint32_t k, int32_t v) {
    sv_x_t *x = sv_xget(1);
    if (!x || t >= 8u || k >= 32u || v < -0x7FFF || v > 0x7FFF) return;
    if (x->ckey[t] != key) { x->ckey[t] = key; for (uint32_t i = 0; i < 32u; i++) x->cut[t][i] = 0; }
    x->cut[t][k] = (int16_t)v;
    x->gen[t]++;
}
/* Einheiten (0…0x7FFF des Samples) je ms; Laenge in ms */
static uint32_t cut_u(uint32_t t, uint32_t *len_ms) {
    uint32_t d = 0, fr = 0, rate = 44100u;
    if (!wv_sample_data(t, &d, &fr, &rate) || !fr || rate < 1000u) { *len_ms = 0; return 16u; }
    *len_ms = fr / (rate / 1000u);
    uint32_t u = (rate / 1000u) * 0x7FFFu / fr;
    return u ? u : 1u;
}
int32_t sv_cut_ms(uint32_t t, uint32_t k, uint32_t n) {
    uint32_t L = 0; (void)cut_u(t, &L);
    int32_t c = sv_cut(t, k, n);
    int32_t m = ((c < 0 ? -c : c) * (int32_t)(L >> 4) + (0x7FFF >> 5)) / (0x7FFF >> 4);   /* gerundet */
    return c < 0 ? -m : m;
}
/* Anfang von Slice k um dir Rasten (1 ms, schnell 10 ms) verschieben; bleibt zwischen den Nachbarn (2 ms Abstand) */
void sv_cut_turn(uint32_t t, uint32_t k, uint32_t n, int dir, uint32_t now) {
    sv_x_t *x = sv_xget(1);
    if (!x || t >= 8u || k >= n || k >= 32u || !dir) return;
    uint32_t key = sv_cut_key(t, n);
    if (x->ckey[t] != key) { x->ckey[t] = key; for (uint32_t i = 0; i < 32u; i++) x->cut[t][i] = 0; }
    uint32_t L = 0, u = cut_u(t, &L);
    int q = (now - x->cut_t) < 60u ? 10 : 1;
    x->cut_t = now;
    int32_t a, b, pa, pb, na, nb;
    sl_bounds(t, k, n, 0x7FFF, &a, &b);
    int32_t lo = 0, hi = b - 2 * (int32_t)u;
    if (k > 0u) { sl_bounds(t, k - 1u, n, 0x7FFF, &pa, &pb); lo = pa + 2 * (int32_t)u; }
    (void)na; (void)nb;
    int32_t nw = a + dir * q * (int32_t)u;
    if (nw < lo) nw = lo;
    if (nw > hi) nw = hi;
    int32_t c = x->cut[t][k] + (nw - a);
    if (c < -0x7FFF) c = -0x7FFF;
    if (c > 0x7FFF) c = 0x7FFF;
    x->cut[t][k] = (int16_t)c;
    x->gen[t]++;
}

volatile uint32_t *sv_dirty_flag(void) { return SV->magic == SV_MAGIC ? &SV->dirty : 0; }
void sv_set_ui(uint32_t ui) { if (SV->magic == SV_MAGIC) SV->ui = ui; }
uint32_t sv_ui(void) { return SV->magic == SV_MAGIC ? SV->ui : 0u; }   /* 045: fuer tap.c (Menue offen?) */
/* 045: neues Sample auf der Spur = frisches Raster (Grid-Breite und Move auf Fit) */
void sv_grid_fit(uint32_t t) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || t >= 8u) return;
    v->off[t] = 0;
    sv_x_t *x = sv_xget(0);
    if (x) { x->wid[t] = 0; x->fitp[t] = 0; x->gen[t]++; }
}
/* 045: wie viele der n Slices im Sample liegen (Grid breiter als Fit → hintere fallen weg) */
uint32_t sv_fit_count(uint32_t t, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t k = 0; k < n; k++) if (!sl_slice_outside(t, k, n)) c++;
    return c;
}
/* 045: Shift gehalten (UI+0x18, gueltig nach dem Ereignis von Taste 10) */
int sv_shift(void) { uint32_t ui = SV->ui; return (SV->magic == SV_MAGIC && ui >= 0x1FFF0000u && ui < 0x20030000u) ? *(volatile uint8_t *)(ui + 0x18u) != 0u : 0; }
static void sample_focus(uint32_t t, char *a, char *b);
static int step_ok(uint32_t st);
static void step_set(uint32_t st, uint32_t idx, int32_t val);   /* 17.11. */
static int32_t step_get(uint32_t st, uint32_t idx);
uint32_t sl_orig_fmt(uint32_t idx);                     /* slicer.c (045) */
#ifndef HOST_TEST
static uint32_t cur_track(void);
static int kb_active(uint32_t ed);
#endif
#ifdef HOST_TEST
__attribute__((weak)) uint32_t host_cur_track = 8u; __attribute__((weak)) int host_multi_held;
uint32_t sv_cur_track(void) { return host_cur_track; }
int sv_multi_held(void) { return host_multi_held; }
#else
uint32_t sv_cur_track(void) { return SV->magic == SV_MAGIC ? cur_track() : 8u; }   /* 045: fuer wave.c */
/* 045: mehr als ein Step gehalten (Waveform waere nicht eindeutig) */
int sv_multi_held(void) {
    uint32_t ed = EDITOR, n = 0;
    if (ed < 0x1FFF0000u || ed >= 0x20030000u) return 0;
    volatile uint32_t *h = (volatile uint32_t *)(ed + 0x88u);
    for (uint32_t i = 0; i < 4u; i++) { uint32_t x = h[i]; while (x) { x &= x - 1u; n++; } }
    return n > 1u;
}
#endif
/* 045: Bildschirm der Firmware neu zeichnen wie bei jedem Seitenwechsel (FUN_000307c4(Manager, aktiver Bildschirm): gleicher
 * Bildschirm → nur dessen vt+0x0C). Aufruf beim Verlassen von View, damit keine leere Seite stehen bleibt. */
/* 13.11.: aktiver Bildschirm der Firmware (Seitenverwaltung = UI+8, +0x2FC), 0 = unbekannt */
uint32_t sv_fw_screen(void) {
#ifndef HOST_TEST
    uint32_t ui = SV->ui;
    if (SV->magic != SV_MAGIC || ui < 0x1FFF0000u || ui >= 0x2003FF00u) return 0;
    uint32_t sm = *(volatile uint32_t *)(ui + 8u);
    if (sm < 0x1FFF0000u || sm >= 0x2003FF00u) return 0;
    uint32_t scr = *(volatile uint32_t *)(sm + 0x2FCu);
    return (scr >= 0x1FFF0000u && scr < 0x2003FF00u) ? scr : 0u;
#else
    return 0;
#endif
}
void sv_fw_redraw(void) {
#ifndef HOST_TEST
    uint32_t ui = SV->ui;
    if (SV->magic != SV_MAGIC || ui < 0x1FFF0000u || ui >= 0x2003FF00u) return;
    uint32_t sm = *(volatile uint32_t *)(ui + 8u);
    if (sm < 0x1FFF0000u || sm >= 0x2003FF00u) return;
    uint32_t scr = *(volatile uint32_t *)(sm + 0x2FCu);
    if (scr < 0x1FFF0000u || scr >= 0x2003FF00u) return;
    uint32_t vt = *(volatile uint32_t *)scr;
    if (vt < 0x9A000u || vt >= 0xAE000u) return;
    ((void (*)(uint32_t, uint32_t))0x000307C5u)(sm, scr);
#endif
}

/* 045: Shift gedrueckt/losgelassen → Pad-LEDs neu, wenn die Spur in View mehr Slices als Pads hat */
void sv_shift_changed(void) {
#ifndef HOST_TEST
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || SL->magic != SL_MAGIC) return;
    uint32_t t = cur_track();
    if (t < 8u && sl_slices(SL->mode[t]) > PADS && kb_active(EDITOR)) UI_REFRESH(*(volatile uint32_t *)(EDITOR + 4u), 1u);
#endif
}

/* Auswertung beim Abspielen (aus slicer.c) */
int32_t sv_off(uint32_t t) {
    volatile sv_t *v = SV;
    return (v->magic == SV_MAGIC && t < 8u) ? v->off[t] : 0;
}
int32_t sv_wid(uint32_t t) { sv_x_t *x = sv_xget(0); return (x && t < 8u) ? x->wid[t] : 0; }
int sv_beat_on(uint32_t t) { sv_x_t *x = sv_xget(0); return (x && t < 8u) ? x->smode[t] != 0u : 0; }
int sv_beat_ready(uint32_t t) { sv_x_t *x = sv_xget(0); return (x && t < 8u && x->smode[t]) ? x->b[t].done != 0u : 0; }
uint32_t sv_grid_gen(uint32_t t) { sv_x_t *x = sv_xget(0); return (x && t < 8u) ? x->gen[t] : 0u; }

/* ---- 031: Beat-Erkennung. Energie je Hop, Anstieg = Anschlag; nur die letzten Werte im Speicher (kein Puffer ueber das Sample). */
/* 032: Anschlag = Energie je Hop (~6 ms) ueberragt die abklingende Huellkurve davor deutlich. Nachklang und Wackler im Ausklang
 * zaehlen so nicht, leise Schlaege nach einer Pause schon. Position danach samplegenau: erster Wert ueber 1/4 der Spitze. */
void sv_scan_start(sv_x_t *x, uint32_t t, uint32_t data, uint32_t frames, uint32_t rate) {
    sv_beat_t *b = &x->b[t];
    b->data = data; b->frames = frames; b->npk = 0; b->nt = 0; b->nfor = 0; b->done = 0; b->rate = rate;
    x->sc_t = t; x->sc_i = 0; x->sc_prev = 0; x->sc_last = 0;
    uint32_t hop = rate / 172u;
    if (hop < 64u) hop = 64u;
    x->sc_hop = hop; x->sc_n = frames / hop;
    uint32_t d = (rate / 1000u) * 25u / hop;              /* Mindestabstand 25 ms */
    x->sc_dist = d ? d : 1u;
}
static uint32_t scan_exact(const volatile int16_t *dt, uint32_t frames, uint32_t hop, uint32_t i) {
    uint32_t f0 = i ? (i - 1u) * hop : 0u, f1 = (i + 2u) * hop;
    if (f1 > frames) f1 = frames;
    uint32_t pk = 0;
    for (uint32_t f = f0; f < f1; f++) { int32_t v = dt[f]; uint32_t a = (uint32_t)(v < 0 ? -v : v); if (a > pk) pk = a; }
    uint32_t th = pk / 4u, pos = i * hop;
    for (uint32_t f = f0; f < f1; f++) { int32_t v = dt[f]; if ((uint32_t)(v < 0 ? -v : v) > th) { pos = f; break; } }
    uint32_t back = hop / 6u;                            /* ~1 ms davor: der Anschlag wird nicht angeschnitten */
    return pos > back ? pos - back : 0u;
}
int sv_scan_run(sv_x_t *x, uint32_t max_hops) {
    uint32_t t = x->sc_t;
    if (t >= 8u) return 1;
    sv_beat_t *b = &x->b[t];
    const volatile int16_t *dt = (const volatile int16_t *)(uintptr_t)b->data;
    uint32_t hop = x->sc_hop, stride = hop / 32u ? hop / 32u : 1u;
    while (max_hops-- && x->sc_i < x->sc_n) {
        uint32_t i = x->sc_i, f0 = i * hop, sum = 0, dif = 0, cnt = 0, pk = 0;
        for (uint32_t f = 0; f + 1u < hop; f += stride) {     /* Paare: Differenz betont scharfe Anschlaege auf einem Ausklang */
            int32_t v = dt[f0 + f], w = dt[f0 + f + 1u]; uint32_t a = (uint32_t)(v < 0 ? -v : v);
            int32_t dd = w - v;
            sum += a; dif += (uint32_t)(dd < 0 ? -dd : dd); cnt++; if (a > pk) pk = a;
        }
        uint32_t e = (sum + dif * 4u) / cnt + pk / 4u;
        uint32_t env = x->sc_prev;                       /* Huellkurve vor diesem Hop */
        x->sc_prev = e > env ? e : env - env / 8u;
        x->sc_i = i + 1u;
        if (e <= env + env / 3u || e < 80u) continue;    /* mindestens 1,33fach und kein Grundrauschen */
        if (x->sc_last && i - (x->sc_last - 1u) < x->sc_dist && b->npk) {
            uint32_t q = b->npk - 1u;
            if (pk > b->str[q]) b->str[q] = (uint16_t)pk;       /* gleicher Anschlag: Spitze merken, Position bleibt am Beginn */
            continue;
        }
        uint32_t q = b->npk;
        if (q >= SV_PEAKS) {                             /* voll: schwaechsten ersetzen, Reihenfolge nach Zeit bleibt */
            uint32_t mn = 0;
            for (uint32_t j = 1; j < SV_PEAKS; j++) if (b->str[j] < b->str[mn]) mn = j;
            if (pk <= b->str[mn]) continue;
            for (uint32_t j = mn; j + 1u < SV_PEAKS; j++) { b->pos[j] = b->pos[j + 1u]; b->str[j] = b->str[j + 1u]; }
            q = SV_PEAKS - 1u;
        } else b->npk = (uint8_t)(q + 1u);
        uint32_t fx = scan_exact(dt, b->frames, hop, i);
        {                                                /* ohne 64-Bit-Division: skalieren, abgerundet = eher etwas frueher */
            uint32_t sh = 0;
            while ((b->frames >> sh) >= (1u << 17)) sh++;
            b->pos[q] = (uint16_t)((fx >> sh) * 0x7FFFu / (b->frames >> sh));
        }
        b->str[q] = (uint16_t)pk; x->sc_last = i + 1u;     /* Staerke = Spitze: Schwelle von oben durch die Waveform */
    }
    if (x->sc_i < x->sc_n) return 0;
    b->done = 1; b->nfor = 0; x->sc_t = 8u;
    return 1;
}
/* Sensitivity 1 … 16: Schwelle sinkt von oben durch die Waveform. Slice-Beginn = jeder Anschlag, dessen Spitze die Schwelle erreicht.
 * Dadurch nur die lautesten zuerst, und jede hoehere Stufe fuegt nur hinzu, nie weg. Slice 1 beginnt immer bei 0. */
void sv_beat_pick(sv_beat_t *b, uint32_t level) {
    static const uint8_t PCT[16] = { 90, 80, 70, 60, 50, 42, 35, 28, 22, 17, 13, 10, 7, 5, 3, 2 };
    if (level < 1u) level = 1u;
    if (level > 16u) level = 16u;
    uint32_t np = b->npk, mx = 0;
    for (uint32_t i = 0; i < np; i++) if (b->str[i] > mx) mx = b->str[i];
    uint32_t th = mx * PCT[level - 1u] / 100u;
    b->thr = (uint16_t)th;
    uint32_t m = 0;
    b->tab[m++] = 0;
    uint32_t near0 = b->frames ? (b->rate / 1000u * 25u) * 0x7FFFu / b->frames : 0u;   /* Anschlag direkt am Anfang: das ist Slice 1 */
    /* 045: hoechstens SL_MAXSLICES Slices. Zu viele Anschlaege ueber der Schwelle → die staerksten (bei Gleichstand die frueheren)
     * behalten, in zeitlicher Reihenfolge. Eine hoehere Schwelle allein versagt bei gleich starken Anschlaegen. */
    uint64_t keep = 0; uint32_t cand = 0;
    for (uint32_t i = 0; i < np && i < 64u; i++) if (b->str[i] && b->str[i] >= th && b->pos[i] > near0) { keep |= (uint64_t)1 << i; cand++; }
    if (cand > SL_MAXSLICES - 1u) {
        uint64_t sel = 0;
        for (uint32_t k = 0; k < SL_MAXSLICES - 1u; k++) {
            uint32_t bi = 64u, bs = 0;
            for (uint32_t i = 0; i < np && i < 64u; i++)
                if (((keep >> i) & 1u) && !((sel >> i) & 1u) && (bi == 64u || b->str[i] > bs)) { bi = i; bs = b->str[i]; }
            if (bi == 64u) break;
            sel |= (uint64_t)1 << bi;
        }
        keep = sel;
    }
    for (uint32_t i = 0; i < np && i < 64u && m < SV_PEAKS; i++) if ((keep >> i) & 1u) b->tab[m++] = b->pos[i];
    b->tab[m] = 0x7FFFu;
    b->nt = (uint8_t)m; b->nfor = (uint8_t)level;
}
uint32_t sv_beat_thr(uint32_t t) { sv_x_t *x = sv_xget(0); return (x && t < 8u && x->smode[t] && x->b[t].done) ? x->b[t].thr : 0u; }
uint32_t sv_focus_knob(void) { return SV->knob; }
volatile uint32_t *sv_play_state(void) { return &SV->play_px; }
uint32_t sv_beat_level(uint32_t t) {
    sv_x_t *x = sv_xget(0);
    return (x && t < 8u && x->fine[t]) ? x->fine[t] : 8u;
}
uint32_t sv_beat_count(uint32_t t) {
    sv_x_t *x = sv_xget(0);
    return (x && t < 8u && x->smode[t] && x->b[t].done && x->b[t].nfor == sv_beat_level(t)) ? x->b[t].nt : 0u;
}
int sv_beat_bounds(uint32_t t, uint32_t k, uint32_t n, int32_t *a, int32_t *b) {
    (void)n;
    sv_x_t *x = sv_xget(0);
    if (!x || t >= 8u || !x->smode[t] || !x->b[t].done || x->b[t].nfor != sv_beat_level(t)) return 0;
    sv_beat_t *bt = &x->b[t];
    if (k >= bt->nt) { *a = 0x7FFE; *b = 0x7FFF; return 1; }
    *a = bt->tab[k]; *b = bt->tab[k + 1u];
    if (x->nudge[t] && bt->frames) {                  /* 045: Nudge in ms → Einheiten 0…0x7FFF; Slice 1 beginnt bei 0, letzter endet bei 0x7FFF */
        int32_t d = (int32_t)x->nudge[t] * (int32_t)(bt->rate / 100u) / 10 * 0x7FFF / (int32_t)bt->frames;
        if (*a > 0) { *a += d; if (*a < 0) *a = 0; if (*a > 0x7FFE) *a = 0x7FFE; }
        if (*b < 0x7FFF) { *b += d; if (*b < 1) *b = 1; if (*b > 0x7FFF) *b = 0x7FFF; }
        if (*b <= *a) *b = *a + 1;
    }
    return 1;
}
/* Wechsel Grid/Beat. Beat rechnet immer neu (das Sample kann getauscht sein) und nutzt alle 64 Pad-Plaetze; die Slice-Zahl kommt
 * aus der Genauigkeit. Zurueck: gemerkte Slice-Zahl. */
void sv_beat_toggle(uint32_t t, int on, volatile int32_t *mode) {
    sv_x_t *x = sv_xget(1);
    if (!x || t >= 8u) return;
    if (on && !x->smode[t]) { x->gmode[t] = (uint8_t)*mode; *mode = SL_MAXSLMODE; }   /* 045: Beat hoechstens 32 Slices */
    else if (!on && x->smode[t]) { if (x->gmode[t] >= 1u && x->gmode[t] <= 8u) *mode = x->gmode[t]; }
    if (on) { x->b[t].data = 0; x->b[t].frames = 0; x->b[t].done = 0; if (x->sc_t == t) x->sc_t = 8u; }
    x->smode[t] = on ? 1u : 0u;
    x->gen[t]++;
}
void sv_fine_step(uint32_t t, int dir) {
    sv_x_t *x = sv_xget(1);
    if (!x || t >= 8u) return;
    int32_t l = (int32_t)sv_beat_level(t) + (dir > 0 ? 1 : -1);
    x->fine[t] = (uint8_t)(l < 1 ? 1 : l > 16 ? 16 : l);
    x->gen[t]++;
}

int32_t sv_sample(uint32_t t) {
    volatile sv_t *v = SV;
    return (v->magic == SV_MAGIC && t < 8u) ? (int32_t)v->sample[t] - 1 : -1;
}
#ifndef HOST_TEST
/* 17.11.: Spur ohne festes Sample (nach dem Start der Normalfall): Pads in View spielen das Sample des ersten gesetzten Steps
 * (wv_track_sample), abgespielte Steps nahmen aber ihr eigenes – ein neuer Step das der Vorlage → Pad und Step klangen
 * verschieden. Jetzt wird beim ersten Abspielen das Sample festgelegt, das auch die Pads spielen (und mit dem Projekt gespeichert). */
int32_t sv_track_sample(uint32_t t) {
    int32_t si = sv_sample(t);
    if (si >= 0 || t >= 8u || SV->magic != SV_MAGIC) return si;
    si = wv_track_sample(t);
    if (si >= 0 && si < 255) { SV->sample[t] = (int16_t)(si + 1); SV->dirty = 1; }
    return si;
}
#endif
#ifdef HOST_TEST
uint32_t sv_page_track(void) { return 8u; }
void sv_capture(uint32_t i0, uint32_t i1, const volatile char *n0, const volatile char *v0, const volatile char *n1, const volatile char *v1, uint32_t sel) {
    (void)i0; (void)i1; (void)n0; (void)v0; (void)n1; (void)v1; (void)sel;
}
#endif
/* 045: steht ein gezieltes Vorhoeren an? */
int sv_audition_pending(uint32_t t) { volatile sv_t *v = SV; return v->magic == SV_MAGIC && t < 8u && v->aud[t]; }
/* Vorhoeren: naechster Trigger der Spur nimmt diesen Slice (sl_param_hook) */
int sv_take_audition(uint32_t t, uint32_t *k) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || t >= 8u || !v->aud[t]) return 0;
    *k = v->aud[t] - 1u; v->aud[t] = 0;
    return 1;
}

/* Regler in View: eine Raste. Gate 1 %, Attack/Decay 1 %, Versatz 1/64 Slice */
void sv_apply(uint32_t t, uint32_t idx, int dir) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || t >= 8u) return;
    int d = dir > 0 ? 1 : dir < 0 ? -1 : 0;
    if (idx == 20u) {                             /* dir kann beschleunigt sein: |dir| Rasten */
        sv_x_t *x = sv_xget(1);
        if (x) {
            int32_t w0 = x->wid[t], w = w0 + dir;
            if (w0 == 0) {                         /* 045: Fit rastet: erst die zweite Raste in dieselbe Richtung verlaesst Fit */
                int32_t f = x->fitp[t] + (dir > 0 ? 1 : -1);
                if ((f > 0) != (x->fitp[t] > 0) && x->fitp[t]) f = dir > 0 ? 1 : -1;
                x->fitp[t] = (int8_t)f;
                if (f > -2 && f < 2) return;
                x->fitp[t] = 0; w = dir > 0 ? 1 : -1;
            } else if ((w0 > 0 && w < 0) || (w0 < 0 && w > 0)) w = 0;   /* auf dem Weg zurueck haelt Fit an */
            x->wid[t] = (int16_t)(w < -512 ? -512 : w > 1024 ? 1024 : w); x->gen[t]++;
        }
    } else if (idx == 21u) {
        int32_t x = v->off[t] + d;
        v->off[t] = (int8_t)(x < -63 ? -63 : x > 63 ? 63 : x);
    }
}
/* Indizes mit eigener Anzeige auf der Slicer-Seite (die Step-Seite zeigte ohne gehaltenen Step nur den Standard-Step) */
int sv_own_param(uint32_t idx) { return idx == 9u || idx == 15u || idx == 10u || idx == 11u || idx == 20u || idx == 21u; }
/* "Name Wert" gekuerzt: der Wert bleibt ganz, der Name bekommt den Rest. max inkl. Nullbyte. */
void sv_join(char *d, uint32_t max, const volatile char *name, const volatile char *val) {
    uint32_t vl = 0, nl = 0, n = 0;
    while (val[vl] && vl + 1u < max) vl++;
    while (name[nl]) nl++;
    for (uint32_t i = 1; i < nl; i++) if (name[i] == '(' && name[i - 1u] == ' ') { nl = i - 1u; break; }   /* "Sample Attack (In)" → ohne Klammer */
    uint32_t room = max - 1u - vl;
    if (room) room--;                                    /* Leerzeichen */
    if (nl > room) {                                     /* zu lang: letztes Wort ("Sample Attack" → "Attack") */
        uint32_t sp = nl;
        while (sp && name[sp - 1u] != ' ') sp--;
        name += sp; nl -= sp;
    }
    if (nl > room) nl = vl ? 0u : room;                  /* passt auch das nicht: nur der Wert, kein abgeschnittener Name */
    for (uint32_t i = 0; i < nl; i++) d[n++] = name[i];
    if (nl && vl) d[n++] = ' ';
    for (uint32_t i = 0; i < vl; i++) d[n++] = val[i];
    d[n] = 0;
}

/* Slice-Zahl: Menuewerte 1…8 (Slicer 4…64), klemmt an den Enden */
int32_t sv_step_mode(int32_t mode, int dir) {
    if (mode < 1 || mode > 8) return mode;
    int32_t m = mode + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
    return m < 1 ? 1 : m > SL_MAXSLMODE ? SL_MAXSLMODE : m;
}
/* Naechstes Sample: cls[255] 0 = leer, 1 = Sample, 2 = Live Recordings. Reihenfolge: Live Recordings, neueste zuerst, danach
 * der Rest. cur nicht in der Liste (-1) → Anfang der Reihenfolge. Rueckgabe -1 = keine Samples. */
int32_t sv_next_sample(const uint8_t *cls, int32_t cur, int dir) {
    uint8_t ord[255]; uint32_t n = 0;
    for (int32_t i = 254; i >= 0; i--) if (cls[i] == 2u) ord[n++] = (uint8_t)i;
    for (int32_t i = 0; i < 255; i++) if (cls[i] == 1u) ord[n++] = (uint8_t)i;
    if (!n) return -1;
    for (uint32_t p = 0; p < n; p++)
        if ((int32_t)ord[p] == cur) return ord[(p + n + (dir > 0 ? 1u : n - 1u)) % n];
    return ord[0];
}
/* engines.cfg: 0 g, 1 a, 2 d, 3 o, 4 p */
int32_t sv_cfg_get(uint32_t t, uint32_t i) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || t >= 8u) return 0;
    switch (i) {
    case 3: return v->off[t]; case 4: return v->sample[t];   /* 0, 1, 2, 5 (g, a, d, n aus 029–032): alte Zeilen werden ignoriert */
    case 6: return sv_wid(t); case 7: return sv_beat_on(t);
    case 8: { sv_x_t *e = sv_xget(0); return e ? e->fine[t] : 0; }
    case 10: { uint32_t w_ = gr_cfg(t)->n; return w_ == 4u ? 0 : (int32_t)w_; }
    case 11: return gr_cfg(t)->win == 50u ? 0 : (int32_t)gr_cfg(t)->win + 1;
    case 12: return gr_cfg(t)->spray;
    case 13: return gr_cfg(t)->pspray;
    case 14: return gr_cfg(t)->scan;
    case 15: return gr_cfg(t)->lrate == 12u ? 0 : (int32_t)gr_cfg(t)->lrate + 1;
    case 16: return gr_cfg(t)->ldepth;
    case 17: return gr_cfg(t)->ltgt;
    case 18: return gr_cfg(t)->pmode;
    case 19: return gr_cfg(t)->speed == 100u ? 0 : gr_cfg(t)->speed;
    case 20: return gr_cfg(t)->rate == 10u ? 0 : gr_cfg(t)->rate;
    case 9: { sv_x_t *e = sv_xget(0); return e ? e->gmode[t] : 0; }
    case 21: { sv_x_t *e = sv_xget(0); return e ? e->nudge[t] : 0; }   /* 045 */
    default: return 0;
    }
}
void sv_cfg_set(uint32_t t, uint32_t i, int32_t x) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || t >= 8u) return;
    if (i == 3u && x >= -63 && x <= 63) v->off[t] = (int8_t)x;
    else if (i == 4u && x >= 0 && x <= 255) v->sample[t] = (int16_t)x;
    else if (i == 6u && x >= -512 && x <= 1024) { sv_x_t *e = sv_xget(x != 0); if (e) { e->wid[t] = (int16_t)x; e->gen[t]++; } }
    else if (i == 7u && x >= 0 && x <= 1) { sv_x_t *e = sv_xget(x != 0); if (e) { e->smode[t] = (uint8_t)x; e->gen[t]++; } }
    else if (i == 8u && x >= 0 && x <= 16) { sv_x_t *e = sv_xget(x != 0); if (e) { e->fine[t] = (uint8_t)x; e->gen[t]++; } }
    else if (i == 9u && x >= 0 && x <= 8) { sv_x_t *e = sv_xget(x != 0); if (e) e->gmode[t] = (uint8_t)x; }
    else if (i == 21u && x >= -50 && x <= 50) { sv_x_t *e = sv_xget(x != 0); if (e) { e->nudge[t] = (int8_t)x; e->gen[t]++; } }   /* 045 */
    else if (i == 10u && x >= 1 && x <= 8) gr_cfg(t)->n = (uint8_t)x;
    else if (i == 11u && x >= 1 && x <= 101) gr_cfg(t)->win = (uint8_t)(x - 1);
    else if (i == 12u && x >= 0 && x <= 100) gr_cfg(t)->spray = (uint8_t)x;
    else if (i == 13u && x >= 0 && x <= 24) gr_cfg(t)->pspray = (uint8_t)x;
    else if (i == 14u && x >= -100 && x <= 100) gr_cfg(t)->scan = (int8_t)x;
    else if (i == 15u && x >= 1 && x <= 32) gr_cfg(t)->lrate = (uint8_t)(x - 1);
    else if (i == 16u && x >= 0 && x <= 100) gr_cfg(t)->ldepth = (uint8_t)x;
    else if (i == 17u && x >= 0 && x <= 3) gr_cfg(t)->ltgt = (uint8_t)x;
    else if (i == 18u && x >= 0 && x <= 3) gr_cfg(t)->pmode = (uint8_t)x;
    else if (i == 19u && x >= 5 && x <= 200) gr_cfg(t)->speed = (uint8_t)x;
    else if (i == 20u && x >= 10 && x <= 100) gr_cfg(t)->rate = (uint8_t)x;
}

#ifdef HOST_TEST
int sv_steps_held_now(void) { return 0; }
#endif
#ifndef HOST_TEST
static uint32_t put_u(char *b, uint32_t n, uint32_t v) {
    char t[10]; uint32_t k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 10u);
    while (k) b[n++] = t[--k];
    return n;
}
static uint32_t put_s(char *b, uint32_t n, const char *s) { while (*s) b[n++] = *s++; return n; }

static uint32_t cur_track(void) {
    uint32_t m = SEL_MASK & 0xFFu;
    if (m && !(m & (m - 1u))) {                         /* genau eine Spur gewaehlt → merken */
        uint32_t t = 0; while (!(m & 1u)) { m >>= 1; t++; }
        SV->last_t = t + 1u;
        return t;
    }
    return SV->last_t ? SV->last_t - 1u : 8u;           /* sonst die zuletzt gewaehlte (Steps setzen loescht die Auswahl) */
}
static int kb_active(uint32_t ed) { return *(volatile uint8_t *)(ed + 0x18u) != 0; }
/* gehaltene Grid-Pads: Editor +0x88…+0x97, Bit (Step + Spur*16), gesetzt beim Druecken, geloescht beim Loslassen
 * (FUN_00039c10). Frueher hier Editor+0x3C/+0x40 – das ist aber der Vektor der gehaltenen TASTATUR-Noten
 * (Tastatur Editor+0x14, +0x28), deshalb wurde in 026 nie auf Steps geschrieben. */
static uint32_t steps_held(void) {
    uint32_t ed = EDITOR;
    if (ed < 0x1FFF0000u || ed >= 0x20030000u) return 0;
    volatile uint32_t *h = (volatile uint32_t *)(ed + 0x88u);
    return (h[0] | h[1] | h[2] | h[3]) != 0u;
}
/* Slice k auf die gewaehlten (= gehaltenen) Steps legen: Sample Start ganz runter, dann k*64+32 (Slice = Wert>>6) */
int sv_steps_held_now(void) { return steps_held(); }
static void set_slice(uint32_t ed, uint32_t k) {
    ORIG_CHG(ed, 10, -32767);
    if (k) ORIG_CHG(ed, 10, (int)(k * 64u + 32u));
}

#define NOW()       (*(const volatile uint32_t *)0x20000D7Cu)
#define FOC_MS      0x3FFFFFFFu   /* 045: Fokuszeile bleibt (bis ein anderer Knob sie ersetzt); vorher 3000 ms */
#define FW_MS       0x3FFFFFFFu   /* 045: Master/Tempo/Random bleiben, bis ein anderer Knob kommt (vorher 1,5 s) */
static int until(uint32_t t) { return (int32_t)(t - NOW()) > 0; }
/* Fokus der zwei linken Tasten wie SeqCtl_knobTurn: FUN_0001ff7c(seq) != 0 → unterer Parameter */
static int alt_focus(void) {
    uint32_t ui = SV->ui;
    if (ui < 0x1FFF0000u || ui >= 0x20030000u || (ui & 3u)) return 0;
    uint32_t seq = *(volatile uint32_t *)(ui + 0xCu);
    if (seq < 0x1FFF0000u || seq >= 0x20030000u) return 0;
    return ((uint32_t (*)(uint32_t))0x0001FF7Du)(seq) != 0u;
}
/* 045: Menue offen (aktiver Bildschirm (ui+8)+0x2FC = Menue-vtable 0xA97E0)? Dann ruht die eingerastete Slicer-Seite,
 * sonst blendet sie das Menue aus (Bildmitte) und die Knobs gehen ins unsichtbare Menue. */
int sv_menu_on(void) {
#ifdef HOST_TEST
    return 0;
#else
    uint32_t ui = SV->ui;
    if (ui < 0x1FFF0000u || ui >= 0x20030000u) return 0;
    uint32_t sm = *(volatile uint32_t *)(ui + 8u);
    if (sm < 0x1FFF0000u || sm >= 0x20030000u) return 0;
    uint32_t scr = *(volatile uint32_t *)(sm + 0x2FCu);
    return scr >= 0x1FFF0000u && scr < 0x20030000u && *(volatile uint32_t *)scr == 0x000A97E0u;
#endif
}
uint32_t sv_page_track(void) {
    volatile sv_t *v = SV; volatile sl_t *s = SL;
    if (v->magic != SV_MAGIC || s->magic != SL_MAGIC) return 8u;
    if (sv_menu_on()) return 8u;
    uint32_t ed = EDITOR;
    if (ed < 0x1FFF0000u || ed >= 0x20030000u || *(volatile uint32_t *)ed != ED_VT || !kb_active(ed)) return 8u;
    uint32_t t = cur_track();
    return (t < 8u && (sl_slices(s->mode[t]) || sl_is_granular(s->mode[t]))) ? t : 8u;
}
/* Wert des Standard-Steps mit dem Formatter der Firmware (Tabelle 0x1FFF7918: Eintrag +0x10), sonst der Text der Seite */
static void tmpl_text(uint32_t idx, char *d, uint32_t max, const volatile char *fallback) {
    uint32_t obj = idx < 22u ? ((const volatile uint32_t *)(0x1FFF7918u + idx * 0x20u))[4] : 0u;
    if (obj >= 0x1FFF0000u && obj < 0x20030000u) {
        const uint32_t *vt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)obj;
        d[0] = 0;
        int32_t x = wv_touched_param(idx);
        if (x == -32768) x = *(const volatile int16_t *)(0x1FFF7918u + idx * 0x20u);   /* nicht gesetzt: Standardwert der Tabelle */
        ((void (*)(uint32_t, int32_t, int32_t, char *, uint32_t, void *))vt[0])(obj, x, 0, d, max, 0);
        d[max - 1u] = 0;
        return;
    }
    uint32_t i = 0;
    for (; i + 1u < max && fallback[i]; i++) d[i] = fallback[i];
    d[i] = 0;
}

/* Aus wv_view_hook: Step-Seite der Firmware in View. Fremde Parameter und gehaltene Steps kommen in die Fokuszeile; Regler mit
 * eigener Anzeige ohne gehaltenen Step nicht (die Seite zeigt dann nur den Standard-Step). */
void sv_capture(uint32_t i0, uint32_t i1, const volatile char *n0, const volatile char *v0, const volatile char *n1, const volatile char *v1, uint32_t sel) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC) return;
    if (!steps_held() && (sv_own_param(i0) || sv_own_param(i1))) return;   /* Sample Start/End, Sample/Folder, Move: eigene Anzeige */
    { uint32_t tc = sv_page_track(); if (!steps_held() && tc < 8u && sl_is_granular(SL->mode[tc]) && (i0 == 16u || i1 == 16u || i0 == 18u || i1 == 18u)) return; }
    char a[18], b[18];
    if (steps_held()) { sv_join(a, sizeof a, n0, v0); sv_join(b, sizeof b, n1, v1); }
    else {                                               /* 035: ohne gehaltenen Step gilt der Standard-Step. Die Firmware zeigt bei gewaehlter */
        char t0[24], t1[24];                             /* Spur sonst "~" bzw. "+x" (Mehrfachauswahl), auch beim Beruehren und bei den linken Tasten */
        tmpl_text(i0, t0, sizeof t0, v0); tmpl_text(i1, t1, sizeof t1, v1);
        sv_join(a, sizeof a, n0, t0); sv_join(b, sizeof b, n1, t1);
    }
    for (uint32_t i = 0; i < 18u; i++) { v->cap0[i] = a[i]; v->cap1[i] = b[i]; if (!a[i] && !b[i]) break; }
    v->cap0[17] = 0; v->cap1[17] = 0;
    v->cap_sel = sel ? 1u : 0u;
    v->cap_until = NOW() + FOC_MS;
    v->knob = 0;
}
static uint32_t pct_put(char *b, uint32_t n, uint32_t p) { n = put_u(b, n, p); b[n++] = '%'; return n; }

/* 031: Beat-Erkennung in Stuecken, unabhaengig von der Ansicht; bei anderem Sample oder anderer Slice-Zahl neu */
static void scan_poll(void) {
    volatile sl_t *s = SL;
    sv_x_t *x = sv_xget(0);
    if (!x) return;
    if (x->sc_t < 8u) { uint32_t st_ = x->sc_t; if (sv_scan_run(x, 32u)) x->gen[st_]++; }
    for (uint32_t t = 0; t < 8u; t++) {
        uint32_t n = sl_slices(s->mode[t]);
        if (!x->smode[t] || !n) continue;
        sv_beat_t *b = &x->b[t];
        uint32_t d = 0, fr = 0, rate = 44100u;
        if (!wv_sample_data(t, &d, &fr, &rate)) continue;
        if (x->sc_t == t) continue;
        if (b->data != d || b->frames != fr) { if (x->sc_t >= 8u) { sv_scan_start(x, t, d, fr, rate); x->gen[t]++; } continue; }
        if (b->done && b->nfor != sv_beat_level(t)) { sv_beat_pick(b, sv_beat_level(t)); x->gen[t]++; }
    }
}

/* 039: Seite Granular. Rueckgabe Fokus; Texte in a/b, top */
static const char *const DIRN[6] = { "Forward", "Reverse", "Ping-Pong", "Random", "Scan Bounce", "Random Walk" };
static const char *const TGTN[4] = { "Position", "Spray", "Size", "Pitch" };
static uint32_t put_hz(char *b, uint32_t j, uint32_t mhz) {
    j = put_u(b, j, mhz / 1000u);
    if (mhz < 10000u) { b[j++] = '.'; j = put_u(b, j, (mhz % 1000u) / 100u); if (mhz < 1000u && mhz / 10u % 10u) j = put_u(b, j, mhz / 10u % 10u); }
    return put_s(b, j, " Hz");
}
static uint32_t gr_texts(uint32_t t, uint32_t kn, int alt, char *top, char *a, char *b) {
    volatile sl_t *s = SL;
    const gr_cfg_t *c = gr_cfg(t);
    int32_t pv = wv_touched_param(10u), sv_ = wv_touched_param(11u);
    if (pv == -32768) pv = 0;
    if (sv_ == -32768) sv_ = 0x7FFF;
    uint32_t pp = ((uint32_t)pv * 100u + 0x3FFFu) / 0x7FFFu, ms = gr_grain_ms(sv_), j;
    j = put_s(top, 0, "Pos "); j = put_u(top, j, pp); top[j++] = '%'; top[j] = 0;
    uint32_t foc = alt ? 4u : 3u;
    if (kn == 8u) {
        j = put_s(a, 0, "Position "); j = put_u(a, j, pp); a[j++] = '%'; a[j] = 0;
        j = put_s(b, 0, "Size "); j = put_u(b, j, ms); b[put_s(b, j, " ms")] = 0;
    } else if (kn == 11u) {
        j = put_s(a, 0, "Grains "); a[put_u(a, j, c->n)] = 0;
        b[put_s(b, 0, DIRN[c->pmode ? c->pmode + 2u : (uint32_t)(s->mode[t] - SL_GRAN) & 3u])] = 0;
    } else if (kn == 15u) {
        int32_t o = c->scan;
        j = put_s(a, 0, "Scan "); if (o) a[j++] = o < 0 ? '-' : '+'; j = put_u(a, j, (uint32_t)(o < 0 ? -o : o)); a[j++] = '%'; a[j] = 0;
        j = put_s(b, 0, "Spray "); j = put_u(b, j, c->spray); b[j++] = '%'; b[j] = 0;
    } else if (kn == 14u) {
        j = put_s(a, 0, "Window "); j = put_u(a, j, c->win); a[j++] = '%'; a[j] = 0;
        j = put_s(b, 0, "Pitch Spray "); b[put_u(b, j, c->pspray)] = 0;
    } else if (kn == 5u) {
        j = put_s(a, 0, "LFO "); j = put_hz(a, j, gr_lfo_mhz(c->lrate)); a[j] = 0;
        j = put_s(b, 0, TGTN[c->ltgt & 3u]); b[j++] = ' '; j = put_u(b, j, c->ldepth); b[j++] = '%'; b[j] = 0;
    } else if (kn == 3u) {
        sample_focus(t, a, b);                            /* 045: oben "Sample 3/40" (Name steht schon oben links), unten der Ordnername */
    } else if (kn == 10u) {                               /* 044: Knob Random: Speed / Rate */
        j = put_s(a, 0, "Speed "); j = put_u(a, j, c->speed); a[j++] = '%'; a[j] = 0;
        j = put_s(b, 0, "Rate "); j = put_u(b, j, (uint32_t)c->rate * 10u); b[j++] = '%'; b[j] = 0;
    } else {
        j = put_u(a, 0, c->n); a[put_s(a, j, " Grains")] = 0;
        j = put_s(b, 0, "Size "); j = put_u(b, j, ms); b[put_s(b, j, " ms")] = 0;
        foc = 0u;
    }
    return foc;
}

#ifndef HOST_TEST
/* 045: Allgemeine Slicer-Einstellung schuetzen (Nutzer 09.11.: "RIESIGER Bug"). Die Firmware fuellt beim Beruehren eines Steps
 * die Vorlage (0x1FFF450C+0x7C) mit dessen Werten; auf Slicer-Spuren ist die Vorlage aber die Einstellung fuer das ganze Sample.
 * Solange kein Step gehalten wird: Kopie der Vorlage. Nach dem Loslassen eines Steps DIESER Spur: Kopie zurueck. Undo half nicht. */
typedef struct { uint32_t magic, t, held, htr; uint8_t d[44]; } tpl_t;
#define TPL ((volatile tpl_t *)0x1FFFB800u)
_Static_assert(0x1FFFB800u + sizeof(tpl_t) <= 0x1FFFB900u, "tpl_t vor NIS");
#define TPL_MAGIC 0x7E1A0450u
#define STEP_BYTES 42u
static uint32_t held_tracks(void) {
    uint32_t ed = EDITOR, m = 0;
    if (ed < 0x1FFF0000u || ed >= 0x20030000u) return 0;
    volatile uint32_t *h = (volatile uint32_t *)(ed + 0x88u);
    for (uint32_t i = 0; i < 4u; i++) { if (h[i] & 0xFFFFu) m |= 1u << (2u * i); if (h[i] >> 16) m |= 1u << (2u * i + 1u); }
    return m;
}
static void tpl_poll(void) {
    volatile tpl_t *p = TPL;
    uint32_t dst = *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu);
    if (!step_ok(dst)) return;
    uint32_t t = cur_track();
    int slicer = t < 8u && sl_slices(SL->mode[t]);
    if (steps_held()) {
        if (!p->held) { p->held = 1; p->htr = 0; }
        p->htr |= held_tracks();
        return;
    }
    if (p->held) {
        p->held = 0;
        if (p->magic == TPL_MAGIC && p->t == t && slicer && ((p->htr >> t) & 1u))
            for (uint32_t i = 0; i < STEP_BYTES; i++) *(volatile uint8_t *)(dst + i) = p->d[i];
        return;
    }
    if (!slicer) { p->magic = 0; return; }
    for (uint32_t i = 0; i < STEP_BYTES; i++) p->d[i] = *(volatile uint8_t *)(dst + i);
    p->t = t; p->magic = TPL_MAGIC;
}
#endif

/* Hauptschleife (sl_poll): in View auf einer Slicer-Spur die Waveform zeigen (wave.c) */
void sv_poll(void) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    if (v->magic != SV_MAGIC || s->magic != SL_MAGIC) return;
#ifndef HOST_TEST
    tpl_poll();                                          /* 045: Vorlage der Slicer-Spur gegen Ueberschreiben durch Step-Beruehren schuetzen */
#endif
    scan_poll();
    uint32_t t = sv_page_track();
    if (t >= 8u || until(v->fw_until)) { wv_kb(0, 0, 0, 0, "", "", "", 0); return; }
    if (sl_is_granular(s->mode[t])) {                     /* 039 */
        char gt_[24], ga[24], gb[24]; uint32_t kg = v->knob, jg;
        if (steps_held() || !kg || !until(v->foc_until)) kg = 0;
        uint32_t fg = gr_texts(t, kg, alt_focus(), gt_, ga, gb);
        if (!kg && until(v->cap_until)) {
            for (jg = 0; jg < 18u; jg++) { ga[jg] = v->cap0[jg]; gb[jg] = v->cap1[jg]; }
            fg = (alt_focus() || v->cap_sel) ? 4u : 3u;
        }
        int32_t kk = v->sel[t] ? (int32_t)v->sel[t] - 1 : -1;
        wv_kb(1, t, kk, 0x7FFF, gt_, ga, gb, fg);
        return;
    }
    uint32_t n = sl_slices(s->mode[t]);
    int held = steps_held();
    uint32_t nshow = sv_beat_on(t) ? sv_beat_count(t) : n;
    int32_t k = v->sel[t] ? (int32_t)v->sel[t] - 1 : -1;
    if (held) k = wv_touched_slice(n);                   /* gehaltener Step: dessen Slice zeigen */
    if (k >= (int32_t)n) k = -1;
    int32_t gate = wv_touched_gate();                    /* gehaltener Step oder Vorlage fuer neue Steps */
    uint32_t gp = ((uint32_t)gate * 100u + 0x3FFFu) / 0x7FFFu;
    char top[24], a[24], b[24]; uint32_t j;
    uint32_t nfit = sv_beat_on(t) ? nshow : sv_fit_count(t, n);   /* 045: Grid breiter als Fit → hintere Slices fehlen: "2/3 (4)" */
    if (k >= 0) { j = put_s(top, 0, "Slice "); j = put_u(top, j, (uint32_t)k + 1u); j = put_s(top, j, "/"); j = put_u(top, j, nfit < nshow ? nfit : nshow); }
    else { j = put_s(top, 0, "Slices "); j = put_u(top, j, nfit < nshow ? nfit : nshow); }
    if (nfit < nshow) { j = put_s(top, j, " ("); j = put_u(top, j, nshow); j = put_s(top, j, ")"); }
    top[j] = 0;
    uint32_t foc = 0;
    int alt = alt_focus();
    uint32_t kn = v->knob;
    if (held || !kn || !until(v->foc_until)) kn = 0;
    if (0) {
    } else if (kn == 8u) {                               /* Sample Start / Sample End */
        for (j = 0; j < 23u && top[j]; j++) a[j] = top[j];
        a[j] = 0;
        sv_x_t *xh_ = sv_xget(0);
        uint32_t ph_ = (xh_ && v->kmode[t] != 1u && xh_->hold[t] && xh_->hold[t] <= n) ? xh_->hold[t] : 0u;
        if (!held && !ph_) {                              /* 17.11.: ohne Halten = Feinversatz der Vorlage: "Start +5%" */
            int32_t pv_ = wv_touched_param(10u), pc_ = pv_ == -32768 ? 0 : sl_fine(pv_) * 100 / 64;
            j = put_s(a, 0, "Start "); a[j++] = pc_ < 0 ? '-' : '+'; j = put_u(a, j, (uint32_t)(pc_ < 0 ? -pc_ : pc_)); a[put_s(a, j, "%")] = 0;
        } else if (!held) {                               /* 13.11./17.11.: Slice-Pad gehalten = Schnitt: "Cut 3 +12 ms" */
            k = (int32_t)ph_ - 1;
            int32_t cm = sv_cut_ms(t, (uint32_t)k, n);
            j = put_s(a, 0, "Cut "); j = put_u(a, j, (uint32_t)k + 1u); a[j++] = ' '; a[j++] = cm < 0 ? '-' : '+';
            j = put_u(a, j, (uint32_t)(cm < 0 ? -cm : cm)); a[put_s(a, j, " ms")] = 0;
        }
        j = put_s(b, 0, "Gate "); b[pct_put(b, j, gp)] = 0;
        foc = alt ? 4u : 3u;
    } else if (kn == 11u) {                              /* Track Length / Play Mode */
        if (sv_beat_on(t)) {                              /* Beat: Genauigkeit statt Anzahl */
            j = put_s(a, 0, "Sensitivity "); j = put_u(a, j, sv_beat_level(t)); a[put_s(a, j, "/16")] = 0;
            if (sv_beat_count(t)) { j = put_s(b, 0, "Beat "); j = put_u(b, j, sv_beat_count(t)); b[j] = 0; }
            else b[put_s(b, 0, "Beat ...")] = 0;
        } else {
            j = put_u(a, 0, n); a[put_s(a, j, " Slices")] = 0;
            b[put_s(b, 0, "Grid")] = 0;
        }
        foc = alt ? 4u : 3u;
    } else if (kn == 15u) {                              /* Move / Micromove */
        int32_t o = sv_off(t);
        if (sv_beat_on(t)) {                              /* 045: Move bei Beat = Nudge aller Beat-Grenzen in ms */
            sv_x_t *xn = sv_xget(0); int32_t nd = xn ? xn->nudge[t] : 0;
            j = put_s(a, 0, "Nudge "); a[j++] = nd < 0 ? '-' : '+'; j = put_u(a, j, (uint32_t)(nd < 0 ? -nd : nd)); a[put_s(a, j, " ms")] = 0;
        }
        else { j = put_s(a, 0, "Offset "); a[j++] = o < 0 ? '-' : '+'; j = put_u(a, j, wv_off_ms(t, n)); a[put_s(a, j, " ms")] = 0; }
        if (sv_beat_on(t)) b[put_s(b, 0, "Beat")] = 0;
        else {
            uint32_t bp = wv_bpm10(t, n);
            if (!sv_wid(t)) b[put_s(b, 0, "Grid Fit")] = 0;   /* 045 */
            else { j = put_u(b, 0, bp / 10u); b[j++] = '.'; j = put_u(b, j, bp % 10u); b[put_s(b, j, " BPM")] = 0; }
        }
        foc = alt ? 4u : 3u;
    } else if (kn == 3u) {                               /* Sample / Folder */
        sample_focus(t, a, b);                            /* 045: oben "Sample 3/40" (Name steht schon oben links), unten der Ordnername */
        foc = alt ? 4u : 3u;
    } else if (until(v->cap_until)) {                    /* Fremdparameter / gehaltener Step: Texte der Firmware-Seite */
        for (j = 0; j < 18u; j++) { a[j] = v->cap0[j]; b[j] = v->cap1[j]; }
        foc = (alt_focus() || v->cap_sel) ? 4u : 3u;
    } else {                                             /* Ruhe: Slice-Zahl + Versatz, Gate */
        j = put_u(a, 0, nshow); j = put_s(a, j, " Slices");
        if (sv_beat_on(t) && !sv_beat_count(t)) { j = put_s(a, 0, "Beat ..."); }
        else if (sv_off(t) && !sv_beat_on(t)) { int32_t o = sv_off(t); j = put_s(a, j, o < 0 ? "  -" : "  +"); j = put_u(a, j, wv_off_ms(t, n)); j = put_s(a, j, " ms"); }
        a[j] = 0;
        j = put_s(b, 0, "Gate "); b[pct_put(b, j, gp)] = 0;
    }
    wv_kb(1, t, k, gate, top, a, b, foc);
}

/* 0x3B77E: b.w FUN_0003a1a8(editor, reihe, spalte, ereignis) → hier */
/* Slice anspielen (Slices: Note des Steps, 045 Keys: Note des Pads). Stimme direkt wie AudioStep_play; unbekannt → Tastatur der Firmware. */
static void play_slice(uint32_t ed, uint32_t t, uint32_t idx, uint32_t n, uint32_t note) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    uint32_t kbo = ed + 0x14u;
        {                                                 /* vorhoeren (auch beim Schreiben, wie das Original) */
            int32_t st, en;
            sl_bounds(t, idx, n, wv_touched_gate(), &st, &en);
            /* 035: Stimme aus dem Spurobjekt (Pattern + 0x10 + t·0xBA8, +0x88 = Kontext {Knoten, Mixer, Kanal}, am Geraet von Anfang an
             * gesetzt, in beiden Patterns gleich). Vorher nur aus sl_begin bekannt: bis zum ersten Abspielen klang das ganze Sample. */
            uint32_t node = sl_track_ctx(t) ? (uint32_t)sl_track_ctx(t)[0] : 0u;
            if (node) s->node[t] = node;                  /* auch fuer gr_update (Knackschutz beim Vorhoeren) */
            if (node >= 0x1FFF0000u && node < 0x20030000u && *(volatile uint32_t *)node >= 0x9A000u && *(volatile uint32_t *)node < 0xAE000u) {
                /* Stimme direkt ausloesen wie AudioStep_play (Knoten vt+0x10 = TrackNode_trigger), Grenzen vorher in die Slots */
                void *a = NODE_SLOT((void *)(uintptr_t)node, 8u), *b = NODE_SLOT((void *)(uintptr_t)node, 9u);
                if (a) SLOT_SET(a, 1u, st);
                if (b) SLOT_SET(b, 1u, en);
                {                                         /* Sample der Spur, sonst das ihres ersten Steps */
                    int32_t smp = v->sample[t] ? (int32_t)v->sample[t] - 1 : wv_track_sample(t);
                    void *c = NODE_SLOT((void *)(uintptr_t)node, 7u);
                    if (c && smp >= 0) SLOT_SET(c, 1u, smp);
                }
                sl_preview_params(t);                     /* 032: Vorlage (Standard-Step) hoerbar machen */
                const uint32_t *nvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)node;
                ((void (*)(uint32_t, uint32_t))nvt[4])(node, (uint32_t)note);
                v->aud_node = node;
            } else {                                      /* Stimme noch unbekannt (Spur seit dem Start nicht gespielt) */
                v->aud[t] = (uint8_t)(idx + 1u);
                KB_PLAY(kbo, note, 100u, 1);
                UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
            }
        }
}

/* 045: Note eines Tastatur-Pads wie die Firmware (FUN_0002301c(aus, tastatur, reihe, spalte) → aus[1], -1 = keine) */
static int32_t kb_note(uint32_t kbo, uint32_t row, uint32_t col) {
    int32_t o[4] = { 0, -1, 0, 0 };
    ((void (*)(int32_t *, uint32_t, uint32_t, uint32_t))0x0002301Du)(o, kbo, row, col);
    return o[1];
}

/* 17.11.: Live Rec auf Slicer-Spuren. Die Firmware nimmt Anschlaege der Tastatur auf (Note + Werte der Vorlage) und spielt sie.
 * Vorher kommt der Slice in die Vorlage (Sample Start, Feinversatz bleibt). Slices-Modus: der Anschlag geht als die Taste
 * an die Firmware, deren Ton die Tonhoehe der Vorlage ist (sonst haette jedes Pad eine andere Tonhoehe); keine passende
 * Taste → das Pad selbst. Keys-Modus: die echte Taste (Tonhoehe gewollt). Klang kommt von der Firmware (sl_param_hook, aud). */
#define TPL_STEP() (*(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu))
static void live_slice(uint32_t t, uint32_t slice) {
    int32_t cur = step_get(TPL_STEP(), 10u);
    if (cur < 0) cur = 32;
    step_set(TPL_STEP(), 10u, (int32_t)(slice * 64u) + (cur & 63));
    SV->aud[t] = (uint8_t)(slice + 1u);
}
static void live_key(uint32_t ed, uint32_t row, uint32_t col, uint32_t ev) {
    int32_t want = step_get(TPL_STEP(), 6u);
    for (uint32_t r = 1u; want >= 0 && r <= 7u; r++)
        for (uint32_t c = 0; c <= 3u; c++)
            if (kb_note(ed + 0x14u, r, c) == want) { ORIG_KB(ed, r, c, ev); return; }
    ORIG_KB(ed, row, col, ev);
}

__attribute__((used, section(".text.entry_svkb")))
void sv_kb_hook(uint32_t ed, uint32_t row, uint32_t col, uint32_t ev) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    uint32_t t = cur_track();
    uint32_t n = (v->magic == SV_MAGIC && s->magic == SL_MAGIC && t < 8u) ? sl_slices(s->mode[t]) : 0u;
    if (v->magic == SV_MAGIC && s->magic == SL_MAGIC && t < 8u && sl_is_granular(s->mode[t]) && row == 0u) {
        /* 040: Gruene Pads auf Granular: 3 = Drone an/aus (aus: Wolke klingt mit dem Decay aus). Die Sample-Plaetze wirken hier ohnehin nicht. */
        if (ev == 0u && col == 2u) {
            gr_cfg_t *c = gr_cfg(t); gr_t *g = gr_state(t);
            c->drone = c->drone ? 0u : 1u;
            if (!c->drone && g) { g->hold = 0u; g->rel2 = 1u; }
            v->aud_node = 0;
            UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
        }
        return;
    }
    if (v->magic == SV_MAGIC && s->magic == SL_MAGIC && t < 8u && sl_is_granular(s->mode[t]) && row >= 1u && row <= 7u && col <= 3u) {
        /* 039: Pads = Position. Halten = Wolke klingt, Loslassen = Decay. Gehaltene Steps bekommen die Position. */
        uint32_t idx = (row - 1u) * 4u + col;
        gr_t *g = gr_state(t);
        const gr_cfg_t *cg = gr_cfg(t);
        if (cg->drone && ev == 1u) return;                /* Drone: Loslassen aendert nichts */
        if (cg->drone && ev == 0u && g && v->aud_node) {
            int *cd = sl_track_ctx(t);
            uint32_t nd = cd ? (uint32_t)cd[0] : 0u;
            const volatile uint8_t *st_ = nd ? (const volatile uint8_t *)(nd + 0xD8u) : 0;
            if (st_ && st_[0x18] && v->sel[t] == idx + 1u) {   /* dasselbe Pad: beenden */
                g->hold = 0u; g->rel2 = 1u; v->aud_node = 0;
                UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
                return;
            }
            if (st_ && st_[0x18]) {                        /* anderes Pad: Position wandert, kein Neustart */
                int32_t val2 = (int32_t)(idx * 0x7FFFu / (PADS - 1u));
                void *a2 = NODE_SLOT((void *)(uintptr_t)nd, 8u);
                if (a2) SLOT_SET(a2, 1u, val2);
                v->sel[t] = (uint8_t)(idx + 1u);
                uint32_t sel2 = *(volatile uint32_t *)(ed + 8u), m2 = SEL_MASK;
                SEL_MASK = 0u;
                ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel2, 10, -32767);
                if (val2) ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel2, 10, val2);
                SEL_MASK = m2;
                UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
                return;
            }
        }
        if (ev == 0u) {
            int32_t val = (int32_t)(idx * 0x7FFFu / (PADS - 1u));
            v->sel[t] = (uint8_t)(idx + 1u);
            if (steps_held()) { ORIG_CHG(ed, 10, -32767); if (val) ORIG_CHG(ed, 10, val); }
            else {                                        /* Vorlage: neue Steps uebernehmen die Position */
                uint32_t sel_ = *(volatile uint32_t *)(ed + 8u), m_ = SEL_MASK;
                SEL_MASK = 0u;
                ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel_, 10, -32767);
                if (val) ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel_, 10, val);
                SEL_MASK = m_;
            }
            int *c_ = sl_track_ctx(t);
            uint32_t node = c_ ? (uint32_t)c_[0] : 0u;
            if (node) {
                s->node[t] = node;
                void *a = NODE_SLOT((void *)(uintptr_t)node, 8u), *c = NODE_SLOT((void *)(uintptr_t)node, 7u);
                int32_t smp = v->sample[t] ? (int32_t)v->sample[t] - 1 : wv_track_sample(t);
                if (a) SLOT_SET(a, 1u, val);
                if (c && smp >= 0) SLOT_SET(c, 1u, smp);
                sl_preview_params(t);
                if (g) { g->hold = 1u; g->rel2 = 0u; }
                const uint32_t *nvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)node;
                ((void (*)(uint32_t, uint32_t))nvt[4])(node, (uint32_t)wv_touched_note());
                v->aud_node = node;
            }
            UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
        } else if (ev == 1u) {
            if (g) { g->hold = 0u; g->rel2 = 1u; }
            v->aud_node = 0;
        }
        return;
    }
    if (n && row == 0u && col <= 3u) {                   /* 045: gruene Pads = Modus der gelben Pads: 1 Slices, 2 Keys (3/4 frei) */
        if (ev == 0u && col <= 1u && v->kmode[t] != col) {   /* 13.11.: Pad 3 kein eigener Modus mehr (Sample Start schneidet immer) */
            v->kmode[t] = (uint8_t)col;
            TOAST(col ? "Keys: pitch selected slice" : "Slices");
            UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
        }
        return;
    }
    if (n && v->kmode[t] == 1u && row >= 1u && row <= 7u && col <= 3u) {
        /* 045 Keys: Pads gehen an die Original-Tastatur (Tonhoehe, Live Rec, Note auf gehaltene Steps). Gespielt wird der
         * gewaehlte Slice: sl_param_hook nimmt ihn beim naechsten Trigger der Spur (Vorhoeren, wie 026). */
        uint32_t live = *(volatile uint8_t *)(ed + 0x113u);
        if (ev == 0u && !steps_held()) {                  /* 045: ohne gehaltenen Step: gewaehlten Slice transponiert anspielen */
            int32_t nt = kb_note(ed + 0x14u, row, col);
            if (nt >= 0) {
                step_set(TPL_STEP(), 6u, nt);             /* 17.11.: zuletzt gespielte Tonhoehe in die Vorlage (neue Steps), wie die Original-Tastatur */
                if (!live) play_slice(ed, t, v->sel[t] ? v->sel[t] - 1u : 0u, n, (uint32_t)nt);
            }
            if (!live) return;
            live_slice(t, v->sel[t] ? v->sel[t] - 1u : 0u);   /* 17.11.: Live Rec – Firmware nimmt auf und spielt (kein Doppelklang) */
            ORIG_KB(ed, row, col, ev);
            return;
        }
        if (ev == 0u) v->aud[t] = (uint8_t)(v->sel[t] ? v->sel[t] : 1u);
        ORIG_KB(ed, row, col, ev);
        return;
    }
    if (!n || row == 0u || row > 7u || col > 3u) { ORIG_KB(ed, row, col, ev); return; }
    uint32_t ui = v->ui;                                  /* UI-Objekt aus lr_button_hook (lrec.c): +0x18 = Shift gehalten */
    uint32_t shift = (ui >= 0x1FFF0000u && ui < 0x20030000u) ? *(volatile uint8_t *)(ui + 0x18u) : 0u;
    uint32_t kbo = ed + 0x14u;
    if (ev == 0u && row == 1u && shift) {                 /* Seitenwahl (Shift + Reihe 1) */
        uint32_t pages = (n + PADS - 1u) / PADS;
        if (col < pages) { v->page[t] = (uint8_t)col; char b[24]; uint32_t k = 0;
            const char *p = "Slices "; while (*p) b[k++] = *p++; k = put_u(b, k, col * PADS + 1u); b[k++] = '-';
            uint32_t last = col * PADS + PADS; if (last > n) last = n; k = put_u(b, k, last); b[k] = 0; TOAST(b);
            UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u); }
        return;
    }
    uint32_t idx = (uint32_t)v->page[t] * PADS + (row - 1u) * 4u + col;
    if (idx >= n || sl_slice_outside(t, idx, n)) return;  /* Pad ohne Slice (031: auch hinter dem Sample-Ende) */
    if (ev == 0u) {
        v->sel[t] = (uint8_t)(idx + 1u);
        { sv_x_t *x_ = sv_xget(1); if (x_) x_->hold[t] = (uint8_t)(idx + 1u); }   /* 17.11.: Pad halten + Sample Start = Schnitt */
        if (steps_held()) set_slice(ed, idx);             /* Slice auf die gehaltenen Steps legen (wie Note/Transpose) */
        else if (*(volatile uint8_t *)(ed + 0x113u)) {    /* 17.11.: Live Rec – Anschlag aufnehmen (Firmware) */
            live_slice(t, idx); live_key(ed, row, col, 0u);
            UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
            return;
        }
        play_slice(ed, t, idx, n, (uint32_t)wv_touched_note());
        /* keine Meldung mehr (028): die Waveform in der Bildmitte zeigt "Slice k/N" und springt mit */
        UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);  /* Pad-LEDs neu (gewaehlter Slice hell) */
    } else if (ev == 1u) {
        { sv_x_t *x_ = sv_xget(0); if (x_ && x_->hold[t] == (uint8_t)(idx + 1u)) x_->hold[t] = 0; }   /* 17.11. */
        if (!steps_held() && *(volatile uint8_t *)(ed + 0x113u)) { live_key(ed, row, col, 1u); return; }   /* 17.11.: Live Rec Loslassen */
        uint32_t node = v->aud_node;
        if (node) {                                       /* 038: One-Shot wie bei vielen Samplern: der Slice spielt bis zum Ende, Loslassen aendert nichts */
            v->aud_node = 0;
        } else {
            KB_PLAY(kbo, wv_touched_note(), 0u, 0);
            UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
        }
    }
}

/* 041: Neue Aufnahme der gewaehlten Slicer-/Granular-Spur zuweisen. 1 = erledigt, 0 = andere Spur: Weg der Firmware */
int sv_assign_sample(int slot) {
    volatile sv_t *v = SV; volatile sl_t *s = SL;
    if (v->magic != SV_MAGIC || s->magic != SL_MAGIC || slot < 0 || slot >= 255) return 0;
    uint32_t t = cur_track();
    if (t >= 8u || (!sl_slices(s->mode[t]) && !sl_is_granular(s->mode[t]))) return 0;
    if (v->sample[t] != (int16_t)(slot + 1)) sv_grid_fit(t);   /* 045 */
    v->sample[t] = (int16_t)(slot + 1);
    v->dirty = 1;
    return 1;
}

/* 045: Parameter eines Steps lesen/setzen ueber seine vtable (+0x18 lesen, +0x44 setzen (step, idx, wert, klemmen)) */
static int step_ok(uint32_t st) {
    if (st < 0x1FFF0000u || st >= 0x2003FF00u || (st & 1u)) return 0;
    uint32_t vt = *(const volatile uint32_t *)st;
    return vt >= 0x9A000u && vt < 0xAE000u;
}
static int32_t step_get(uint32_t st, uint32_t idx) {
    if (!step_ok(st)) return -1;
    uint16_t val = 0;
    uint32_t ok = ((uint32_t (*)(uint32_t, uint32_t, uint16_t *))(((const uint32_t *)(uintptr_t)*(const volatile uint32_t *)st)[6]))(st, idx, &val);
    return ok ? (int32_t)(int16_t)val : -1;
}
static void step_set(uint32_t st, uint32_t idx, int32_t val) {
    if (step_ok(st)) ((void (*)(uint32_t, uint32_t, int32_t, int))(((const uint32_t *)(uintptr_t)*(const volatile uint32_t *)st)[0x44 / 4]))(st, idx, val, 1);
}

/* 045: Fokuszeile des Sample-Knobs. a = "Sample p/N" in der Reihenfolge des Knobs, b = Ordnername wie die Firmware ihn zeigt
 * (Formatter von Eintrag 15, Original 0x2B620: mit Step → Ordner seines Samples). Die Vorlage bekommt dafuer das Spur-Sample. */
static void sample_focus_calc(uint32_t t, char *a, char *b);
static void sample_focus(uint32_t t, char *a, char *b) {
#ifndef HOST_TEST
    volatile sfc_t *c = SFC;
    int32_t cur = sv_sample(t);
    uint32_t key = (t & 0xFFu) | ((uint32_t)(cur & 0xFFFF) << 8) | 0x1000000u, now = NOW();
    if (c->magic != 0x5FC00450u || c->key != key || now - c->t_ms > 250u) {
        char ta[18], tb[18];
        sample_focus_calc(t, ta, tb);
        for (uint32_t i = 0; i < 18u; i++) { c->a[i] = ta[i]; c->b[i] = tb[i]; }
        c->key = key; c->t_ms = now; c->magic = 0x5FC00450u;
    }
    for (uint32_t i = 0; i < 18u; i++) { a[i] = c->a[i]; b[i] = c->b[i]; }
    a[17] = 0; b[17] = 0;
#else
    sample_focus_calc(t, a, b);
#endif
}
static void sample_focus_calc(uint32_t t, char *a, char *b) {
    uint8_t cls[255];
    wv_sample_classes(cls);
    int32_t cur = sv_sample(t);
    if (cur < 0) cur = wv_track_sample(t);
    uint32_t n = 0, pos = 0;
    for (int32_t i = 254; i >= 0; i--) if (cls[i] == 2u) { n++; if (i == cur) pos = n; }
    for (int32_t i = 0; i < 255; i++) if (cls[i] == 1u) { n++; if (i == cur) pos = n; }
    uint32_t j = put_s(a, 0, "Sample ");
    if (pos) { j = put_u(a, j, pos); a[j++] = '/'; j = put_u(a, j, n); } else a[j++] = '-';
    a[j] = 0;
    b[put_s(b, 0, "Folder")] = 0;
#ifndef HOST_TEST
    uint32_t dst = *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu);
    if (!step_ok(dst)) return;
    if (cur >= 0 && step_get(dst, 9u) != cur) step_set(dst, 9u, cur);
    uint32_t fo = sl_orig_fmt(15u);                       /* Original (Eintrag 15 zeigt seit 045 auf die Mitlese-Weiche) */
    if (!fo) fo = *(const volatile uint32_t *)(0x1FFF7918u + 0x20u * 15u + 0x10u);
    if (fo < 0x1FFF0000u || fo >= 0x20030000u || *(const volatile uint32_t *)fo != 0x000A0458u) return;
    char tmp[18]; tmp[0] = 0;
    ((void (*)(void *, int32_t, int32_t, char *, uint32_t, void *))0x0002B621u)((void *)(uintptr_t)fo, 0, 0, tmp, sizeof tmp, (void *)(uintptr_t)dst);
    tmp[17] = 0;
    if (tmp[0]) { uint32_t k = 0; while (k < 17u && tmp[k]) { b[k] = tmp[k]; k++; } b[k] = 0; }
#endif
}

/* Slice waehlen (Sample Start ohne gehaltenen Step, 045: Encoder), rund */
static void sel_step(uint32_t t, int dir, uint32_t ed) {
    volatile sv_t *v = SV;
    uint32_t n = sv_beat_on(t) && sv_beat_count(t) ? sv_beat_count(t) : sl_slices(SL->mode[t]), c = v->sel[t];
    if (!n) return;
    c = dir > 0 ? (c >= n ? 1u : c + 1u) : (c <= 1u ? n : c - 1u);
    v->sel[t] = (uint8_t)c;
    v->page[t] = (uint8_t)((c - 1u) / PADS);
    UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);
}

#ifndef HOST_TEST
/* 045: UI-vtable [4] (Wort 0xA0168, Original Ui_onEncoder 0x2A848): In View auf einer Slicer-Spur waehlt der Encoder den Slice.
 * Menue offen oder die aktive Seite nimmt den Encoder selbst (vt+0x28 != 0): Original. Im Original tut er hier nichts. */
/* 046 (14.11.): Encoder bei gehaltenem Step auf der Slicer-Spur in View = Slice DIESES Steps wechseln (wie Pad: setzen + anspielen).
 * Vorher ging der Encoder dann ans Original, das den zuletzt beruehrten Parameter dreht – meist Sample Start = Feinversatz.
 * Regel (Nutzer): Encoder = Slice waehlen, Sample Start = Feinversatz. Ausgangspunkt = Slice des ersten gehaltenen Steps. */
static int enc_held_slice(uint32_t t, int delta, uint32_t ed) {
    volatile sv_t *v = SV;
    volatile uint32_t *h = (volatile uint32_t *)(ed + 0x88u);
    uint32_t bits = (h[t >> 1] >> ((t & 1u) * 16u)) & 0xFFFFu;
    uint32_t sel = *(volatile uint32_t *)(ed + 8u), sc = *(volatile uint32_t *)(ed + 4u);
    if (!bits || sel < 0x1FFF0000u || sel >= 0x2003FF00u || sc < 0x1FFF0000u || sc >= 0x2003FF00u) return 0;
    uint32_t n = sv_beat_on(t) && sv_beat_count(t) ? sv_beat_count(t) : sl_slices(SL->mode[t]);
    if (!n) return 0;
    uint32_t b = 0; while (!((bits >> b) & 1u)) b++;
    uint32_t pg = *(volatile uint32_t *)(sc + (t + 0x904u) * 4u) * 16u;
    uint32_t st = ((uint32_t (*)(uint32_t, uint32_t, uint32_t))0x0002C37Du)(sel, t, pg + b);   /* STEP_PTR */
    int32_t cur = step_get(st, 10u);
    uint32_t k = cur < 0 ? (v->sel[t] ? v->sel[t] - 1u : 0u) : ((uint32_t)cur >> 6) % n;
    int cnt = delta < 0 ? -delta : delta; if (cnt > 8) cnt = 8;   /* wie der Slice-Encoder ohne Step: hoechstens 8 Rasten je Aufruf */
    while (cnt--) k = delta > 0 ? (k + 1u >= n ? 0u : k + 1u) : (k == 0u ? n - 1u : k - 1u);
    v->sel[t] = (uint8_t)(k + 1u);
    v->page[t] = (uint8_t)(k / PADS);
    set_slice(ed, k);
    play_slice(ed, t, k, n, (uint32_t)wv_touched_note());
    UI_REFRESH(sc, 1u);
    return 1;
}

__attribute__((used, section(".text.entry_svenc")))
void sv_enc_hook(uint32_t ui, uint32_t id, int delta) {
    volatile sv_t *v = SV;
    uint32_t t = cur_track();
    if (delta && v->magic == SV_MAGIC && SL->magic == SL_MAGIC && t < 8u && sl_slices(SL->mode[t]) && steps_held() && !sv_menu_on()) {
        uint32_t ed = EDITOR;
        if (ed >= 0x1FFF0000u && ed < 0x20030000u && kb_active(ed) && enc_held_slice(t, delta, ed)) return;
    }
    if (delta && v->magic == SV_MAGIC && SL->magic == SL_MAGIC && t < 8u && sl_slices(SL->mode[t]) && kb_active(EDITOR) && !steps_held()
        && ui >= 0x1FFF0000u && ui < 0x20030000u) {
        uint32_t mgr = *(volatile uint32_t *)(ui + 8u);
        uint32_t scr = (mgr >= 0x1FFF0000u && mgr < 0x20030000u) ? *(volatile uint32_t *)(mgr + 0x2FCu) : 0u;
        if (scr >= 0x1FFF0000u && scr < 0x20030000u && *(volatile uint32_t *)scr != 0x000A97E0u) {
            const uint32_t *svt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)scr;
            if (((uint32_t (*)(uint32_t, int, uint32_t))svt[0x28 / 4])(scr, delta, *(volatile uint8_t *)(ui + 0x18u))) return;
            int n = delta < 0 ? -delta : delta; if (n > 8) n = 8;
            while (n--) sel_step(t, delta > 0 ? 1 : -1, EDITOR);
            return;
        }
    }
    ((void (*)(uint32_t, uint32_t, int))0x0002A849u)(ui, id, delta);
}
#endif

/* Sample der Spur weiterschalten (Regler Sample) */
static void next_sample(uint32_t t, int dir) {
    volatile sv_t *v = SV;
    uint8_t cls[255];
    int any_lr = wv_sample_classes(cls);
    int32_t cur = sv_sample(t);
    if (cur < 0 && !any_lr) cur = wv_track_sample(t);    /* erste Drehung: neueste Aufnahme, sonst ab dem jetzigen Sample */
    int32_t x = sv_next_sample(cls, cur, dir);
    if (x >= 0) { if (v->sample[t] != (int16_t)(x + 1)) sv_grid_fit(t); v->sample[t] = (int16_t)(x + 1); v->dirty = 1; }   /* 045: neues Sample = Fit */
}

/* Editor vt+0x1C (Wort 0xA5A70): Parameter aendern. Ausserhalb von View und mit gehaltenem Step unveraendert Firmware.
 * In View auf einer Slicer-Spur: Sample immer, Slice-Wahl/Move/Breite ohne gehaltene Steps an die Vorbereitung; Klangregler an den
 * Standard-Step, auch wenn die Spur gewaehlt ist (0x1FFF4518 != 0 hiesse in FUN_0002d0d8: alle Steps relativ, am Geraet gemessen). */
__attribute__((used, section(".text.entry_svchg")))
/* 13.11.: Feinversatz eines Steps um eine Raste (Slice bleibt; untere 6 Bit, 0/32/63 = keiner, -1 ausgelassen) */
static void fine_adj(uint32_t st, int dir) {
    int32_t cur = step_get(st, 10u);
    if (cur < 0) cur = 32;
    int32_t o = sl_fine(cur) + dir;
    if (o == -1) o = dir < 0 ? -2 : 0;
    if (o < -31) o = -31;
    if (o > 31) o = 31;
    int32_t nv = (cur & ~63) + sl_fine_enc(o);
    if (nv != cur) step_set(st, 10u, nv);
}
#define STEP_PTR(sel, t, i) (((uint32_t (*)(uint32_t, uint32_t, uint32_t))0x0002C37Du)((sel), (t), (i)))
#define SHOW_PARAM(sel)     (((void (*)(uint32_t))0x0002DF81u)(sel))      /* FUN_0002df80: Step-Seite mit aktuellen Werten */
/* Sample Start (10): nie Slices wechseln, sondern Feinversatz – gehaltene Steps, markierte Spuren (alle Steps) oder Vorlage.
 * Sample (9) bei markierter Slicer-Spur: Sample der Spur wechseln (Slices bleiben), wie +1 bei normalen Spuren. 1 = erledigt */
static int out_view_chg(uint32_t ed, int idx, int dir) {
    volatile sl_t *s = SL;
    uint32_t sel = *(volatile uint32_t *)(ed + 8u), sc = *(volatile uint32_t *)(ed + 4u);
    if (sel < 0x1FFF0000u || sel >= 0x2003FF00u || sc < 0x1FFF0000u || sc >= 0x2003FF00u) return 0;
    if (steps_held()) {
        if (idx != 10) return 0;                          /* Sample bei gehaltenem Step: wie Original (ein Sample je Spur) */
        volatile uint32_t *h = (volatile uint32_t *)(ed + 0x88u);
        int done = 0;
        for (uint32_t t = 0; t < 8u; t++) {
            uint32_t bits = (h[t >> 1] >> ((t & 1u) * 16u)) & 0xFFFFu;
            if (!bits || !sl_slices(s->mode[t])) continue;
            uint32_t pg = *(volatile uint32_t *)(sc + (t + 0x904u) * 4u) * 16u;
            for (uint32_t b = 0; b < 16u; b++) if ((bits >> b) & 1u) { fine_adj(STEP_PTR(sel, t, pg + b), dir); done = 1; }
        }
        if (done) SHOW_PARAM(sel);
        return done;
    }
    uint32_t m = SEL_MASK & 0xFFu, sm = 0;
    for (uint32_t t = 0; t < 8u; t++) if (((m >> t) & 1u) && sl_slices(s->mode[t])) sm |= 1u << t;
    if (!m) {                                             /* nichts markiert: Vorlage der Slicer-Spur */
        uint32_t t = cur_track();
        if (idx != 10 || t >= 8u || !sl_slices(s->mode[t])) return 0;
        fine_adj(*(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu), dir);
        SHOW_PARAM(sel);
        return 1;
    }
    if (!sm) return 0;
    for (uint32_t t = 0; t < 8u; t++) {
        if (!((sm >> t) & 1u)) continue;
        if (idx == 10) { for (uint32_t i = 0; i < 64u; i++) if (STEP_ON(sel, t, i)) fine_adj(STEP_PTR(sel, t, i), dir); }
        else next_sample(t, dir);
    }
    uint32_t full = SEL_MASK;
    if (m & ~sm) { SEL_MASK = full & ~sm; ORIG_CHG(ed, idx, dir); SEL_MASK = full; }   /* uebrige (normale) Spuren wie Original */
    SHOW_PARAM(sel);
    return 1;
}
void sv_chg_hook(uint32_t ed, int idx, int delta) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    if (v->magic == SV_MAGIC && s->magic == SL_MAGIC && delta) {
        if (kb_active(ed)) {
            uint32_t t = cur_track();
            if (t < 8u && sl_is_granular(s->mode[t])) {   /* 039 */
                int dir = delta > 0 ? 1 : -1;
                gr_cfg_t *c = gr_cfg(t);
                if (idx == 9) { next_sample(t, dir); return; }
                if (!steps_held() && !*(volatile uint8_t *)(ed + 0x113u)) {
                    #define STEPU8(f, st_, hi) { int32_t x_ = (int32_t)c->f + dir * (st_); c->f = (uint8_t)(x_ < 0 ? 0 : x_ > (hi) ? (hi) : x_); v->dirty = 1; return; }
                    if (idx == 21) { int32_t x_ = c->scan + dir * 2; c->scan = (int8_t)(x_ < -100 ? -100 : x_ > 100 ? 100 : x_); v->dirty = 1; return; }
                    if (idx == 20) STEPU8(spray, 2, 100)
                    if (idx == 16) STEPU8(win, 2, 100)
                    if (idx == 17) STEPU8(pspray, 1, 24)
                    if (idx == 19) STEPU8(ldepth, 2, 100)
                    if (idx == 18) {
                        uint32_t ui_ = v->ui;
                        if (ui_ >= 0x1FFF0000u && ui_ < 0x20030000u && *(volatile uint8_t *)(ui_ + 0x18u)) { c->ltgt = (uint8_t)((c->ltgt + (dir > 0 ? 1u : 3u)) & 3u); v->dirty = 1; return; }
                        STEPU8(lrate, 1, 31)
                    }
                    uint32_t m = SEL_MASK, sel = *(volatile uint32_t *)(ed + 8u);
                    SEL_MASK = 0u;
                    ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel, idx, delta);
                    SEL_MASK = m;
                    sl_preview_params(t);
                    if (idx == 10) { int *c_ = sl_track_ctx(t); if (c_) { void *a = NODE_SLOT((void *)(uintptr_t)(uint32_t)c_[0], 8u); int32_t x_ = wv_touched_param(10u); if (a && x_ != -32768) SLOT_SET(a, 1u, x_); } }
                    if (idx == 6) UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 0u);
                    return;
                }
            }
            if (t < 8u && sl_slices(s->mode[t])) {
                int dir = delta > 0 ? 1 : -1;
                if (idx == 9) { next_sample(t, dir); return; }
                if (idx == 15 && !steps_held() && !*(volatile uint8_t *)(ed + 0x113u)) {
                    /* 045 Folder (unterer Fokus des Sample-Knobs): Ordnerwechsel der Firmware, ausgehend vom Sample der Spur. Bisher
                     * wechselte nur die Vorlage (nicht sichtbar, die Spur spielt ihr eigenes Sample) und sie spielte Slice 1 an. */
                    uint32_t dst = *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu);
                    int32_t cur = sv_sample(t);
                    if (cur >= 0) step_set(dst, 9u, cur);
                    if (v->sel[t]) v->aud[t] = v->sel[t];             /* Anspielen der Firmware: gewaehlter Slice */
                    uint32_t m = SEL_MASK, sel = *(volatile uint32_t *)(ed + 8u);
                    SEL_MASK = 0u;
                    ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel, idx, delta);
                    SEL_MASK = m;
                    int32_t ns = step_get(dst, 9u);
                    if (ns >= 0 && ns < 255) { if (v->sample[t] != (int16_t)(ns + 1)) sv_grid_fit(t); v->sample[t] = (int16_t)(ns + 1); v->dirty = 1; }
                    return;
                }
                if (idx == 10 && !steps_held()) {          /* 17.11.: Sample Start schneidet nur, solange ein Slice-Pad gehalten wird (vorher immer: zu leicht aus Versehen) */
                    uint32_t n_ = sl_slices(s->mode[t]);
                    sv_x_t *x_ = sv_xget(0);
                    uint32_t h_ = (x_ && v->kmode[t] != 1u) ? x_->hold[t] : 0u;
                    if (h_ && n_ && h_ <= n_) { sv_cut_turn(t, h_ - 1u, n_, dir, NOW()); v->dirty = 1; UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u); }
                    else fine_adj(*(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu), dir);   /* 17.11.: sonst Feinversatz der Vorlage (neue Steps, Vorhoeren) wie ausserhalb von View */
                    return;
                }
                if (idx == 10 && steps_held()) {          /* 045: gehaltener Step: Feinversatz des Anfangs (Slice-Wahl: Step halten + Pad) */
                    if (out_view_chg(ed, 10, dir)) return;  /* 17.11.: direkt wie ausserhalb von View – kein Anspielen je Raste (Marker blieb unruhig) */
                    int32_t cur = wv_touched_param(10u);
                    if (cur == -32768) cur = 32;
                    int32_t o = sl_fine(cur) + dir;
                    if (o == -1) o = dir < 0 ? -2 : 0;      /* −1 = 63 waere "kein Versatz" (Knob am Anschlag) */
                    if (o < -31) o = -31;
                    if (o > 31) o = 31;
                    int32_t nv = (cur & ~63) + sl_fine_enc(o);
                    if (nv != cur) ORIG_CHG(ed, 10, nv - cur);
                    return;
                }
                if (idx == 20 && !steps_held()) {         /* Micromove = Grid-Breite; schnell drehen = 8 Rasten */
                    sv_x_t *x = sv_xget(1);
                    if (x && !x->smode[t]) {
                        int q = (NOW() - x->wid_t) < 60u ? 8 : 1;
                        x->wid_t = NOW();
                        sv_apply(t, 20u, dir * q);
                        v->dirty = 1;
                        UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u);   /* 045: Pad-LEDs sofort (Slices hinter dem Ende aus) */
                    }
                    return;
                }
                if (idx == 21 && !steps_held()) {         /* Move: nur im Grid-Modus */
                    if (!sv_beat_on(t)) { sv_apply(t, 21u, dir); v->dirty = 1; UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 1u); }   /* 045: LEDs sofort */
                    else { sv_x_t *x = sv_xget(1); if (x) { int32_t nn = x->nudge[t] + dir; x->nudge[t] = (int8_t)(nn < -50 ? -50 : nn > 50 ? 50 : nn); x->gen[t]++; v->dirty = 1; } }   /* 045: Beat-Nudge 1 ms */
                    return;
                }
                if (!steps_held() && !*(volatile uint8_t *)(ed + 0x113u)) {   /* Live Rec braucht die Auswahl */
                    /* 034: direkt FUN_0002d0d8 (schreibt den Standard-Step, zeigt ihn an). Der Weg ueber 0x3B088/FUN_0002df48 verzweigt bei
                     * gewaehlter Spur in den Mehrfach-Edit (am Geraet gemessen: Anzeige aendert sich, Standard-Step nicht). */
                    uint32_t m = SEL_MASK, sel = *(volatile uint32_t *)(ed + 8u);
                    SEL_MASK = 0u;
                    ((void (*)(uint32_t, int, int))0x0002D0D9u)(sel, idx, delta);
                    SEL_MASK = m;
                    if (idx == 6) UI_REFRESH(*(volatile uint32_t *)(ed + 4u), 0u);
                    return;
                }
            }
        } else if (idx == 10 || idx == 9) {               /* 13.11.: ausserhalb von View auf Slicer-Spuren */
            if (out_view_chg(ed, idx, delta > 0 ? 1 : -1)) return;
        }
    }
    ORIG_CHG(ed, idx, delta);
}

/* Knob merken: Fokuszeile bzw. Firmware-Anzeige fuer Knobs ohne Step-Seite (Master, Tempo, Random, Play Mode) */
static void note_knob(uint32_t ui, uint32_t knob) {
    volatile sv_t *v = SV;
    if (v->magic != SV_MAGIC || knob >= 15u) return;
    v->ui = ui;
    v->fw_until = NOW();                                  /* 045: anderer Knob beendet die Firmware-Anzeige (Master/Tempo/Random) */
    if (sv_page_track() >= 8u) return;
    if (sv_page_track() < 8u && knob == 9u && sl_is_granular(SL->mode[sv_page_track()])) { v->knob = 10u; v->foc_until = NOW() + FOC_MS; v->cap_until = NOW(); return; }
    if (knob == 0u || knob == 5u || knob == 9u) { v->fw_until = NOW() + FW_MS; v->knob = 0; return; }
    uint32_t tn = sv_page_track();
    if (knob == 2u || knob == 7u || knob == 10u || knob == 14u || (tn < 8u && sl_is_granular(SL->mode[tn]) && (knob == 13u || knob == 4u))) {
        v->knob = knob + 1u; v->foc_until = NOW() + FOC_MS; v->cap_until = NOW();
    }
}

/* UI-vtable 0xA0158 [2] (Wort 0xA0160, Original Ui_onKnobTurn 0x2A71C): Track Length (Knob 10, Fokus oben) ist kein
 * Step-Parameter, sondern eine Funktion → hier abfangen. Fokus wie im Original ueber FUN_0001ff7c, nicht ueber Shift. */
__attribute__((used, section(".text.entry_svknob")))
void sv_knob_hook(uint32_t ui, uint32_t knob, int delta) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    if (tt_turn(ui, knob, delta)) return;                    /* 045: Menue offen → Tempo-Knob, Menue bleibt offen */
    note_knob(ui, knob);
    uint32_t t = sv_page_track();
    if (knob == 9u && delta && t < 8u && !steps_held() && sl_is_granular(s->mode[t])) {    /* 044: Random-Knob auf Granular: Speed / Rate */
        int d9 = delta > 0 ? 1 : -1;
        gr_cfg_t *c9 = gr_cfg(t);
        if (alt_focus()) { int32_t y = (int32_t)c9->rate + d9 * 2; c9->rate = (uint8_t)(y < 10 ? 10 : y > 100 ? 100 : y); }
        else { int32_t y = (int32_t)c9->speed + d9 * (c9->speed <= 20u ? 1 : 5); c9->speed = (uint8_t)(y < 5 ? 5 : y > 200 ? 200 : y); }
        v->dirty = 1;
        return;
    }
    if (knob == 10u && delta && t < 8u && !steps_held() && sl_is_granular(s->mode[t])) {   /* 039: Grains / Direction */
        uint32_t scr = *(volatile uint32_t *)(*(volatile uint32_t *)(ui + 8u) + 0x2FCu);
        if (scr) {
            const uint32_t *svt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)scr;
            if (((uint32_t (*)(uint32_t, uint32_t, int, uint32_t))svt[7])(scr, knob, delta, *(volatile uint8_t *)(ui + 0x18u))) return;
        }
        int d_ = delta > 0 ? 1 : -1;
        if (alt_focus()) {                                /* Play Mode: Forward, Reverse, Ping-Pong, Random, Scan Bounce, Random Walk */
            gr_cfg_t *c_ = gr_cfg(t);
            int32_t ix = c_->pmode ? (int32_t)c_->pmode + 2 : s->mode[t] - SL_GRAN;
            ix += d_; ix = ix < 0 ? 0 : ix > 5 ? 5 : ix;
            if (ix <= 2) { c_->pmode = 0u; s->mode[t] = SL_GRAN + ix; }
            else { c_->pmode = (uint8_t)(ix - 2); s->mode[t] = SL_GRAN; }
        }
        else { int32_t n_ = (int32_t)gr_cfg(t)->n + d_; gr_cfg(t)->n = (uint8_t)(n_ < 1 ? 1 : n_ > 8 ? 8 : n_); }
        v->dirty = 1;
        return;
    }
    if (knob == 10u && delta && t < 8u && !steps_held()) {
        uint32_t scr = *(volatile uint32_t *)(*(volatile uint32_t *)(ui + 8u) + 0x2FCu);
        if (scr) {
            const uint32_t *svt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)scr;
            if (((uint32_t (*)(uint32_t, uint32_t, int, uint32_t))svt[7])(scr, knob, delta, *(volatile uint8_t *)(ui + 0x18u))) return;
        }
        if (alt_focus()) {                                /* Fokus unten: rechts Beat (rechnet neu), links Grid */
            sv_beat_toggle(t, delta > 0, &s->mode[t]);
            v->dirty = 1;
            uint32_t n2 = sl_slices(s->mode[t]);
            if (v->sel[t] > n2) v->sel[t] = 0;
            if ((uint32_t)v->page[t] * PADS >= n2) v->page[t] = 0;
            UI_REFRESH(*(volatile uint32_t *)(EDITOR + 4u), 1u);
            return;
        }
        if (sv_beat_on(t)) {                              /* Beat: Genauigkeit statt Anzahl */
            sv_fine_step(t, delta);
            v->dirty = 1;
            UI_REFRESH(*(volatile uint32_t *)(EDITOR + 4u), 1u);
            return;
        }
        s->mode[t] = sv_step_mode(s->mode[t], delta);
        uint32_t n = sl_slices(s->mode[t]);
        if (v->sel[t] > n) v->sel[t] = 0;
        if ((uint32_t)v->page[t] * PADS >= n) v->page[t] = 0;
        UI_REFRESH(*(volatile uint32_t *)(EDITOR + 4u), 1u);      /* Pad-LEDs */
        return;
    }
    ((void (*)(uint32_t, uint32_t, int))0x0002A71Du)(ui, knob, delta);
}

/* UI-vtable [3] (Wort 0xA0164, Original Ui_onKnobTouch 0x2A74C): Beruehren zeigt schon den Fokus */
__attribute__((used, section(".text.entry_svtouch")))
void sv_touch_hook(uint32_t ui, uint32_t knob, int ev) {
    if (tt_touch(ui, knob, ev)) return;                      /* 045: Tap Tempo im Menue */
    if (ev == 0) note_knob(ui, knob);
    ((void (*)(uint32_t, uint32_t, int))0x0002A74Du)(ui, knob, ev);
}

/* Editor vt+0x14 (Wort 0xA5A68, Original FUN_0003bc20): Grid-Pad. In View auf einer Slicer-Spur mit gewaehltem Slice
 * bekommt ein NEU gesetzter Step den zuletzt gewaehlten Slice (wie die Firmware die letzte Tastatur-Note nimmt).
 * Vorhandene Steps (halten zum Bearbeiten, Loeschen beim Loslassen) bleiben unberuehrt. */
__attribute__((used, section(".text.entry_svgrid")))
void sv_grid_hook(uint32_t ed, uint32_t row, uint32_t col, uint32_t ev, uint32_t f5) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    uint32_t t = 8u, st = 0u, pre = 1u;
    if (ev == 0u && row < 8u && col < 16u && v->magic == SV_MAGIC && s->magic == SL_MAGIC && kb_active(ed)) {
        t = row + (*(volatile uint8_t *)(ed + 0x112u) ? 8u : 0u);
        if (t < 8u && sl_is_granular(s->mode[t]) && v->sel[t]) {   /* 039: neuer Step uebernimmt die Pad-Position: steckt schon in der Vorlage */
            t = 8u;
        } else if (t < 8u && sl_slices(s->mode[t]) && v->sel[t] && v->sel[t] <= sl_slices(s->mode[t])) {
            uint32_t sc = *(volatile uint32_t *)(ed + 4u), sel = *(volatile uint32_t *)(ed + 8u);
            st = col + *(volatile uint32_t *)(sc + (t + 0x904u) * 4u) * 16u;   /* Seite der Spur wie FUN_0003b2e4 */
            pre = STEP_ON(sel, t, st);
        } else t = 8u;
    }
    ORIG_GRID(ed, row, col, ev, f5);
    if (t < 8u && !pre && STEP_ON(*(volatile uint32_t *)(ed + 8u), t, st)) set_slice(ed, v->sel[t] - 1u);
}

/* 0x3A75A / 0x3A784 (FUN_0003a6fc): bl FUN_00023150 = Farbe eines Tastatur-Pads in View. Auf einer Slicer-Spur:
 * gewaehlter Slice hell gelb, Pads mit Slice gedimmt gelb, Pads ohne Slice aus. Reihe 0 (Sample-Plaetze) wie Original. */
__attribute__((used, section(".text.entry_svled")))
void sv_led_hook(uint32_t kb, uint32_t row, uint32_t col, int note, uint32_t pg) {
    volatile sv_t *v = SV;
    volatile sl_t *s = SL;
    uint32_t ed = kb - 0x14u;
    if (row == 0u && col <= 3u && v->magic == SV_MAGIC && s->magic == SL_MAGIC && ed == EDITOR) {   /* 040: gruene Pads auf Granular */
        uint32_t tg = cur_track();
        if (tg < 8u && sl_is_granular(s->mode[tg])) {
            /* 044: Drone in eigener Farbe (Violett): blass = aus, kraeftig = an */
            static const uint8_t DRONE_LED[2][6] = { { 28, 4, 44, 28, 4, 44 }, { 200, 30, 255, 200, 30, 255 } };
            uint32_t cc = col == 2u ? (uint32_t)(uintptr_t)&DRONE_LED[gr_cfg(tg)->drone ? 1 : 0][((pg & 0xFFu) ^ 1u) * 3u] : COL_OFF;
            uint32_t leds = *(volatile uint32_t *)(kb + 0xCu);
            const uint32_t *lvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)leds;
            ((void (*)(uint32_t, int, uint32_t, int, int))lvt[1])(leds, (int)(col + row * 0x14u + 0x10u), cc, 0, 0);
            return;
        }
        if (tg < 8u && sl_slices(s->mode[tg])) {         /* 045: Modus-Pads Slices/Keys: aktiv hell, anderer blass, 3/4 aus */
            static const uint8_t MODE_LED[2][6] = { { 0, 40, 12, 0, 40, 12 }, { 0, 220, 70, 0, 220, 70 } };
            uint32_t cc = col <= 1u ? (uint32_t)(uintptr_t)&MODE_LED[v->kmode[tg] == col ? 1 : 0][((pg & 0xFFu) ^ 1u) * 3u] : COL_OFF;
            uint32_t leds = *(volatile uint32_t *)(kb + 0xCu);
            const uint32_t *lvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)leds;
            ((void (*)(uint32_t, int, uint32_t, int, int))lvt[1])(leds, (int)(col + row * 0x14u + 0x10u), cc, 0, 0);
            return;
        }
    }
    if (row >= 1u && row <= 7u && col <= 3u && v->magic == SV_MAGIC && s->magic == SL_MAGIC && ed == EDITOR) {
        uint32_t t = cur_track();
        uint32_t n = t < 8u ? sl_slices(s->mode[t]) : 0u;
        if (t < 8u && sl_is_granular(s->mode[t])) {       /* 039 */
            uint32_t idx = (row - 1u) * 4u + col;
            uint32_t c = (v->sel[t] == idx + 1u ? COL_BRIGHT : COL_NORMAL) + ((pg & 0xFFu) ^ 1u) * 3u;
            uint32_t leds = *(volatile uint32_t *)(kb + 0xCu);
            const uint32_t *lvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)leds;
            ((void (*)(uint32_t, int, uint32_t, int, int))lvt[1])(leds, (int)(col + row * 0x14u + 0x10u), c, 0, 0);
            return;
        }
        if (n && v->kmode[t] == 1u) { ORIG_PADLED(kb, row, col, note, pg); return; }   /* 045 Keys: Tastatur-Farben des Originals */
        if (n && row == 1u && n > PADS && sv_shift()) {        /* 045: Shift gehalten → Seitenwahl sichtbar */
            static const uint8_t PAGE_LED[2][6] = { { 0, 40, 12, 0, 40, 12 }, { 0, 220, 70, 0, 220, 70 } };
            uint32_t pages = (n + PADS - 1u) / PADS;
            uint32_t c = col < pages ? (uint32_t)(uintptr_t)&PAGE_LED[v->page[t] == col ? 1 : 0][((pg & 0xFFu) ^ 1u) * 3u] : COL_OFF;
            uint32_t leds = *(volatile uint32_t *)(kb + 0xCu);
            const uint32_t *lvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)leds;
            ((void (*)(uint32_t, int, uint32_t, int, int))lvt[1])(leds, (int)(col + row * 0x14u + 0x10u), c, 0, 0);
            return;
        }
        if (n) {
            uint32_t idx = (uint32_t)v->page[t] * PADS + (row - 1u) * 4u + col;
            uint32_t c = (idx >= n || sl_slice_outside(t, idx, n)) ? COL_OFF : (v->sel[t] == idx + 1u ? COL_BRIGHT : COL_NORMAL) + ((pg & 0xFFu) ^ 1u) * 3u;
            uint32_t leds = *(volatile uint32_t *)(kb + 0xCu);
            const uint32_t *lvt = (const uint32_t *)(uintptr_t)*(volatile uint32_t *)leds;
            ((void (*)(uint32_t, int, uint32_t, int, int))lvt[1])(leds, (int)(col + row * 0x14u + 0x10u), c, 0, 0);
            return;
        }
    }
    ORIG_PADLED(kb, row, col, note, pg);
}
#endif
