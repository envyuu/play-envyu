/* PC-Test fuer nis.c (Patch 018 „Note In Scale“), mit -DHOST_TEST -fsanitize=address,undefined.
 * usage: nis_test ../stock/firmware.bin   (Scale-Masken 0xAC838 und Namen 0x9F704 aus dem echten Image) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

uint8_t  host_nis_ram[256] __attribute__((aligned(8)));
uint32_t host_scale_type, host_scale_root;
uint16_t host_scale_masks_rw[39];
static int strncpy_calls;
void host_strncpy(char *d, const char *s, uint32_t n) { strncpy_calls++; strncpy(d, s, n); }

/* aus nis.c */
typedef struct { const void *vt; uint32_t mode; } gen_t;
typedef struct { int32_t idx; uint32_t range; const void *gen; } triple_t;
typedef struct { const char *name; triple_t t[8]; } rec_t;
extern const rec_t NIS_LIST[6];
int nis_gen(const gen_t *gen, const void *triple, int cand, int orig, uint32_t pct_bits, uint32_t *seed);
uint32_t nis_q16(uint32_t bits);
void nis_refresh(void);
void nis_name_copy(char *dst, const char *src, uint32_t n);
const char *host_nis_name(int v);
uint32_t host_nis_stat(int k);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FEHLER "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static uint8_t *fw; static long fwlen;
static const char *fwstr(uint32_t a) { return a < (uint32_t)fwlen ? (const char *)fw + a : "?"; }
static uint32_t fw32(uint32_t a) { uint32_t v; memcpy(&v, fw + a, 4); return v; }
static int inscale(uint32_t mask, int root, int n) { int d = ((n - root) % 12 + 12) % 12; return (mask >> d) & 1; }
static uint32_t eff_mask(uint32_t t) { uint32_t m = host_scale_masks_rw[t] & 0xFFF; return (t == 0 || !m) ? 0xAB5 : m; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s firmware.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); fwlen = ftell(f); rewind(f); fw = malloc(fwlen);
    if (fread(fw, 1, fwlen, f) != (size_t)fwlen) return 2;
    fclose(f);
    for (int i = 0; i < 39; i++) host_scale_masks_rw[i] = (uint16_t)(fw[0xAC838 + 2 * i] | fw[0xAC839 + 2 * i] << 8);
    memset(host_nis_ram, 0xA5, sizeof host_nis_ram);                     /* Muell wie nach dem Einschalten */
    const gen_t *G0 = NIS_LIST[4].t[0].gen, *G1 = NIS_LIST[5].t[0].gen;

    printf("== 1. Liste (Aufbau wie Randomize_initTypes, 6 x 0x64 Byte)\n");
    CHECK(sizeof(rec_t) == 0x64 || sizeof(void *) != 4, "Datensatzgroesse");
    int idx[6] = { 2, 6, 6, 0, 6, 6 };
    for (int e = 0; e < 6; e++) {
        CHECK(NIS_LIST[e].t[0].idx == idx[e], "Eintrag %d Index", e);
        for (int k = 1; k < 8; k++) CHECK(NIS_LIST[e].t[k].idx == -1 && NIS_LIST[e].t[k].range == 0 && !NIS_LIST[e].t[k].gen, "Eintrag %d Ende %d", e, k);
    }
    CHECK(G0 && G1 && G0->mode == 0 && G1->mode == 1 && G0->vt == G1->vt, "Generator-Objekte");
    CHECK(NIS_LIST[4].t[0].range == 0x000CFFF4u && NIS_LIST[5].t[0].range == 0x000CFFF4u, "Bereich ±12");

    printf("== 2. Prozent-Umrechnung (IEEE-Bits → 1/65536)\n");
    int bad = 0;
    for (int k = 0; k <= 100; k++) {
        float p = (float)k * 0.01f; uint32_t q = nis_q16(fbits(p));
        double want = p * 65536.0; if (q + 1.0 < want || q > want + 1.0) bad++;
    }
    CHECK(!bad, "0…100 %% in 1-%%-Schritten (%d Abweichungen)", bad);
    CHECK(nis_q16(fbits(0.0f)) == 0 && nis_q16(fbits(-0.0f)) == 0 && nis_q16(fbits(-0.5f)) == 0, "0 / negativ");
    CHECK(nis_q16(fbits(1.0f)) == 65536 && nis_q16(fbits(3.0f)) == 65536 && nis_q16(0x7FC00000u) == 65536, "≥1, NaN");
    CHECK(nis_q16(fbits(1e-30f)) == 0, "winzig");

    printf("== 3. Alle Scales × Roots × Noten: Ergebnis in der Scale, im Fenster, ≠ Original\n");
    uint32_t seed; bad = 0; int badw = 0, same = 0, n_changed = 0, n_total = 0;
    for (uint32_t t = 0; t < 39; t++) for (uint32_t r = 0; r < 12; r++) {
        host_scale_type = t; host_scale_root = r;
        uint32_t m = eff_mask(t);
        for (int o = 0; o <= 127; o++) for (int s = 0; s < 4; s++) {
            seed = 0x1234567u * (uint32_t)(o + 1) + (uint32_t)s * 7919u + t * 131u + r;
            int v = nis_gen(G0, 0, o, o, fbits(1.0f), &seed); n_total++;
            if (v == o) {
                int lo = o - 12 < 12 ? 12 : o - 12, hi = o + 12 > 119 ? 119 : o + 12, cand = 0;
                for (int n = lo; n <= hi; n++) if (n != o && inscale(m, (int)r, n)) cand++;
                if (cand) same++; else n_total--;                               /* Randnote ohne Kandidat: bleibt zu Recht */
                continue;
            }
            n_changed++;
            if (!inscale(m, (int)r, v)) bad++;
            int lo = o - 12 < 12 ? 12 : o - 12, hi = o + 12 > 119 ? 119 : o + 12;
            if (v < lo || v > hi) badw++;
        }
    }
    CHECK(!bad, "ausserhalb der Scale: %d", bad);
    CHECK(!badw, "ausserhalb des Fensters ±12 / 12…119: %d", badw);
    CHECK(same == 0, "bei 100 %% unveraendert geblieben: %d von %d", same, n_total);
    CHECK(n_changed == n_total, "geaendert %d/%d", n_changed, n_total);

    printf("== 4. Anteil geaenderter Steps ≈ Prozent, monoton steigend, Note je Step stabil\n");
    host_scale_type = 3; host_scale_root = 2;                                   /* D Dorian */
    const int N = 20000; int *note_at = malloc(sizeof(int) * N);
    int prev = -1, mono_bad = 0, stab_bad = 0;
    for (int k = 0; k <= 100; k += 5) {
        int ch = 0;
        for (int i = 0; i < N; i++) {
            seed = (uint32_t)i * 2654435761u + 17u; int o = 48 + i % 24;
            int v = nis_gen(G0, 0, o, o, fbits((float)k * 0.01f), &seed);
            if (v != o) {
                ch++;
                if (k > 0 && prev >= 0 && note_at[i] >= 0 && note_at[i] != v) stab_bad++;
                note_at[i] = v;
            } else {
                if (k > 0 && note_at[i] >= 0) mono_bad++;                       /* war schon geaendert, jetzt nicht mehr */
                if (k == 0) note_at[i] = -1;
            }
        }
        double fr = (double)ch / N;
        CHECK(fr > k * 0.01 - 0.015 && fr < k * 0.01 + 0.015, "%3d %%: Anteil %.3f", k, fr);
        prev = ch;
    }
    CHECK(!mono_bad, "monoton (%d Steps fielen beim Hochdrehen zurueck)", mono_bad);
    CHECK(!stab_bad, "Note je Step unabhaengig vom Prozentwert (%d Abweichungen)", stab_bad);
    free(note_at);

    printf("== 5. Variante „+“: bis 50 %% nie ausserhalb, bei 100 %% ≈ 25 %% der Aenderungen ausserhalb\n");
    host_scale_type = 2; host_scale_root = 0;                                   /* C Major */
    for (int k = 10; k <= 100; k += 10) {
        int ch = 0, out = 0;
        for (int i = 0; i < N; i++) {
            seed = (uint32_t)i * 40503u + 99u; int o = 60;
            int v = nis_gen(G1, 0, o, o, fbits((float)k * 0.01f), &seed);
            if (v != o) { ch++; if (!inscale(0xAB5, 0, v)) out++; }
        }
        double want = k <= 50 ? 0.0 : (k - 50) / 200.0, got = ch ? (double)out / ch : 0;
        CHECK(got >= want - 0.02 && got <= want + 0.02 && (k > 50 || out == 0), "%3d %%: ausserhalb %.3f (Soll %.3f)", k, got, want);
    }
    {   /* Variante 0 nie ausserhalb */
        int out = 0;
        for (int i = 0; i < N; i++) { seed = (uint32_t)i * 40503u + 99u; int v = nis_gen(G0, 0, 60, 60, fbits(1.0f), &seed); if (!inscale(0xAB5, 0, v)) out++; }
        CHECK(!out, "Variante 0 bei 100 %%: nie ausserhalb (%d)", out);
    }

    printf("== 6. Randfaelle\n");
    host_scale_type = 99; host_scale_root = 77;                                 /* Muell → Chromatic/C → Dur */
    seed = 5; int v = nis_gen(G0, 0, 60, 60, fbits(1.0f), &seed);
    CHECK(v != 60 && inscale(0xAB5, 0, v), "ungueltige Scale → C Dur (%d)", v);
    v = nis_gen(G0, 0, 60, 60, fbits(1.0f), 0);
    CHECK(v >= 48 && v <= 72, "kein Zustandszeiger (%d)", v);
    seed = 0; v = nis_gen(G0, 0, 60, 60, fbits(1.0f), &seed);
    CHECK(v >= 48 && v <= 72, "Zustand 0 (%d)", v);
    seed = 0xFFFFFFFFu; v = nis_gen(G0, 0, 60, 60, fbits(1.0f), &seed);
    CHECK(v >= 48 && v <= 72, "Zustand 0xFFFFFFFF (%d)", v);
    host_scale_type = 0; host_scale_root = 0;
    for (int o = -5; o <= 140; o++) { seed = 3; v = nis_gen(G0, 0, o, o, fbits(1.0f), &seed); if (v != o && (v < 12 || v > 119)) { CHECK(0, "Note %d → %d", o, v); break; } }
    seed = 3; v = nis_gen(G0, 0, 60, 60, fbits(0.0f), &seed); CHECK(v == 60, "0 %% → unveraendert");
    CHECK(seed == 3, "Zustand des Aufrufers wird nicht veraendert");
    seed = 3; v = nis_gen(0, 0, 60, 60, fbits(1.0f), &seed); CHECK(v != 60, "ohne Objekt (Modus 0)");

    printf("== 7. Typnamen (Anzeige, ≤ 16 Zeichen)\n");
    host_scale_type = 3; host_scale_root = 0; nis_refresh();
    CHECK(!strcmp(host_nis_name(0), "In C Dorian") && !strcmp(host_nis_name(1), "In C Dorian+"), "C Dorian: '%s' '%s'", host_nis_name(0), host_nis_name(1));
    host_scale_type = 0; host_scale_root = 9; nis_refresh();
    CHECK(!strcmp(host_nis_name(0), "In A Major"), "Chromatic → '%s'", host_nis_name(0));
    host_scale_type = 9; host_scale_root = 6; nis_refresh();
    CHECK(!strcmp(host_nis_name(0), "In F# Mixolydian") && !strcmp(host_nis_name(1), "F# Mixolydian+"), "lang: '%s' '%s'", host_nis_name(0), host_nis_name(1));
    int maxlen = 0; bad = 0;
    for (uint32_t t = 0; t < 39; t++) for (uint32_t r = 0; r < 12; r++) {
        host_scale_type = t; host_scale_root = r; nis_refresh();
        for (int k = 0; k < 2; k++) { int l = (int)strlen(host_nis_name(k)); if (l > maxlen) maxlen = l; if (l > 16 || l < 4) bad++; }
    }
    CHECK(!bad && maxlen <= 16, "alle 39×12×2 Namen 4…16 Zeichen (max %d)", maxlen);
    /* Kurzname passt zum Firmware-Namen (gleiches erstes Wort bzw. Abkuerzung) */
    bad = 0;
    for (int t = 1; t < 39; t++) {
        host_scale_type = (uint32_t)t; host_scale_root = 0; nis_refresh();
        const char *fn = fwstr(fw32(0x9F704 + 4 * t)), *mine = host_nis_name(0);
        mine += !strncmp(mine, "In ", 3) ? 3 : 0; mine += 2;                    /* "C " */
        if (strncasecmp(fn, mine, 1)) { bad++; printf("    %2d %-18s ↔ %s\n", t, fn, mine); }
    }
    CHECK(!bad, "Kurznamen beginnen wie die Firmware-Namen");
    for (int t = 0; t < 39; t += 1) { host_scale_type = (uint32_t)t; host_scale_root = 1; nis_refresh(); if (t % 13 == 0) printf("    Beispiel %2d %-18s → '%s' / '%s'\n", t, fwstr(fw32(0x9F704 + 4 * t)), host_nis_name(0), host_nis_name(1)); }
    char dst[34]; memset(dst, 'x', sizeof dst);
    host_scale_type = 1; host_scale_root = 4; strncpy_calls = 0;
    nis_name_copy(dst, NIS_LIST[4].name, 33);
    CHECK(!strcmp(dst, "In E Minor") && strncpy_calls == 1, "Anzeige-Hook frischt auf: '%s'", dst);
    host_scale_type = 1; host_scale_root = 5;
    nis_name_copy(dst, NIS_LIST[1].name, 33);
    CHECK(!strcmp(dst, "1 Octave") && !strcmp(host_nis_name(0), "In E Minor"), "fremder Name unveraendert durchgereicht");
    nis_name_copy(dst, NIS_LIST[5].name, 33);
    CHECK(!strcmp(dst, "In F Minor+"), "Eintrag 5: '%s'", dst);
    CHECK(host_nis_stat(0) > 0 && host_nis_stat(1) > 0 && host_nis_stat(2) > 0, "Diagnosezaehler laufen");

    printf("%s: %d Pruefungen, %d Fehler\n", fails ? "FEHLER" : "OK", checks, fails);
    free(fw);
    return fails != 0;
}
