/* PC-Test fuer Patch 021 (slicer.c + granular.c): Slice-Rechnung, Hook-Logik, Anzeige, Menue, /engines.cfg,
 * Granular-DSP (Fenstersumme, Grenzen, Richtungen, Umrechnungen). 32 Bit wie das Geraet, ASan/UBSan.
 * gcc -m32 -O1 -g -DHOST_TEST -fsanitize=address,undefined -o sl_test slicer.c granular.c sl_test.c && ./sl_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "slicer.h"
extern int32_t host_menu_kind[8];
void sl_kind_apply(void);
#include "slview.h"
extern uint8_t host_sv_ram[0xC0];

typedef struct { uint32_t store[2]; uint32_t manager; uint32_t invoker; } stdfn_t;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FEHLER "); printf(__VA_ARGS__); printf("\n"); } } while (0)

uint8_t host_sl_ram[0x100] __attribute__((aligned(8)));
uint8_t host_gr_ram[0x200] __attribute__((aligned(8)));
uint8_t host_wv_ram[0x3B0] __attribute__((aligned(8)));   /* Patch 023 */
uint8_t host_lr_ram[0xB0] __attribute__((aligned(8)));
uint8_t host_param_tab[22 * 0x20] __attribute__((aligned(8)));
uint32_t host_millis = 0;
static uint8_t patmem[2][0x10 + 16 * 0xBA8] __attribute__((aligned(4)));
uintptr_t host_pat0, host_pat1;
#define S ((sl_t *)(void *)host_sl_ram)

/* ---- Wiedergabe-Attrappen ---- */
static int32_t step_vals[22]; static int step_set[22];
static int getparam_calls[22];
uint32_t host_get_param(int *step, uint32_t idx, int *ctx, uint8_t *ev) {
    (void)step; (void)ctx; (void)ev;
    getparam_calls[idx]++;
    return (uint32_t)(int32_t)(int16_t)(step_set[idx] ? step_vals[idx] : (idx == 11 ? 0x7FFF : 0));
}
static int32_t slots[13][4]; static int slot_writes;
static uint32_t node_obj[4];
void *host_node_slot(void *node, uint32_t i) { if (node != (void *)node_obj || i >= 13) { printf("falscher Knoten/Slot\n"); exit(1); } return slots[i]; }
void host_slot_set(void *slot, uint32_t src, int32_t v) { ((int32_t *)slot)[src] = v; slot_writes++; }
static char orig_last[64]; static void *orig_self; static int orig_calls;
void host_orig_fmt(void *self, int32_t value, int32_t rel, char *buf, uint32_t size, void *step) {
    (void)step; orig_calls++; orig_self = self;
    snprintf(buf, size, rel ? "%+0.1f %%" : "%0.1f %%", value * 0.003051851);
    snprintf(orig_last, sizeof orig_last, "%s", buf);
}
static int arp_inits;
static uint32_t fmt_obj10[2] = { 0xA0338, 0x11 }, fmt_obj11[2] = { 0xA0338, 0x22 };
void host_arp_init(void) {
    arp_inits++;
    memset(host_param_tab, 0, sizeof host_param_tab);
    ((uint32_t *)(host_param_tab + 0x20 * 10))[4] = (uint32_t)(uintptr_t)fmt_obj10;
    ((uint32_t *)(host_param_tab + 0x20 * 11))[4] = (uint32_t)(uintptr_t)fmt_obj11;
}

/* ---- Menue-Attrappen ---- */
typedef struct { const char *name; int32_t *val; uint32_t max; const char *const *tab; stdfn_t fn; void *parent; } enum_rec_t;
static enum_rec_t enums[32]; static int nenum;
typedef struct { const char *name; uint32_t fn; void *parent; } menu_rec_t;
static menu_rec_t menus[8]; static int nmenu;
static uint8_t heap_pool[64][0x60]; static int heap_n;
void *host_heap_alloc(uint32_t n) { if (n > 0x60 || heap_n >= 64) return 0; return memset(heap_pool[heap_n++], 0, n); }
void *host_menu_add(void *it, void *parent, const char *name, uint32_t fn, uint32_t adj) {
    (void)adj; menus[nmenu].name = name; menus[nmenu].fn = fn; menus[nmenu].parent = parent; nmenu++; return it;
}
void *host_enum_item(void *it, void *parent, const char *name, int32_t *val, uint32_t max, const char *const *tab, stdfn_t *fn) {
    enums[nenum].name = name; enums[nenum].val = val; enums[nenum].max = max; enums[nenum].tab = tab; enums[nenum].fn = *fn;
    enums[nenum].parent = parent; nenum++; return it;
}

/* ---- FatFs-Attrappe (024: mehrere Dateien nach Pfad) ---- */
#define NF 16
static struct { char path[320]; char buf[1024]; int len; } files[NF];
static int sd_fail_write = 0, mallocs = 0, writes = 0, opens = 0;
typedef struct { int mode, pos, w, idx; char wb[1024]; } hfil_t;
void *host_malloc(uint32_t n) { mallocs++; return calloc(1, n); }
void host_free(void *p) { mallocs--; free(p); }
static int fidx(const char *path, int create) {
    for (int i = 0; i < NF; i++) if (files[i].len >= 0 && files[i].path[0] && !strcmp(files[i].path, path)) return i;
    if (!create) return -1;
    for (int i = 0; i < NF; i++) if (!files[i].path[0]) { snprintf(files[i].path, sizeof files[i].path, "%s", path); files[i].len = 0; return i; }
    return -1;
}
static void fs_clear(void) { memset(files, 0, sizeof files); }
static void fs_put(const char *path, const char *txt) { int i = fidx(path, 1); files[i].len = (int)strlen(txt); memcpy(files[i].buf, txt, (size_t)files[i].len); }
static const char *fs_get(const char *path) { int i = fidx(path, 0); if (i < 0) return 0; files[i].buf[files[i].len] = 0; return files[i].buf; }
int host_f_open(void *fp, const char *path, uint8_t mode) {
    opens++;
    if (strlen(path) > 300) { printf("Pfad zu lang\n"); exit(1); }
    int i = fidx(path, mode != 1);
    if (mode == 1 && i < 0) return 4;
    hfil_t *h = calloc(1, sizeof *h); h->mode = mode; h->idx = i; *(hfil_t **)fp = h; return 0;
}
int host_f_read(void *fp, void *buf, uint32_t n, uint32_t *br) {
    hfil_t *h = *(hfil_t **)fp; uint32_t k = (uint32_t)files[h->idx].len - (uint32_t)h->pos; if (k > n) k = n;
    memcpy(buf, files[h->idx].buf + h->pos, k); h->pos += (int)k; *br = k; return 0;
}
int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw) {
    hfil_t *h = *(hfil_t **)fp; if (sd_fail_write) { *bw = 0; return 1; }
    memcpy(h->wb + h->w, buf, n); h->w += (int)n; *bw = n; writes++; return 0;
}
int host_f_close(void *fp) {
    hfil_t *h = *(hfil_t **)fp;
    if (h->mode != 1 && !sd_fail_write) { memcpy(files[h->idx].buf, h->wb, (size_t)h->w); files[h->idx].len = h->w; }
    free(h); return 0;
}

