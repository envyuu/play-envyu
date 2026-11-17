/* Patch 023: Host-Test der Waveform-Rechnung (wave.c) und der Live-Rec-Zustandsmaschine (lrec.c), ASan/UBSan, 32 Bit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "slicer.h"
#include "wave.h"
#include <math.h>
#include "lrec.h"

uint8_t host_sl_ram[0x100] __attribute__((aligned(8)));
uint8_t host_gr_ram[0x200] __attribute__((aligned(8)));
uint8_t host_wv_ram[0x3B0] __attribute__((aligned(8)));
uint8_t host_lr_ram[sizeof(lr_t)] __attribute__((aligned(8)));
/* Attrappen fuer slicer.c (Host) */
uint8_t host_param_tab[22 * 0x20]; uint32_t host_millis; uintptr_t host_pat0, host_pat1;
uint32_t host_get_param(int *s, uint32_t i, int *c, uint8_t *e) { (void)s; (void)i; (void)c; (void)e; return 0; }
void *host_node_slot(void *n, uint32_t i) { (void)n; (void)i; return 0; }
void host_slot_set(void *s, uint32_t q, int32_t v) { (void)s; (void)q; (void)v; }
void host_orig_fmt(void *a, int32_t b, int32_t c, char *d, uint32_t e, void *f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void host_arp_init(void) {}
void *host_heap_alloc(uint32_t n) { (void)n; return 0; }
void *host_menu_add(void *a, void *b, const char *c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void *host_enum_item(void *a, void *b, const char *c, int32_t *d, uint32_t e, const char *const *f, void *g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return 0; }
void *host_malloc(uint32_t n) { return malloc(n); }
void host_free(void *p) { free(p); }
int host_f_open(void *f, const char *p, uint8_t m) { (void)f; (void)p; (void)m; return 4; }
int host_f_read(void *f, void *b, uint32_t n, uint32_t *r) { (void)f; (void)b; (void)n; *r = 0; return 1; }
int host_f_write(void *f, const void *b, uint32_t n, uint32_t *w) { (void)f; (void)b; (void)n; *w = 0; return 1; }
int host_f_close(void *f) { (void)f; return 0; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FEHLER "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint32_t rnd = 1;
static uint32_t xr(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

int main(void) {
    volatile wv_t *w = (volatile wv_t *)(void *)host_wv_ram;
    wv_reset();
    CHECK(w->magic == WV_MAGIC && w->slot == 0xFFFFFFFFu, "wv_reset");
    /* 1. Spaltenbereiche: lueckenlos, ueberdeckend, nie leer, fuer viele Laengen */
    uint32_t lens[] = { 2, 3, 100, 303, 304, 305, 999, 44100, 88200, 1234567, 15876000, 16777215 };
    for (uint32_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        uint32_t F = lens[li], prev = 0, ok = 1;
        for (uint32_t c = 0; c < WV_W; c++) {
            uint32_t s0, s1; wv_column_range(F, c, &s0, &s1);
            if (s1 > F || s0 >= s1) ok = 0;
            if (F >= WV_W && s0 != prev) ok = 0;
            prev = s1;
        }
        if (F >= WV_W && prev != F) ok = 0;
        CHECK(ok, "Spaltenbereiche F=%u", F);
    }
    /* 2. Min/Max gegen Referenz (Stichprobe <= 64+1 Werte je Spalte liegt innerhalb des echten Bereichs) */
    uint32_t F = 200000; int16_t *d = malloc(F * 2);
    for (uint32_t i = 0; i < F; i++) d[i] = (int16_t)((int32_t)(xr() % 60001u) - 30000);
    d[0] = 32767; d[F - 1] = -32768;
    int okmm = 1;
    for (uint32_t c = 0; c < WV_W; c++) {
        int8_t mn, mx; wv_column(d, F, c, &mn, &mx);
        uint32_t s0, s1; wv_column_range(F, c, &s0, &s1);
        int lo = 32767, hi = -32768;
        for (uint32_t i = s0; i < s1; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
        if (mn < (lo >> 8) || mx > (hi >> 8) || mn > mx) okmm = 0;
        if (c == 0 && mx != 127) okmm = 0;
        if (c == WV_W - 1 && mn != -128) okmm = 0;
    }
    CHECK(okmm, "Min/Max je Spalte innerhalb des echten Bereichs, erstes/letztes Sample erfasst");
    /* Sinus: Huellkurve wird getroffen */
    for (uint32_t i = 0; i < F; i++) d[i] = (int16_t)(((int32_t)((i * 37u) % 200u) - 100) * 300);
    int8_t mn, mx; wv_column(d, F, 10, &mn, &mx);
    CHECK(mx > 100 && mn < -100, "Saegezahn: Spitzen erfasst (%d/%d)", mn, mx);
    /* 3. Bitmap-Zeilen: Stille = Mittellinie, Vollaussteuerung = ganze Hoehe, symmetrisch */
    for (uint32_t c = 0; c < WV_W; c++) { w->mn[c] = 0; w->mx[c] = 0; }
    uint8_t row[WV_STRIDE]; uint32_t lit = 0;
    for (uint32_t y = 0; y < WV_H; y++) { wv_row(w->mn, w->mx, 0, y, row); for (uint32_t b = 0; b < WV_STRIDE; b++) lit += ((row[b] >> 4) != 0) + ((row[b] & 15u) != 0); }
    CHECK(lit == WV_W * 2u, "Stille: 2 Pixel je Spalte (Mittellinie), %u", lit);
    for (uint32_t c = 0; c < WV_W; c++) { w->mn[c] = -128; w->mx[c] = 127; }
    lit = 0;
    for (uint32_t y = 0; y < WV_H; y++) { wv_row(w->mn, w->mx, 0, y, row); for (uint32_t b = 0; b < WV_STRIDE; b++) lit += ((row[b] >> 4) != 0) + ((row[b] & 15u) != 0); }
    CHECK(lit >= WV_W * (WV_H - 2u) && lit <= WV_W * WV_H, "Vollaussteuerung: fast ganze Hoehe, %u", lit);
    w->mn[5] = -64; w->mx[5] = 64; uint32_t top = 999, bot = 0;
    for (uint32_t y = 0; y < WV_H; y++) { wv_row(w->mn, w->mx, 0, y, row); if (row[2] & 0x0Fu) { if (y < top) top = y; bot = y; } }
    CHECK(top + bot == WV_H - 1u || top + bot == WV_H, "symmetrisch: Spalte 5 von %u bis %u", top, bot);
    /* 045: 4 Bit: Koerper (RMS) voll, Spitzen blass, Kante weich; RMS einer Sinusspalte */
    { uint8_t rm[WV_W]; for (uint32_t c = 0; c < WV_W; c++) rm[c] = 32;
      for (uint32_t c = 0; c < WV_W; c++) { w->mn[c] = -100; w->mx[c] = 100; }
      uint32_t nb = 0, lv[WV_H], seen = 0, mono = 1;
      for (uint32_t y = 0; y < WV_H; y++) { wv_row(w->mn, w->mx, rm, y, row); lv[y] = row[0] >> 4; nb += lv[y] == 15u; if (lv[y]) seen |= 1u << lv[y]; }
      for (uint32_t y = 1; y < WV_H / 2u; y++) if (lv[y] && lv[y - 1] && lv[y] < lv[y - 1]) mono = 0;   /* oben: zur Mitte hin heller */
      uint32_t nst = (uint32_t)__builtin_popcount(seen);
      CHECK(nb >= 28u && nb <= 32u && nst >= 10u && mono && (seen & (1u << 2)) && (seen & (1u << 12)), "4 Bit: Koerper %u px voll, Verlauf ueber %u Stufen (2…12), monoton", nb, nst);
      int16_t sn[4410]; for (int i = 0; i < 4410; i++) sn[i] = (int16_t)(20000.0 * sin(i * 0.9));
      uint8_t r0 = wv_column_rms(sn, 4410, 3); int8_t a0, b0; wv_column(sn, 4410, 3, &a0, &b0);
      CHECK(r0 >= 50u && r0 <= 60u && b0 > 70, "RMS Sinus 20000 = %u (erwartet ~55), Spitze %d", r0, b0); }
    /* 4. Layout Slicer: Grenzen, Gate, Zyklus */

    sl_reset();
    for (int32_t m = 1; m <= 8; m++) {
        uint32_t n = sl_slices(m); int ok = 1;
        for (uint32_t k = 0; k < 2u * n; k++) {
            w->mode = m; w->v10 = (int32_t)(k << 6); w->v11 = 0x7FFF; w->frames = 88200;
            wv_layout(w);
            uint32_t kk = k % n;
            uint32_t ex0 = (kk * 0x7FFFu / n) * WV_W / 0x7FFFu;
            if (w->nsl != n || w->kidx != kk || w->hl0 != ex0 || w->hl1 < w->hl0 || w->mk > WV_W) ok = 0;
            w->v11 = 0;
            wv_layout(w);
            if (w->br1 != w->br0) ok = 0;
        }
        CHECK(ok, "Layout Slicer %u: Slice-Index zyklisch, Grenzen, Gate 0", n);
    }
    w->mode = 4; w->v10 = 2 << 6; w->v11 = 0x5FFF; wv_layout(w);
    CHECK(w->hl0 == 37 && w->hl1 == 55 && w->mk == 52, "Slice 3/16 Gate 75%%: %u..%u Marke %u", w->hl0, w->hl1, w->mk);
    /* 5. Granular: Position, Fenster, Spray */
    w->mode = SL_GRAN; w->frames = 88200; w->v10 = (int32_t)(0.4 * 0x7FFF); w->v11 = 0x7FFF; w->v13 = 0; w->v14 = 0;
    wv_layout(w);
    CHECK(w->mk == 121 && w->hl0 == 121 && w->hl1 == 121 + 152 && w->sp0 == 0xFFFF, "Granular 40 %%, 1000 ms: Fenster %u..%u", w->hl0, w->hl1);
    w->track = 0; gr_cfg(0)->spray = 100; wv_layout(w);
    CHECK(w->sp0 == 121 - 76 && w->sp1 == WV_W - 1u, "Spray 500 ms: %u..%u", w->sp0, w->sp1);
    w->v10 = 0x7FFF; gr_cfg(0)->spray = 0; wv_layout(w);
    CHECK(w->hl1 <= WV_W - 1u && w->mk <= WV_W, "Granular am Ende: Fenster geklemmt");
    CHECK(wv_f2u(0x472C4400u) == 44100u && wv_f2u(0x46FA0000u) == 32000u && wv_f2u(0) == 0 && wv_f2u(0xC0000000u) == 0, "float → int (Rate)");
    /* 6. Live Rec: Zustandsmaschine */
    volatile lr_t *s = (volatile lr_t *)(void *)host_lr_ram;
    lr_reset();
    CHECK(lr_step(s, 7, 0, 0, 1000, 0) == LR_FWD && lr_step(s, 3, 0, 0, 1000, 1) == LR_FWD, "ohne rec_request bzw. andere Taste: weiter");
    CHECK(lr_step(s, 7, 0, 1, 1000, 1) == LR_FWD && lr_step(s, 7, 1, 1, 1100, 1) == LR_FWD, "Shift + Live Rec (Undo): weiter");
    /* kurz: Druecken zurueckhalten, Wiederholung bei 400 ms schluckt, Loslassen bei 600 → aufgeschoben */
    CHECK(lr_step(s, 7, 0, 0, 2000, 1) == 0, "kurz: Druecken zurueckgehalten");
    CHECK(lr_step(s, 7, 2, 0, 2401, 1) == 0, "kurz: Wiederholung 401 ms geschluckt");
    CHECK(lr_step(s, 7, 1, 0, 2600, 1) == LR_DEFERRED, "kurz: Loslassen → Druecken+Loslassen ans Original");
    /* lang: Wiederholungen wie FUN_000065B4: 401, +300, +200, +150, +100, +80 … */
    lr_step(s, 7, 0, 0, 10000, 1);
    uint32_t t = 10000, dt[] = { 401, 300, 200, 150, 100, 80, 80, 80 }, toggles = 0, at = 0;
    for (uint32_t i = 0; i < 8; i++) { t += dt[i]; uint32_t r = lr_step(s, 7, 2, 0, t, 1); if (r & LR_TOGGLE) { toggles++; at = t - 10000; } else if (r) toggles += 100; }
    CHECK(toggles == 1 && at >= 1000 && at < 1200, "lang: genau eine Umschaltung, nach %u ms", at);
    CHECK(lr_step(s, 7, 1, 0, t + 5, 1) == 0, "lang: Loslassen geschluckt (kein Live Rec)");
    CHECK(s->n_long == 1 && s->n_short == 1, "Zaehler");
    CHECK(lr_step(s, 7, 1, 0, t + 50, 1) == LR_FWD && lr_step(s, 7, 2, 0, t + 60, 1) == LR_FWD, "Loslassen/Wiederholung ohne Druecken: weiter");
    /* 7. Statustext */
    char b[24];
    lr_text(b, 2, 37, 0); CHECK(!strcmp(b, "REC 0:03"), "Text %s", b);
    lr_text(b, 2, 6015, 1); CHECK(!strcmp(b, "REC 10:01 !"), "Text %s", b);
    lr_text(b, 1, 0, 0); CHECK(!strcmp(b, "REC ..."), "Text %s", b);
    lr_text(b, 3, 655, 0); CHECK(!strcmp(b, "REC 1:05 OK"), "Text %s", b);
    CHECK(lr_bars(0) == 0 && lr_bars(129) == 0 && lr_bars(130) == 1 && lr_bars(16423) == 8 && lr_bars(32767) == 10, "045: Pegelbalken -48…0 dB");
    free(d);
    printf("%d Pruefungen, %d Fehler\n", checks, fails);
    return fails ? 1 : 0;
}
