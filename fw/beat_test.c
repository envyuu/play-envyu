/* Patch 032: Beat-Erkennung gegen bekannte Anschlaege: Testbeat (120 BPM, Viertelraster) und Live-Aufnahme des Nutzers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "slview.h"
int wv_sample_data(uint32_t t, uint32_t *a, uint32_t *b, uint32_t *c) { (void)t; (void)a; (void)b; (void)c; return 0; }
void wv_cur_name(char *d, uint32_t m) { (void)m; d[0] = 0; }
int32_t wv_touched_gate(void) { return 0x7FFF; }
int32_t wv_touched_note(void) { return 60; }
int32_t wv_touched_slice(uint32_t n) { (void)n; return 0; }
int32_t sl_dummy;
#include "slicer.h"
static gr_cfg_t cfg_[8];
gr_cfg_t *gr_cfg(uint32_t t) { return &cfg_[t & 7u]; }
void gr_cfg_default(uint32_t t) { (void)t; }
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FEHLER "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int16_t *load(const char *p, uint32_t maxs, uint32_t *fr, int *rate) {
    FILE *f = fopen(p, "rb"); if (!f) { printf("fehlt: %s\n", p); exit(1); }
    fseek(f, 0, 2); long z = ftell(f); fseek(f, 0, 0); uint8_t *d = malloc(z); if (fread(d, 1, z, f) != (size_t)z) exit(1); fclose(f);
    long o = 12; int ch = 1, sr = 44100; uint32_t dl = 0; uint8_t *q = 0;
    while (o + 8 < z) { uint32_t l; memcpy(&l, d + o + 4, 4);
        if (!memcmp(d + o, "fmt ", 4)) { int16_t c; int32_t r; memcpy(&c, d + o + 10, 2); memcpy(&r, d + o + 12, 4); ch = c; sr = r; }
        if (!memcmp(d + o, "data", 4)) { q = d + o + 8; dl = l; break; }
        o += 8 + l + (l & 1); }
    uint32_t n = dl / 2 / ch; if (maxs && n > maxs * (uint32_t)sr) n = maxs * sr;
    int16_t *m = malloc(n * 2); for (uint32_t i = 0; i < n; i++) { int s = 0; for (int k = 0; k < ch; k++) { int16_t v; memcpy(&v, q + 2 * (i * ch + k), 2); s += v; } m[i] = (int16_t)(s / ch); }
    free(d); *fr = n; *rate = sr; return m;
}
static void run(const char *name, const char *path, uint32_t maxs, const double *truth, int nt, double tol_ms) {
    uint32_t n; int sr; int16_t *m = load(path, maxs, &n, &sr);
    sv_x_t *x = sv_xget(1);
    sv_scan_start(x, 0, (uint32_t)(uintptr_t)m, n, (uint32_t)sr);
    int polls = 0; while (!sv_scan_run(x, 32)) polls++;
    sv_beat_t *b = &x->b[0];
    uint32_t last = 0; uint16_t prev[65]; uint32_t pn = 0;
    for (uint32_t L = 1; L <= 16; L++) {
        sv_beat_pick(b, L);
        CHECK(b->nt >= last, "%s: Stufe %u nicht feiner", name, L); last = b->nt;
        for (uint32_t i = 0; i < pn; i++) {                 /* verschachtelt: was einmal Slice ist, bleibt Slice */
            int ok = 0; for (uint32_t j = 0; j < b->nt; j++) if (b->tab[j] == prev[i]) ok = 1;
            CHECK(ok, "%s: Stufe %u verliert Slice-Beginn %u", name, L, prev[i]);
        }
        pn = b->nt; memcpy(prev, (const void *)b->tab, pn * 2);
        if (L == 1 || L == 8 || L == 16) printf("  %s Sensitivity %u: %u Slices\n", name, L, b->nt);
    }
    sv_beat_pick(b, 16);
    CHECK(b->nt == nt, "%s: %u Slices statt %d", name, b->nt, nt);
    double worst = 0;
    for (int i = 0; i < nt && i < b->nt; i++) {
        double t = (double)b->tab[i] * n / 0x7FFF / sr, e = fabs(t - truth[i]) * 1000;
        if (e > worst) worst = e;
        CHECK(e <= tol_ms, "%s: Slice %d bei %.4f s, wahr %.3f s", name, i + 1, t, truth[i]);
    }
    printf("  %s: %u Slices, %d Durchlaeufe, groesste Abweichung %.1f ms\n", name, b->nt, polls, worst);
    free(m);
}
int main(int c, char **v) {
    (void)c;
    sv_reset();
    double tb[16]; for (int i = 0; i < 16; i++) tb[i] = i * 0.25;
    run("Testbeat 4 s", v[1], 4, tb, 16, 3.0);
    { sv_x_t *x = sv_xget(1); sv_beat_t *b = &x->b[0];
      uint32_t n; int sr; int16_t *m = load(v[1], 4, &n, &sr);
      sv_scan_start(x, 0, (uint32_t)(uintptr_t)m, n, (uint32_t)sr); while (!sv_scan_run(x, 32)) {}
      sv_beat_pick(b, 1);
      for (uint32_t i = 1; i < b->nt; i++) { double t = (double)b->tab[i] * n / 0x7FFF / sr; CHECK(fabs(fmod(t + 0.1, 1.0) - 0.1) < 0.01, "Stufe 1: nur die lautesten Schlaege (volle Sekunden), %.3f s", t); }
      CHECK(b->nt >= 2, "Stufe 1 findet etwas (%u)", b->nt);
      free(m); }
    double rec[10]; for (int i = 1; i < 10; i++) rec[i] = 0.266 + (i - 1) * 0.25; rec[0] = 0;
    run("Rec_001", v[2], 0, rec, 10, 3.0);
    /* 6 min am Stueck: laeuft durch, Durchlaeufe begrenzt */
    { uint32_t n; int sr; int16_t *m = load(v[1], 0, &n, &sr); sv_x_t *x = sv_xget(1);
      sv_scan_start(x, 1, (uint32_t)(uintptr_t)m, n, (uint32_t)sr); int p = 0; while (!sv_scan_run(x, 32)) p++;
      sv_beat_pick(&x->b[1], 16);
      CHECK(p < 3000 && x->b[1].nt == SL_MAXSLICES, "6 min (045: genau die Obergrenze): %d Durchlaeufe, %u Slices", p, x->b[1].nt);
      for (uint32_t i = 0; i < x->b[1].nt; i++) { double t = (double)x->b[1].tab[i] * n / 0x7FFF / sr; double r = fmod(t + 0.125, 0.25) - 0.125; CHECK(fabs(r) < 0.02, "6 min: Slice %u bei %.3f s liegt nicht im Raster", i, t); }
      printf("  6 min: %d Durchlaeufe, %u Slices\n", p, x->b[1].nt); free(m); }
    printf("%d Pruefungen, %d Fehler\n", checks, fails);
    return fails ? 1 : 0;
}
int sl_slice_outside(uint32_t t, uint32_t k, uint32_t n) { (void)t; (void)k; (void)n; return 0; }   /* 045: sv_fit_count (Stub) */