static void run_ms(uint32_t ms) { for (uint32_t i = 0; i < ms; i += 10) { host_millis += 10; sl_poll(); } }

/* ---- 1. Slice-Rechnung ---- */
static const uint32_t NS[8] = { 4, 8, 12, 16, 24, 32, 48, 64 };
static void test_bounds(void) {
    printf("== 1. Slice-Grenzen\n");
    int c0 = checks, f0 = fails;
    for (int m = 1; m <= 8; m++) CHECK(sl_slices(m) == NS[m - 1], "Modus %d", m);
    CHECK(sl_slices(0) == 0 && sl_slices(9) == 0 && sl_slices(11) == 0 && sl_slices(12) == 0 && sl_slices(-1) == 0, "Normal/Granular ohne Slices");
    for (int i = 0; i < 8; i++) {
        uint32_t n = NS[i]; int32_t ps = -1, pe = -1;
        for (uint32_t k = 0; k < n; k++) {
            int32_t s, e; sl_slice_bounds(k, n, 0x7FFF, &s, &e);
            if (k == 0) CHECK(s == 0, "Slice 1 beginnt bei 0");
            if (k == n - 1) CHECK(e == 0x7FFF, "letzter Slice endet bei 0x7FFF (= ganze Laenge)");
            if (k) CHECK(s == pe, "lueckenlos N=%u k=%u", n, k);
            CHECK(e > s && s > ps, "aufsteigend");
            ps = s; pe = e;
            int32_t s2, e2; sl_slice_bounds(k, n, 0, &s2, &e2); CHECK(s2 == s && e2 == s, "Gate 0 → Laenge 0");
            sl_slice_bounds(k, n, 1, &s2, &e2); CHECK(e2 == s + 1, "Gate 1 → mindestens 1 Einheit");
            sl_slice_bounds(k, n, 0x4000, &s2, &e2); CHECK(e2 > s && e2 < e, "Gate 50 %% im Slice");
            /* Frames wie FUN_000166F4: len*u/0x7FFF; Abweichung zur idealen Grenze k*len/n */
            static const uint32_t LENS[4] = { 4410, 176400, 2646000, 15876000 };   /* 0,1 s / 4 s / 1 min / 6 min */
            for (int l = 0; l < 4; l++) {
                uint64_t L = LENS[l], fs = L * (uint32_t)s / 0x7FFFu;
                double ideal = (double)k * L / n;
                CHECK(fs <= ideal + 1e-9 && ideal - fs <= L / 32767.0 + 1.0, "Abweichung N=%u k=%u L=%llu: %f", n, k, (unsigned long long)L, ideal - fs);
            }
        }
        /* Rasten: Wert k*64 → Slice k mod n */
        for (int32_t v = 0; v <= 0x7FFF; v += 17) CHECK(sl_slice_index(v, n) == ((uint32_t)v / 64u) % n, "Index");
        CHECK(sl_slice_index(-5, n) == 0, "negativ → Slice 1");
    }
    printf("  %d Pruefungen, %d Fehler\n", checks - c0, fails - f0);
}

/* ---- 2. Hook-Logik ---- */
static int ctx[16];
static void trig(uint32_t t, int set10, int32_t v10, int set11, int32_t v11, int32_t *st, int32_t *en) {
    int stepobj = 0;
    memset(step_set, 0, sizeof step_set);
    step_set[10] = set10; step_vals[10] = v10; step_set[11] = set11; step_vals[11] = v11;
    ctx[0] = (int)(uintptr_t)node_obj; ctx[2] = (int)t;
    sl_begin(&stepobj, ctx, 0);
    /* Reihenfolge wie MidiTrack_resolveEvent: Index aufsteigend, nur gesetzte Parameter */
    if (set10) slots[8][1] = (int32_t)sl_param_hook(&stepobj, 10, ctx, 0);
    if (set11) slots[9][1] = (int32_t)sl_param_hook(&stepobj, 11, ctx, 0);
    *st = slots[8][1]; *en = slots[9][1];
}
static void test_hook(void) {
    printf("== 2. Hook (sl_begin / sl_param_hook)\n");
    int c0 = checks, f0 = fails;
    sl_reset();
    int32_t st, en, s, e;
    /* Normal: Werte unveraendert, keine Slot-Schreibzugriffe */
    slot_writes = 0;
    trig(2, 1, 12345, 1, 23456, &st, &en);
    CHECK(st == 12345 && en == 23456 && slot_writes == 0, "Normal: unveraendert");
    for (uint32_t t = 0; t < 8; t++) {
        for (int m = 1; m <= 8; m++) {
            S->mode[t] = m; uint32_t n = NS[m - 1];
            for (uint32_t k = 0; k < 2 * n + 2; k++) {
                int32_t v10 = (int32_t)(k * 64 + (k % 2 ? 32 : 0));   /* 045: untere 6 Bit = Feinversatz, 0/32/63 = keiner */
                trig(t, 1, v10, 1, 0x7FFF, &st, &en);
                sl_slice_bounds(k % n, n, 0x7FFF, &s, &e);
                CHECK(st == s && en == e, "Start+Ende gesetzt t=%u N=%u k=%u", t, n, k);
                slots[9][1] = -1;
                trig(t, 1, v10, 0, 0, &st, &en);
                CHECK(st == s && en == e, "nur Start → Ende = ganzer Slice");
                trig(t, 1, v10, 1, 0x2000, &st, &en);
                sl_slice_bounds(k % n, n, 0x2000, &s, &e);
                CHECK(st == s && en == e, "Gate 25 %%");
            }
            memset(getparam_calls, 0, sizeof getparam_calls);
            slots[8][1] = -7;
            trig(t, 0, 0, 1, 0x4000, &st, &en);
            sl_slice_bounds(0, n, 0x4000, &s, &e);
            CHECK(st == s && en == e && getparam_calls[10] == 1, "nur Ende → Start aus dem Standardwert (1 Abfrage)");
            memset(getparam_calls, 0, sizeof getparam_calls);
            trig(t, 1, 300, 1, 0x4000, &st, &en);
            CHECK(getparam_calls[10] == 1 && getparam_calls[11] == 1, "Start nur einmal abgefragt (Zufall bleibt gleich)");
        }
        S->mode[t] = 0;
    }
    /* falscher Kanal / falsche Magie */
    S->mode[3] = 4; trig(9, 1, 640, 1, 0x7FFF, &st, &en); CHECK(st == 640 && en == 0x7FFF, "Kanal 9 → unveraendert");
    S->magic = 0; trig(3, 1, 640, 1, 0x7FFF, &st, &en); CHECK(st == 640, "ohne Magie → unveraendert");
    sl_reset();
    S->mode[5] = 9; trig(5, 1, 640, 1, 0x1234, &st, &en); CHECK(st == 640 && en == 0x1234, "Granular → Rohwerte");
    CHECK(S->node[5] == (uint32_t)(uintptr_t)node_obj, "Knoten gemerkt");
    printf("  %d Pruefungen, %d Fehler\n", checks - c0, fails - f0);
}

/* ---- 3. Anzeige ---- */
static uintptr_t stp(int p, uint32_t t, uint32_t i) { return (uintptr_t)(patmem[p] + 0x10 + t * 0xBA8 + 0x128 + 42 * i); }
typedef void (*fmt_fn)(void *, int32_t, int32_t, char *, uint32_t, void *);
static const char *fmt(uint32_t idx, int32_t v, int32_t rel, uintptr_t step) {
    static char b[32]; memset(b, 'x', sizeof b);
    uint32_t *obj = (uint32_t *)(uintptr_t)((uint32_t *)(host_param_tab + 0x20 * idx))[4];
    fmt_fn f = (fmt_fn)(uintptr_t)((uint32_t *)(uintptr_t)obj[0])[0];
    f(obj, v, rel, b, sizeof b, (void *)step);
    return b;
}
static void test_format(void) {
    printf("== 3. Anzeige\n");
    int c0 = checks, f0 = fails;
    host_pat0 = (uintptr_t)patmem[0]; host_pat1 = (uintptr_t)patmem[1];
    arp_inits = 0; sl_init();
    CHECK(arp_inits == 1 && S->fmt_ok == 1, "sl_init ruft arp_init und installiert die Formatter");
    CHECK(S->fmt10[1] == 0x11 && S->fmt11[1] == 0x22, "Objekte uebernehmen Wort 1");
    CHECK(S->vt[1] == 0x2A9D1 && S->vt[2] == 0x2AAE9 && S->vt[3] == 0x2A8B1, "vtable-Rest wie 0xA0338");
    for (int p = 0; p < 2; p++) for (uint32_t t = 0; t < 8; t++) for (uint32_t i = 0; i < 64; i++)
        CHECK(sl_step_track(stp(p, t, i)) == t, "Spur aus Step-Adresse p=%d t=%u i=%u", p, t, i);
    CHECK(sl_step_track(stp(0, 0, 0) - 42) == 8 && sl_step_track(stp(0, 3, 7) + 2) == 8 && sl_step_track(stp(0, 7, 63) + 42 + 0x128) == 8,
          "keine Step-Adresse → 8");
    CHECK(sl_step_track(0) == 8 && sl_step_track((uintptr_t)patmem[0] + 0x10 + 8 * 0xBA8) == 8, "ausserhalb → 8");
    S->mode[2] = 4;   /* Slicer 16 */
    CHECK(!strcmp(fmt(10, 3 * 64 + 32, 0, stp(0, 2, 9)), "Slice 4/16"), "Slice 4/16 (%s)", fmt(10, 3 * 64 + 32, 0, stp(0, 2, 9)));
    CHECK(!strcmp(fmt(10, 3 * 64 + 5, 0, stp(0, 2, 9)), "Slice 4 +7%"), "045 Feinversatz (%s)", fmt(10, 3 * 64 + 5, 0, stp(0, 2, 9)));
    CHECK(!strcmp(fmt(10, 0x7FFF, 0, stp(1, 2, 0)), "Slice 16/16"), "Wert 0x7FFF → Raste 511 → Slice 16");
    CHECK(!strcmp(fmt(11, 0x7FFF, 0, stp(0, 2, 9)), "Gate 100%"), "Gate 100%%");
    CHECK(!strcmp(fmt(11, 0, 0, stp(0, 2, 9)), "Gate 0%"), "Gate 0%%");
    orig_calls = 0;
    CHECK(!strcmp(fmt(10, 0x4000, 1, stp(0, 2, 9)), "+50.0 %") && orig_calls == 1 && orig_self == fmt_obj10, "relativ → Original mit Original-Objekt");
    CHECK(!strcmp(fmt(11, 0x4000, 0, stp(0, 3, 9)), "50.0 %") && orig_self == fmt_obj11, "Normal-Spur → Original (Ende)");
    CHECK(!strcmp(fmt(10, 0x4000, 0, 0), "50.0 %"), "ohne Step → Original");
    S->mode[2] = 8;
    CHECK(!strcmp(fmt(10, 47 * 64, 0, stp(0, 2, 9)), "Slice 48/64"), "Slicer 64");
    S->mode[2] = 10;
    CHECK(!strcmp(fmt(11, 0x7FFF, 0, stp(0, 2, 9)), "Grain 1000 ms"), "Grain 1000 ms");
    CHECK(!strcmp(fmt(10, 0x4000, 0, stp(0, 2, 9)), "50.0 %"), "Granular Start → Original (Position)");
    /* kurzer Puffer */
    char small[6]; memset(small, 'x', sizeof small);
    S->mode[2] = 4;
    uint32_t *obj = (uint32_t *)(uintptr_t)((uint32_t *)(host_param_tab + 0x20 * 10))[4];
    ((fmt_fn)(uintptr_t)((uint32_t *)(uintptr_t)obj[0])[0])(obj, 0, 0, small, sizeof small, (void *)stp(0, 2, 1));
    CHECK(memchr(small, 0, sizeof small) != NULL, "kurzer Puffer bleibt abgeschlossen (%.6s)", small);
    printf("  %d Pruefungen, %d Fehler\n", checks - c0, fails - f0);
}

/* ---- 4. Menue + Datei ---- */
static void test_menu_cfg(void) {
    printf("== 4. Menue und /engines.cfg\n");
    int c0 = checks, f0 = fails;
    sl_reset(); nmenu = nenum = 0;
    int parent;
    sl_menu_add(&parent);
    CHECK(nmenu == 1 && !strcmp(menus[0].name, "Engines") && menus[0].parent == &parent, "Seite „Engines“");
    sl_build(0, &parent);
    CHECK(nenum == 8, "8 Zeilen");
    for (int t = 0; t < 8 && t < nenum; t++) {
        char nm[24]; snprintf(nm, sizeof nm, "Track %d", t + 1);
        CHECK(!strcmp(enums[t].name, nm) && enums[t].max == 2 && enums[t].val == &host_menu_kind[t], "Zeile %d", t + 1);
        CHECK(enums[t].fn.manager == 0x31A35 && enums[t].fn.store[0] == (uint32_t)(uintptr_t)S, "std::function");
    }
    CHECK(!strcmp(enums[0].tab[0], "Normal") && !strcmp(enums[0].tab[1], "Slicer") && !strcmp(enums[0].tab[2], "Granular"), "Namen");
    /* 045: Art waehlen → Modus; gleiche Art behaelt die Slice-Zahl; Menue spiegelt den Modus */
    S->mode[0] = 0; S->mode[1] = 6; S->mode[2] = 10; sl_build(0, &parent);
    CHECK(host_menu_kind[0] == 0 && host_menu_kind[1] == 1 && host_menu_kind[2] == 2, "Menue zeigt die Art des Modus");
    host_menu_kind[0] = 1; host_menu_kind[1] = 1; host_menu_kind[2] = 0; sl_kind_apply();
    CHECK(S->mode[0] == 4 && S->mode[1] == 6 && S->mode[2] == 0, "Slicer neu = 16 Slices, vorhandener Slicer behaelt 32, Granular → Normal");
    host_menu_kind[3] = 2; sl_kind_apply(); CHECK(S->mode[3] == SL_GRAN, "Granular gewaehlt");
    for (int t = 0; t < 8; t++) { S->mode[t] = 0; host_menu_kind[t] = 0; }
    /* 024: Engines pro Projekt */
    fs_clear(); host_millis = 5000; writes = 0; mallocs = 0;
    sl_reset();
    run_ms(100); CHECK(!S->loaded, "ohne Lade-Hook noch nicht geladen");
    sl_load_dir("workspace", 1);
    CHECK(S->loaded && S->mode[0] == 0 && S->mode[7] == 0, "Start ohne Dateien → alle Normal");
    run_ms(20); CHECK(fs_get("workspace/engines.cfg") && strstr(fs_get("workspace/engines.cfg"), "t8=0\n"), "workspace/engines.cfg angelegt");
    /* alter Stand aus 021 */
    fs_clear(); sl_reset(); fs_put("/engines.cfg", "t2=5\nt3=9\n");
    sl_load_dir("workspace", 1);
    CHECK(S->mode[1] == 5 && S->mode[2] == 9 && (S->load_res & 0x20000u), "Rueckfall /engines.cfg beim Start");
    run_ms(20); CHECK(fs_get("workspace/engines.cfg") && strstr(fs_get("workspace/engines.cfg"), "t2=5\n"), "→ in workspace uebernommen");
    sl_load_dir("workspace", 1);                       /* zweiter Start: workspace gewinnt, kein Schreiben */
    int w = writes; run_ms(3000);
    CHECK(S->mode[1] == 5 && writes == w, "Start mit workspace-Datei: gelesen, nichts zu schreiben");
    /* Aenderung → nach 1,5 s workspace */
    S->mode[0] = 4; S->mode[7] = 11;
    run_ms(1000); CHECK(!strstr(fs_get("workspace/engines.cfg"), "t1=4\n"), "Verzoegerung");
    run_ms(800); CHECK(strstr(fs_get("workspace/engines.cfg"), "t1=4\n") && strstr(fs_get("workspace/engines.cfg"), "t8=11\n"), "workspace gespeichert");
    /* Speichern → Projektordner */
    sl_save_project("Projects", "Breaks");
    const char *pj = fs_get("Projects/Breaks/engines.cfg");
    CHECK(pj && strstr(pj, "t1=4\n") && strstr(pj, "t2=5\n") && strstr(pj, "t8=11\n"), "Projects/Breaks/engines.cfg geschrieben");
    sl_save_project("Projects/", "X");
    CHECK(fs_get("Projects/X/engines.cfg") != 0, "Schraegstrich am Ende nicht doppelt");
    /* Oeffnen eines anderen Projekts */
    fs_put("Projects/Pads/engines.cfg", "# x\nt5=10\n");
    sl_load_dir("Projects/Pads", 0);
    CHECK(S->mode[4] == 10 && S->mode[0] == 0 && S->mode[1] == 0, "Projekt Pads: nur t5 = Granular Rev, Rest Normal");
    run_ms(20); CHECK(strstr(fs_get("workspace/engines.cfg"), "t5=10\n") && strstr(fs_get("workspace/engines.cfg"), "t1=0\n"), "workspace = Projekt Pads");
    sl_load_dir("Projects/Breaks", 0);
    CHECK(S->mode[0] == 4 && S->mode[7] == 11 && S->mode[4] == 0, "zurueck zu Breaks");
    sl_load_dir("Projects/Ohne", 0);
    CHECK(S->mode[0] == 0 && S->mode[7] == 0, "Projekt ohne Datei → alle Normal");
    /* Neues Projekt */
    S->mode[3] = 9; run_ms(2000);
    sl_new_project();
    CHECK(S->mode[3] == 0, "Neues Projekt → Normal");
    run_ms(20); CHECK(strstr(fs_get("workspace/engines.cfg"), "t4=0\n"), "workspace zurueckgesetzt");
    /* Lesen: gueltige/ungueltige Zeilen */
    fs_put("Projects/P/engines.cfg", "# Kommentar\nt3=9\r\nt4=99\nt5=x\nt9=1\nt6=7\nt2=10\nfoo=1\nt1=\nt7=12\n");
    sl_load_dir("Projects/P", 0);
    CHECK(S->mode[2] == 9 && S->mode[5] == 7 && S->mode[1] == 10, "gueltige Zeilen gelesen");
    CHECK(S->mode[3] == 0 && S->mode[4] == 0 && S->mode[0] == 0 && S->mode[6] == 0, "ungueltige ignoriert");
    run_ms(20); S->mode[6] = 55; run_ms(20); CHECK(S->mode[6] == 0, "Wert ausserhalb → Normal");
    /* Pfad zu lang */
    char longname[300]; memset(longname, 'a', 299); longname[299] = 0;
    uint32_t se = S->save_err; sl_save_project("Projects", longname);
    CHECK(S->save_err == se + 1, "zu langer Pfad: nicht geschrieben, gezaehlt");
    /* Stop → Granular-Spuren blenden aus */
    for (int t = 0; t < 8; t++) S->mode[t] = 0;
    S->mode[2] = 9; S->mode[6] = 11; S->mode[1] = 4;
    sl_transport(1);
    CHECK(!gr_state(2)->rel && !gr_state(6)->rel, "Start: nichts");
    sl_transport(0);
    CHECK(gr_state(2)->rel && gr_state(6)->rel && !gr_state(1)->rel && !gr_state(0)->rel, "Stop: nur Granular-Spuren 3 und 7 ausblenden");
    sl_load_dir("Projects/P", 0);
    CHECK(!gr_state(2)->rel && !gr_state(2)->act[0], "Projektwechsel: Grains sofort aus");
    /* 029: Spurwerte (View) */
    sv_reset();
    for (int t = 0; t < 8; t++) S->mode[t] = 0;
    S->mode[1] = 4;                                     /* Spur 2 = Slicer 16 */
    CHECK(sv_off(1) == 0 && sv_sample(1) == -1, "leer");
    for (int i = 0; i < 100; i++) sv_apply(1, 21, 1);
    CHECK(sv_off(1) == 63, "Versatz klemmt +63");
    int32_t a0, b0;
    sl_bounds(1, 15, 16, 0x7FFF, &a0, &b0);
    CHECK(a0 < b0 && b0 == 0x7FFF && a0 > 15 * 0x7FFF / 16, "letzter Slice nach rechts: gekuerzt, nicht leer");
    sl_bounds(1, 3, 16, 0x7FFF, &a0, &b0);
    CHECK(a0 == 3 * 0x7FFF / 16 + 63 * (0x7FFF / 16) / 64, "Grenze verschoben (%d)", a0);
    for (int i = 0; i < 200; i++) sv_apply(1, 21, -1);
    CHECK(sv_off(1) == -63, "Versatz klemmt -63");
    sl_bounds(1, 0, 16, 0x7FFF, &a0, &b0);
    CHECK(a0 == 0 && b0 > 0, "erster Slice nach links: gekuerzt, nicht leer");
    for (int m = 1; m <= 8; m++) for (int q = 0; q < 64; q++) for (int o = -63; o <= 63; o += 21) {
        uint32_t n = sl_slices(m); if ((uint32_t)q >= n) continue;
        host_sv_ram[0x44 + 2] = (uint8_t)(int8_t)o;
        int32_t x0, x1; sl_bounds(2, (uint32_t)q, n, 0x7FFF, &x0, &x1);
        CHECK(x0 >= 0 && x1 <= 0x7FFF && x1 > x0, "Grenzen gueltig m=%d k=%d o=%d", m, q, o);
    }
    host_sv_ram[0x44 + 2] = 0;
    CHECK(sv_step_mode(4, 1) == 5 && sv_step_mode(4, -1) == 3 && sv_step_mode(8, 1) == 8 && sv_step_mode(1, -1) == 1 && sv_step_mode(0, 1) == 0 && sv_step_mode(9, -1) == 9, "Slice-Zahl klemmt, Normal/Granular unberuehrt");
    {
        uint8_t c[255]; memset(c, 0, sizeof c);
        CHECK(sv_next_sample(c, -1, 1) == -1, "keine Samples");
        c[2] = 1; c[5] = 2; c[7] = 1; c[9] = 2;         /* Reihenfolge 9, 5, 2, 7 */
        CHECK(sv_next_sample(c, -1, 1) == 9, "erste Drehung: neueste Aufnahme");
        CHECK(sv_next_sample(c, 9, 1) == 5 && sv_next_sample(c, 5, 1) == 2 && sv_next_sample(c, 2, 1) == 7 && sv_next_sample(c, 7, 1) == 9, "Reihenfolge rund");
        CHECK(sv_next_sample(c, 9, -1) == 7 && sv_next_sample(c, 2, -1) == 5, "rueckwaerts");
    }
    {   /* 030: Fokuszeile */
        char j[18];
        sv_join(j, sizeof j, "Filter Resonance", "100%"); CHECK(!strcmp(j, "Resonance 100%"), "letztes Wort: %s", j);
        CHECK(strstr(j, "100%") != 0, "Wert bleibt ganz: %s", j);
        sv_join(j, sizeof j, "Sample Attack (In)", "25.0%"); CHECK(!strcmp(j, "Attack 25.0%"), "Klammer weg: %s", j);
        sv_join(j, sizeof j, "Volume (dB)", "3"); CHECK(!strcmp(j, "Volume 3"), "kurz: %s", j);
        sv_join(j, sizeof j, "", "Beat"); CHECK(!strcmp(j, "Beat"), "ohne Name: %s", j);
        sv_join(j, sizeof j, "Cutoff", ""); CHECK(!strcmp(j, "Cutoff"), "ohne Wert: %s", j);
        sv_join(j, sizeof j, "Name", "ein sehr langer Wert hier"); CHECK(strlen(j) <= 17, "langer Wert begrenzt: %s", j);
        sv_join(j, sizeof j, "Filter Cutoff", "High-Pass 3"); CHECK(!strcmp(j, "High-Pass 3"), "kein abgeschnittener Name: %s", j);
        CHECK(sv_own_param(10) && sv_own_param(21) && sv_own_param(9) && !sv_own_param(2) && !sv_own_param(13) && !sv_own_param(6), "eigene Parameter");
    }
    {   /* 031: Note, Grid-Breite, Beat */
        sv_clear_track(1); S->mode[1] = 4;
        int32_t a1, b1, a2, b2;
        sv_cfg_set(1, 6, 1024);                         /* doppelte Breite */
        sl_bounds(1, 3, 16, 0x7FFF, &a1, &b1);
        CHECK(abs(a1 - 2 * 3 * 0x7FFF / 16) <= 2 && abs(b1 - 2 * 4 * 0x7FFF / 16) <= 2, "Breite ×2: %d %d", a1, b1);
        CHECK(sl_slice_outside(1, 8, 16) && !sl_slice_outside(1, 7, 16), "Slices hinter dem Ende");
        sv_cfg_set(1, 6, -512);
        sl_bounds(1, 15, 16, 0x7FFF, &a2, &b2);
        CHECK(b2 <= 0x7FFF / 2 + 2, "Breite ×0,5: Rest des Samples frei: %d", b2);
        for (int w = -512; w <= 1024; w += 73) for (int o = -63; o <= 63; o += 9) for (int m = 1; m <= 8; m++) for (uint32_t k = 0; k < sl_slices(m); k++) {
            sv_cfg_set(1, 6, w); sv_cfg_set(1, 3, o);
            int32_t x0, x1; sl_bounds(1, k, sl_slices(m), 0x7FFF, &x0, &x1);
            if (!(x0 >= 0 && x1 <= 0x7FFF && x1 > x0)) CHECK(0, "Grenzen w=%d o=%d m=%d k=%u: %d %d", w, o, m, k, x0, x1);
        }
        sv_clear_track(1);
        /* Beat-Erkennung auf Testsignal: Anschlaege bei bekannten Stellen, verschieden laut */
        enum { FR = 200000 };
        static int16_t sig[FR];
        const int hits[8] = { 0, 21000, 44100, 61000, 88200, 111000, 132300, 170000 };
        const int amp[8] = { 30000, 12000, 28000, 9000, 26000, 15000, 24000, 7000 };
        memset(sig, 0, sizeof sig);
        for (int h = 0; h < 8; h++) for (int i = 0; i < 12000 && hits[h] + i < FR; i++) {
            double env = exp(-i / 2500.0);
            sig[hits[h] + i] = (int16_t)(amp[h] * env * ((i & 8) ? 1 : -1));
        }
        sv_x_t *x = sv_xget(1);
        CHECK(x != 0, "Heap-Block");
        sv_scan_start(x, 1, (uint32_t)(uintptr_t)sig, FR, 44100);
        int polls = 0; while (!sv_scan_run(x, 32)) polls++;
        CHECK(polls < 100 && x->b[1].done, "Analyse in Stuecken (%d Durchlaeufe)", polls);
        sv_beat_pick(&x->b[1], 16);
        CHECK(x->b[1].nt == 8 && x->b[1].tab[0] == 0 && x->b[1].tab[8] == 0x7FFF, "Stufe 8: alle Anschlaege, nt=%u", x->b[1].nt);
        for (int h = 1; h < 8 && x->b[1].nt == 8; h++) {
            int pf = (int)((double)x->b[1].tab[h] * FR / 0x7FFF);
            CHECK(abs(pf - hits[h]) <= 90, "Anschlag %d: gefunden %d, wahr %d", h, pf, hits[h]);
        }
        { uint32_t last = 0; for (uint32_t L = 1; L <= 16; L++) { sv_beat_pick(&x->b[1], L); CHECK(x->b[1].nt >= last, "Stufe %u: mehr Slices", L); last = x->b[1].nt; } }
        sv_beat_pick(&x->b[1], 1); CHECK(x->b[1].nt < 8 && x->b[1].nt >= 1, "Stufe 1 grob: nt=%u", x->b[1].nt);
        /* Grenzen im Beat-Modus */
        S->mode[1] = 8;
        sv_cfg_set(1, 7, 1); sv_cfg_set(1, 8, 16); sv_beat_pick(&x->b[1], sv_beat_level(1));
        sl_bounds(1, 1, 64, 0x7FFF, &a1, &b1);
        CHECK(a1 == x->b[1].tab[1] && b1 == x->b[1].tab[2], "Beat-Grenzen aus der Tabelle");
        CHECK(sl_slice_outside(1, x->b[1].nt, 64) && !sl_slice_outside(1, x->b[1].nt - 1u, 64), "Pads hinter dem letzten Slice aus");
        sl_bounds(1, 1, 64, 0x4000, &a1, &b1);
        CHECK(a1 == x->b[1].tab[1] && b1 > a1 && b1 < x->b[1].tab[2], "Gate wirkt im Beat-Modus");
        {   /* 037: Move wirkt im Beat-Modus nicht: Slice-Grenzen bleiben am Anschlag (Befund Rec_001, o2=19) */
            int32_t a9, b9; sv_cfg_set(1, 3, 19); sl_bounds(1, 1, 64, 0x7FFF, &a9, &b9);
            CHECK(a9 == x->b[1].tab[1] && b9 == x->b[1].tab[2], "Beat: Offset ignoriert (%d, %d)", a9, b9);
            sv_cfg_set(1, 3, 0);
        }
        x->b[1].nfor = 99;                              /* Tabelle passt nicht zur Genauigkeit: Grid bis neu gewaehlt */
        sl_bounds(1, 2, 64, 0x7FFF, &a1, &b1);
        CHECK(a1 == 2 * 0x7FFF / 64, "alte Tabelle wird nicht benutzt: %d", a1);
        /* Umschalten */
        S->mode[1] = 3; sv_cfg_set(1, 7, 0);
        sv_beat_toggle(1, 1, &S->mode[1]);
        CHECK(sv_beat_on(1) && S->mode[1] == 8 && !x->b[1].done && !x->b[1].data, "Beat an: 64 Plaetze, rechnet neu");
        sv_beat_toggle(1, 1, &S->mode[1]); CHECK(S->mode[1] == 8, "zweimal an: Slice-Zahl bleibt gemerkt");
        sv_beat_toggle(1, 0, &S->mode[1]); CHECK(!sv_beat_on(1) && S->mode[1] == 3, "Beat aus: alte Slice-Zahl");
        sv_beat_toggle(1, 0, &S->mode[1]); CHECK(S->mode[1] == 3, "zweimal aus: unveraendert");
        sv_cfg_set(1, 8, 0); CHECK(sv_beat_level(1) == 8, "Standard-Sensitivity");
        for (int i = 0; i < 20; i++) sv_fine_step(1, 1);
        CHECK(sv_beat_level(1) == 16, "klemmt oben");
        for (int i = 0; i < 20; i++) sv_fine_step(1, -1);
        CHECK(sv_beat_level(1) == 1, "klemmt unten");
        /* Stille und Rauschen duerfen nicht haengen */
        memset(sig, 0, sizeof sig);
        sv_scan_start(x, 2, (uint32_t)(uintptr_t)sig, FR, 44100); while (!sv_scan_run(x, 32)) {}
        sv_beat_pick(&x->b[2], 16);
        CHECK(x->b[2].npk == 0 && x->b[2].nt == 1 && x->b[2].tab[0] == 0 && x->b[2].tab[1] == 0x7FFF, "Stille: 1 Slice");
        for (int i = 0; i < FR; i++) sig[i] = (int16_t)(rand() % 60000 - 30000);
        sv_scan_start(x, 2, (uint32_t)(uintptr_t)sig, FR, 44100); while (!sv_scan_run(x, 32)) {}
        for (uint32_t n = 1; n <= 16; n++) {
            sv_beat_pick(&x->b[2], n);
            CHECK(x->b[2].nt >= 1 && x->b[2].nt <= 64, "Rauschen n=%u nt=%u", n, x->b[2].nt);
            for (uint32_t i = 0; i < x->b[2].nt; i++) CHECK(x->b[2].tab[i] < x->b[2].tab[i + 1], "Tabelle steigt n=%u i=%u", n, i);
        }
        static int16_t mini[300]; memset(mini, 0, sizeof mini);
        sv_scan_start(x, 3, (uint32_t)(uintptr_t)mini, 300, 44100); while (!sv_scan_run(x, 32)) {}
        CHECK(x->b[3].done, "sehr kurzes Sample");
        sv_clear_track(1); sv_clear_track(2); sv_clear_track(3);
        S->mode[1] = 4;
        {   /* 039: Granular-Einstellungen: nur Abweichungen vom Standard, nach Neuladen wieder da */
            gr_cfg(3)->n = 6; gr_cfg(3)->win = 80; gr_cfg(3)->spray = 40; gr_cfg(3)->pspray = 7; gr_cfg(3)->scan = -30; gr_cfg(3)->lrate = 20; gr_cfg(3)->ldepth = 55; gr_cfg(3)->ltgt = 2;
            gr_cfg(3)->pmode = 3; gr_cfg(3)->speed = 40; gr_cfg(3)->rate = 50;
            sl_save_project("Projects", "Gr");
            const char *gc = fs_get("Projects/Gr/engines.cfg");
            CHECK(gc && strstr(gc, "k4=6\n") && strstr(gc, "i4=81\n") && strstr(gc, "c4=40\n") && strstr(gc, "y4=7\n") && strstr(gc, "x4=-30\n") && strstr(gc, "r4=21\n") && strstr(gc, "l4=55\n") && strstr(gc, "z4=2\n") && strstr(gc, "u4=3\n") && strstr(gc, "s4=40\n") && strstr(gc, "v4=50\n") && !strstr(gc, "k5="), "Granular gespeichert");
            sl_new_project(); CHECK(gr_cfg(3)->n == 4 && gr_cfg(3)->win == 50 && !gr_cfg(3)->spray && !gr_cfg(3)->scan, "neues Projekt: Standard");
            sl_load_dir("Projects/Gr", 0);
            CHECK(gr_cfg(3)->n == 6 && gr_cfg(3)->win == 80 && gr_cfg(3)->spray == 40 && gr_cfg(3)->pspray == 7 && gr_cfg(3)->scan == -30 && gr_cfg(3)->lrate == 20 && gr_cfg(3)->ldepth == 55 && gr_cfg(3)->ltgt == 2 && gr_cfg(3)->pmode == 3 && gr_cfg(3)->speed == 40 && gr_cfg(3)->rate == 50, "Granular wieder geladen");
            sv_clear_track(3);
            for (uint32_t q_ = 0; q_ < 256u; q_++) { uint32_t ph = q_ << 24; int v_ = gr_lfo_tri(ph); CHECK(v_ >= -32768 && v_ <= 32767, "LFO im Bereich %d", v_); }
            CHECK(gr_lfo_tri(0) == -32768 && gr_lfo_tri(0x80000000u) >= 32700, "LFO Dreieck");
            CHECK(gr_attack_blocks(0) == 0 && gr_attack_blocks(10000) == 1379 && gr_attack_blocks(1) >= 1, "Huelle: bis 4 s (%u)", gr_attack_blocks(10000));
            gr_par_t pc; gr_set_count(&pc, 0); CHECK(pc.n == 1, "Grains klemmt unten"); gr_set_count(&pc, 99); CHECK(pc.n == 8, "Grains klemmt oben");
        }
        /* Speichern */
        sv_cfg_set(4, 6, 33); sv_cfg_set(4, 7, 1); sv_cfg_set(4, 8, 6); sv_cfg_set(4, 9, 3);
        sl_save_project("Projects", "Beat");
        const char *bc = fs_get("Projects/Beat/engines.cfg");
        CHECK(bc && !strstr(bc, "n5=") && strstr(bc, "w5=33\n") && strstr(bc, "m5=1\n") && strstr(bc, "e5=6\n") && strstr(bc, "h5=3\n"), "Note/Breite/Modus/Genauigkeit gespeichert");
        sl_new_project();
        CHECK(!sv_wid(4) && !sv_beat_on(4), "neues Projekt: geloescht");
        sl_load_dir("Projects/Beat", 0);
        CHECK(sv_wid(4) == 33 && sv_beat_on(4) && sv_beat_level(4) == 6 && sv_cfg_get(4, 9) == 3, "wieder geladen");
        sv_clear_track(4);
        /* 045: Grid breiter als Fit → hintere Slices fallen weg; neues Sample → Fit */
        S->mode[4] = 1; sv_cfg_set(4, 6, 400); sv_cfg_set(4, 3, 10);
        CHECK(sv_fit_count(4, 4) == 3 && sv_fit_count(4, 16) < 16, "Grid +39 %%: 3 von 4 Slices im Sample");
        sv_grid_fit(4);
        CHECK(sv_wid(4) == 0 && sv_off(4) == 0 && sv_fit_count(4, 4) == 4, "sv_grid_fit: Breite und Move auf Fit, alle 4 Slices");
        S->mode[4] = 0; sv_clear_track(4);
        /* alte Datei: Zeilen aus 029–032 werden ignoriert, die Vorbereitung bleibt */
        fs_put("Projects/Alt/engines.cfg", "a3=700\nd3=900\ng3=500\nn3=-4\nv3.9=0\nv3.:=0\no3=19\n");
        sl_load_dir("Projects/Alt", 0);
        CHECK(sv_off(2) == 19 && !sv_cfg_get(2, 0) && !sv_cfg_get(2, 1) && !sv_cfg_get(2, 5), "alte Spurwerte ignoriert, Versatz bleibt");
        sv_clear_track(2); S->mode[1] = 4;
    }
    uint32_t ak; ((volatile sv_t *)host_sv_ram)->aud[1] = 3; CHECK(sv_take_audition(1, &ak) && ak == 2 && !sv_take_audition(1, &ak), "Vorhoeren einmalig");
    /* Speichern/Laden ueber engines.cfg */
    sv_clear_track(1);
    sv_cfg_set(1, 3, -12); sv_cfg_set(6, 4, 8);
    sl_save_project("Projects", "Sl");
    const char *sc = fs_get("Projects/Sl/engines.cfg");
    CHECK(sc && strstr(sc, "o2=-12\n") && strstr(sc, "p7=8\n") && !strstr(sc, "g2=") && !strstr(sc, "v2."), "Vorbereitung gespeichert");
    sl_new_project();
    CHECK(!sv_off(1) && !sv_cfg_get(6, 4), "Neues Projekt: geloescht");
    sl_load_dir("Projects/Sl", 0);
    CHECK(sv_off(1) == -12 && sv_sample(6) == 7 && S->mode[1] == 4, "Spurwerte wieder geladen");
    fs_put("Projects/Bad/engines.cfg", "s2.5=1,1,1,1\ng9=1\ng2=99999\no2=99\na2=x\np2=300\ng3=100\n");
    sl_load_dir("Projects/Bad", 0);
    CHECK(!sv_off(1) && !sv_cfg_get(1, 4), "ungueltige und alte Zeilen ignoriert");
    *sv_dirty_flag() = 1; int wsd = writes; run_ms(2000);
    CHECK(writes > wsd && *sv_dirty_flag() == 0, "geaenderte Spurwerte → workspace gespeichert");
    for (int t = 0; t < 8; t++) S->mode[t] = 0;

    /* Schreibfehler: hoechstens 5 Versuche */
    sd_fail_write = 1; S->mode[1] = 3; run_ms(20000);
    CHECK(S->save_fail >= 5 && S->save_err >= S->save_fail, "Schreibfehler gezaehlt, Aufgabe nach 5 (%u)", S->save_fail);
    sd_fail_write = 0;
    CHECK(mallocs == 0, "Speicher freigegeben (%d)", mallocs);
    printf("  %d Pruefungen, %d Fehler\n", checks - c0, fails - f0);
}

/* ---- 5. Granular-DSP ---- */
static float fbits_to_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static void test_granular(void) {
    printf("== 5. Granular-DSP\n");
    int c0 = checks, f0 = fails;
    CHECK(gr_grain_ms(0) == 5 && gr_grain_ms(0x7FFF) == 1000 && gr_grain_ms(-3) == 5 && gr_grain_ms(99999) == 1000, "Grain-Laenge 5…1000 ms");
    for (int32_t v = 1; v <= 0x7FFF; v++) CHECK(gr_grain_ms(v) >= gr_grain_ms(v - 1), "monoton");
    srand(21);
    for (int i = 0; i < 20000; i++) {
        float f = (float)(rand() % 100000) / 1000.0f * (rand() % 2 ? 1.0f : 0.001f);
        uint32_t b; memcpy(&b, &f, 4);
        double want = floor((double)f * 65536.0); if (want > 64 * 65536.0) want = 64 * 65536.0;
        CHECK(fabs((double)gr_f2q16(b) - want) <= 1.0, "f2q16(%g) = %u, erwartet %.0f", f, gr_f2q16(b), want);
    }
    CHECK(gr_f2q16(0) == 0 && gr_f2q16(0x80000000u | 0x3F800000u) == 0 && gr_f2q16(0x7F800000u) == 64u << 16, "Sonderfaelle f2q16");
    (void)fbits_to_f;
    /* DC-Quelle: Summe der Fenster = 1 → nach dem ersten Einblenden konstant */
    static const uint32_t LENS[3] = { 4096, 44100, 300000 };
    for (int li = 0; li < 3; li++) {
        uint32_t L = LENS[li];
        int16_t *data = malloc(L * 2);               /* exakt gross: ASan meldet jedes Lesen dahinter */
        for (uint32_t i = 0; i < L; i++) data[i] = 10000;
        for (int dm = 0; dm < 3; dm++) for (int32_t vl = 0; vl <= 0x7FFF; vl += 4096) for (int32_t sh = 0; sh <= 10000; sh += 2500) {
            gr_t g; memset(&g, 0, sizeof g); gr_reset(&g, 1);
            gr_par_t p; p.data = data; p.len = L; p.inc = 1u << 16;
            gr_params(&p, 0x4000, vl, sh, 0, dm);
            CHECK(p.glen >= 220 && p.glen <= 44100 && p.fade >= 32 && p.fade <= p.glen / 2, "Parameter glen=%u fade=%u", p.glen, p.fade);
            int16_t out[128]; int bad = 0, first = 1; uint32_t total = 0;
            uint32_t blocks = (p.glen * 3) / 128 + 4;
            uint32_t span = p.glen;                    /* inc 1.0 */
            int inside = (p.center + span < L) && (dm != 1 || p.center + span < L);
            for (uint32_t b = 0; b < blocks; b++) {
                gr_render(&g, &p, out, 128);
                for (int k = 0; k < 128; k++, total++) {
                    if (total < p.fade) continue;      /* erstes Einblenden */
                    if (inside && abs(out[k] - 10000) > 2) { if (first) printf("    L=%u dm=%d vl=%d sh=%d t=%u out=%d\n", L, dm, vl, sh, total, out[k]); first = 0; bad++; }
                }
            }
            CHECK(bad == 0 || !inside, "DC konstant L=%u Modus %d glen=%u fade=%u (%d Abweichungen)", L, dm, p.glen, p.fade, bad);
        }
        /* Grenzen: Position am Rand, Spray maximal, Tonhoehe maximal, alle Richtungen (ASan prueft Lesezugriffe) */
        for (int dm = 0; dm < 3; dm++) for (int32_t pos = 0; pos <= 0x7FFF; pos += 0x7FFF / 4) for (uint32_t inc = 1; inc <= (64u << 16); inc = inc * 7 + 1) {
            gr_t g; memset(&g, 0, sizeof g); gr_reset(&g, (uint32_t)(pos + dm + 1));
            gr_par_t p; p.data = data; p.len = L; p.inc = inc;
            gr_params(&p, pos, 0x7FFF, 10000, 10000, dm);
            int16_t out[128];
            for (int b = 0; b < 40; b++) gr_render(&g, &p, out, 128);
            CHECK(g.act[0] <= 1 && g.act[1] <= 1, "Zustand");
        }
        free(data);
    }
    /* Richtungen: Rampe als Quelle, Steigung des Ausgangs im vollen Teil des Grains */
    {
        uint32_t L = 100000; int16_t *data = malloc(L * 2);
        for (uint32_t i = 0; i < L; i++) data[i] = (int16_t)((int32_t)(i % 20000) - 10000);
        for (int dm = 0; dm < 3; dm++) {
            gr_t g; memset(&g, 0, sizeof g); gr_reset(&g, 5);
            gr_par_t p; p.data = data; p.len = L; p.inc = 1u << 16;
            gr_params(&p, (int32_t)(0x7FFF * 0.3), 10000, 0, 0, dm);      /* glen ~ 95 ms, harte Kante (32 Samples) */
            int16_t out[128]; int up = 0, down = 0;
            for (int b = 0; b < 200; b++) {
                gr_render(&g, &p, out, 128);
                for (int k = 1; k < 128; k++) { int d = out[k] - out[k - 1]; if (d == 1) up++; else if (d == -1) down++; }
            }
            if (dm == 0) CHECK(up > 10 * down, "vorwaerts steigt (%d/%d)", up, down);
            if (dm == 1) CHECK(down > 10 * up, "rueckwaerts faellt (%d/%d)", up, down);
            if (dm == 2) CHECK(up > 1000 && down > 1000 && abs(up - down) < (up + down) / 4, "Ping-Pong beides (%d/%d)", up, down);
        }
        /* Spray: Grain-Startpositionen streuen innerhalb +/- spray */
        gr_t g; memset(&g, 0, sizeof g); gr_reset(&g, 7);
        gr_par_t p; p.data = data; p.len = L; p.inc = 1u << 16;
        gr_params(&p, 0x4000, 2000, 5000, 10000, 0);
        uint32_t mn = 0xFFFFFFFF, mx = 0; int spawns = 0; uint8_t a0 = 0, a1 = 0;
        int16_t out[128];
        for (int b = 0; b < 400; b++) {
            gr_render(&g, &p, out, 128);
            for (int j = 0; j < 2; j++) if (g.act[j] && g.age[j] < 128) { uint32_t s0 = (uint32_t)g.ipos[j] - g.age[j]; if (s0 < mn) mn = s0; if (s0 > mx) mx = s0; spawns++; }
            (void)a0; (void)a1;
        }
        CHECK(p.spray == 22050 && mn + 2 >= p.center - p.spray && mx <= p.center + p.spray + 2 && mx - mn > p.spray, "Spray %u…%u um %u (+/-%u, %d Grains)", mn, mx, p.center, p.spray, spawns);
        free(data);
    }
    printf("  %d Pruefungen, %d Fehler\n", checks - c0, fails - f0);
}

int main(void) {
    test_bounds();
    test_hook();
    test_format();
    test_menu_cfg();
    test_granular();
    printf("ERGEBNIS: %d Pruefungen, %d Fehler%s\n", checks, fails, fails ? "" : " – OK");
    return fails != 0;
}
