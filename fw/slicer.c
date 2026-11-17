/* Patch 021 – Slicer (und Rahmen fuer Granular) fuer Audio-Spuren, Polyend Play Rev A, Firmware 1.6.0
 *
 * Menue "Engines" (neue Hauptmenue-Seite vor "USB Audio"): je Spur eine Zeile "Track 1" … "Track 8" mit
 *   Normal / Slicer 4 / 8 / 12 / 16 / 24 / 32 / 48 / 64 / Granular / Granular Rev / Granular P-P
 * gespeichert pro Projekt (024): workspace/engines.cfg und <Projekt>/engines.cfg (Textdatei t1=0 … t8=0, siehe README).
 *
 * Slicer: Das Sample der Spur wird in N gleich lange Slices geteilt.
 *   Sample Start (Audio-Index 10, Rohwert 0…0x7FFF, Knob-Raster 64): Slice k = (Wert >> 6) mod N, Anzeige "Slice k+1/N".
 *     Eine Raste = ein Slice; Weiterdrehen springt zyklisch wieder auf Slice 1. Zufall (Step-Action "Rnd Sample Start",
 *     Randomize-Typ "Sample Length") ergibt dadurch einen zufaelligen Slice.
 *   Sample End (Index 11): Gate = Anteil des Slices, der gespielt wird (0x7FFF = ganzer Slice), Anzeige "Gate xx%".
 * Wirkung: Hook im Audio-Step "applyParam" (FUN_00048910, vtable 0xAA338 +0x50) auf den Aufruf von
 *   MidiTrack_getParam (0x489A4): fuer Index 10/11 werden die Werte, die in die Spur-Slots 8 (Start) und 9 (Ende)
 *   gehen, auf die Slice-Grenzen umgerechnet. Die Firmware rechnet sie beim Ausloesen (FUN_000166F4) wie immer mit
 *   Laenge × Wert / 0x7FFF in Frames um.
 * Neuer Step: vtable 0xAA338 +0x4C (0xAA384, im Original "bx lr" 0x48104) → sl_begin: vergisst den gemerkten
 *   Startwert der Spur und merkt den Spur-Knoten (ctx[0]) fuer granular.c.
 * Anzeige: Formatter-Objekte der Audio-Parametertabelle 0x1FFF7918, Eintraege 10/11 (+0x10) → eigene Objekte; die
 *   Spur wird aus der Step-Adresse bestimmt (Pattern im Engine-Objekt 0x200025C4, Spur = Pattern+0x10+t*0xBA8,
 *   Steps ab Spur+0x128, je 42 Byte). Sonst Original-Formatter 0x2AD38.
 *
 * RAM: sl_t bei 0x1FFF9800 (SRAM_L, 2 KB bis 0x1FFF9FFF; Granular-Zustand ab 0x1FFF9900, siehe granular.c).
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt (sl_test.c).
 */
#include <stdint.h>
#include "slicer.h"
#include "slview.h"
#define GR_STATE(t) gr_state(t)
#include "wave.h"
#include "lrec.h"
#include "tap.h"

typedef struct { uint32_t store[2]; uint32_t manager; uint32_t invoker; } stdfn_t;   /* std::function (libstdc++) */
typedef void (*fmt_fn)(void *self, int32_t value, int32_t rel, char *buf, uint32_t size, void *step);

#ifdef HOST_TEST
extern uint8_t  host_sl_ram[];
extern uint8_t  host_param_tab[];             /* Audio-Parametertabelle, 22 × 0x20 */
extern uint32_t host_millis;
extern uintptr_t host_pat0, host_pat1;
extern uint32_t host_get_param(int *step, uint32_t idx, int *ctx, uint8_t *ev);
extern void    *host_node_slot(void *node, uint32_t i);
extern void     host_slot_set(void *slot, uint32_t src, int32_t v);
extern void     host_orig_fmt(void *self, int32_t value, int32_t rel, char *buf, uint32_t size, void *step);
extern void     host_arp_init(void);
extern void    *host_heap_alloc(uint32_t n);
extern void    *host_menu_add(void *it, void *parent, const char *name, uint32_t fn, uint32_t adj);
extern void    *host_enum_item(void *it, void *parent, const char *name, int32_t *val, uint32_t max, const char *const *tab, stdfn_t *fn);
extern void    *host_malloc(uint32_t n);
extern void     host_free(void *p);
extern int      host_f_open(void *fp, const char *path, uint8_t mode);
extern int      host_f_read(void *fp, void *buf, uint32_t n, uint32_t *br);
extern int      host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw);
extern int      host_f_close(void *fp);
#define SL          ((volatile sl_t *)(void *)host_sl_ram)
#define AUDIO_TAB   ((uintptr_t)host_param_tab)
#define MILLIS()    (host_millis)
#define GET_PARAM   host_get_param
#define NODE_SLOT(n,i) host_node_slot((n),(i))
#define SLOT_SET    host_slot_set
#define ORIG_FMT    host_orig_fmt
#define ARP_INIT()  host_arp_init()
#define PAT0        host_pat0
#define PAT1        host_pat1
#define RAM_OK(p)   ((p) != 0u)
#define HEAP_ALLOC(n) host_heap_alloc(n)
#define MENU_ADD    host_menu_add
#define ENUM_ITEM   host_enum_item
#define FN_MANAGER  0x31A35u
#define MALLOC(n)   host_malloc(n)
#define FREE(p)     host_free(p)
#define F_OPEN      host_f_open
#define F_READ      host_f_read
#define F_WRITE     host_f_write
#define F_CLOSE     host_f_close
#define FNADDR(f)   ((uint32_t)(uintptr_t)(f))
#define FMT_VT1     0x2A9D1u
#define FMT_VT2     0x2AAE9u
#define FMT_VT3     0x2A8B1u
#else
void arp_init(void);                                  /* arp.c (ruft ParamDefs_ctor) */
#define SL          ((volatile sl_t *)0x1FFF9800u)
#define AUDIO_TAB   0x1FFF7918u                       /* Audio-Parametertabelle, 22 × 0x20 Byte (ParamDefs_ctor) */
#define MILLIS()    (*(volatile uint32_t *)0x20000D7Cu)
#define GET_PARAM   ((uint32_t (*)(int *, uint32_t, int *, uint8_t *))0x00048811u)   /* MidiTrack_getParam (generisch) */
/* Spur-Knoten: vtable +0x18 = Slot i (FUN_000190FC: Knoten + 4 + 16 i); FUN_00014600(slot, quelle, wert) */
#define NODE_SLOT(n,i) (((void *(*)(void *, uint32_t))(((const uint32_t *)(*(const uint32_t *)(n)))[6]))((n),(i)))
#define SLOT_SET    ((void (*)(void *, uint32_t, int32_t))0x00014601u)
#define ORIG_FMT    ((fmt_fn)0x0002AD39u)             /* Sample-Start/End-Formatter "%0.1f %%" / "%.0f/%.0f ms" */
#define ARP_INIT()  arp_init()
#define PAT0        0x200025D0u                       /* Engine 0x200025C4 + 0xC  (FUN_000497F4) */
#define PAT1        0x2000E060u                       /* Engine 0x200025C4 + 0xBA9C */
#define RAM_OK(p)   ((p) >= 0x1FFF0000u && (p) < 0x2003FF00u && ((p) & 3u) == 0u)
#define HEAP_ALLOC(n) (((void *(*)(uint32_t))0x000030C1u)(n))
#define MENU_ADD    ((void *(*)(void *, void *, const char *, uint32_t, uint32_t))0x0003350Du)    /* MenuItem_ctor */
#define ENUM_ITEM   ((void *(*)(void *, void *, const char *, volatile int32_t *, uint32_t, const char *const *, stdfn_t *))0x00034F31u)
#define FN_MANAGER  0x00031A35u
#define MALLOC(n)   (((void *(*)(uint32_t))0x00056F5Du)(n))
#define FREE(p)     (((void (*)(void *))0x00056F6Du)(p))
#define F_OPEN      ((int (*)(void *, const char *, uint8_t))0x0000FB39u)
#define F_READ      ((int (*)(void *, void *, uint32_t, uint32_t *))0x0000FEB5u)
#define F_WRITE     ((int (*)(void *, const void *, uint32_t, uint32_t *))0x00010089u)
#define F_CLOSE     ((int (*)(void *))0x000104F5u)
#define FNADDR(f)   ((uint32_t)(uintptr_t)(f) | 1u)
#define FMT_VT1     0x0002A9D1u                       /* Rest der vtable 0xA0338 (benutzen this nicht) */
#define FMT_VT2     0x0002AAE9u
#define FMT_VT3     0x0002A8B1u
#endif

#define ENTRY(i)    ((volatile uint32_t *)(AUDIO_TAB + 0x20u * (i)))
#define P_START     10u
#define P_END       11u
#define SAVE_DELAY_MS 1500u
#define LOAD_FIRST_MS 1200u
#define LOAD_RETRY_MS 500u
#define LOAD_GIVEUP_MS 20000u
#define FA_READ      0x01u
#define FA_WRITE_NEW 0x0Au
#define FIL_SIZE     0x250u
#define FR_NO_FILE   4
#define FR_NO_PATH   5
#define PAT_TRACKS   (8u * 0xBA8u)

static const uint16_t SLICES[SL_NMODES] = { 0, 4, 8, 12, 16, 24, 32, 48, 64, 0, 0, 0 };
uint32_t sl_slices(int32_t m) { return (m >= 0 && m < SL_NMODES) ? SLICES[m] : 0u; }
#if defined(WITH_GRANULAR) || defined(HOST_TEST)
int sl_is_granular(int32_t m) { return m >= SL_GRAN && m < SL_NMENU; }
#endif

/* ------------------------------------------------------------------ Slice-Rechnung (Einheiten 0…0x7FFF) */
uint32_t sl_slice_index(int32_t v, uint32_t n) {
    if (v < 0) v = 0;
    return n ? (((uint32_t)v >> 6) % n) : 0u;
}
/* Start/Ende von Slice k in Sample-Start-Einheiten; gate 0…0x7FFF = Anteil des Slices */
void sl_slice_bounds(uint32_t k, uint32_t n, int32_t gate, int32_t *start, int32_t *end) {
    uint32_t s = k * 0x7FFFu / n, e = (k + 1u) * 0x7FFFu / n;
    if (gate < 0) gate = 0;
    if (gate > 0x7FFF) gate = 0x7FFF;
    uint32_t en = s + (e - s) * (uint32_t)gate / 0x7FFFu;
    if (gate > 0 && en <= s) en = s + 1u;
    *start = (int32_t)s; *end = (int32_t)en;
}

/* 029/031: Grenzen der Spur: Grid (Breite, Versatz) oder Beat-Tabelle. Kein Slice wird leer; hinter dem Sample-Ende 1 Wert am Rand. */
void sl_bounds(uint32_t t, uint32_t k, uint32_t n, int32_t gate, int32_t *start, int32_t *end) {
    int32_t off = sv_off(t), wid = sv_wid(t);
    int32_t a, b;
    if (n && sv_beat_bounds(t, k, n, &a, &b)) {
        if (a >= 0x7FFE) { *start = 0x7FFE; *end = 0x7FFF; return; }
    } else {
        if (!off && !wid && !sv_cut_any(t, n)) { sl_slice_bounds(k, n, gate, start, end); return; }   /* exakt wie frueher (auch Gate 0) */
        if (!n) { sl_slice_bounds(k, n, gate, start, end); return; }
        uint32_t sc = (uint32_t)(1024 + wid);
        a = (int32_t)(k * 0x7FFFu * sc / 1024u / n);
        b = (int32_t)((k + 1u) * 0x7FFFu * sc / 1024u / n);
    }
    if (gate < 0) gate = 0;
    if (gate > 0x7FFF) gate = 0x7FFF;
    /* 037: Move gilt nur fuers Grid; im Beat-Modus liegen die Grenzen am Anschlag (Offset schob Slices in den naechsten Schlag) */
    int32_t d = (n && !sv_beat_on(t)) ? off * (int32_t)(0x7FFFu * (uint32_t)(1024 + wid) / 1024u / n) / 64 : 0;
    a += d; b += d;
    if (n) { a += sv_cut(t, k, n); if (k + 1u < n) b += sv_cut(t, k + 1u, n); }   /* 12.11.: Handschnitte (Anfang k, Ende = Anfang k+1) */
    if (a < 0) a = 0;
    if (a >= 0x7FFF) { *start = 0x7FFE; *end = 0x7FFF; return; }
    if (b > 0x7FFF) b = 0x7FFF;
    if (b <= a) b = a + 1;
    int32_t en = a + (int32_t)((uint32_t)(b - a) * (uint32_t)gate / 0x7FFFu);
    if (en <= a) en = a + 1;
    *start = a; *end = en;
}
int sl_slice_outside(uint32_t t, uint32_t k, uint32_t n) {
    int32_t a, b;
    sl_bounds(t, k, n, 0x7FFF, &a, &b);
    return a >= 0x7FFE;
}

/* ------------------------------------------------------------------ Initialisierung (init_array, statt arp_init) */
static void fmt_install(void);
void sl_reset(void) {
    volatile sl_t *s = SL;
    volatile uint32_t *z = (volatile uint32_t *)(void *)s;
    for (uint32_t i = 0; i < sizeof(sl_t) / 4u; i++) z[i] = 0;
    gr_clear();
    s->t_load = LOAD_FIRST_MS;
    s->magic = SL_MAGIC;
}

__attribute__((used, section(".text.entry_slinit")))
void sl_init(void) {
    ARP_INIT();                         /* ParamDefs_ctor + Arp (Patch 014), unveraendert */
    sl_reset();
    wv_reset();                         /* Patch 023 */
    sv_reset();                         /* Patch 026: Slicer in View */
    lr_reset();
    tt_reset();                         /* 045: Tap Tempo */
    hd_reset();                         /* 045: Kopfzeile */
    fmt_install();
}

/* ------------------------------------------------------------------ Spur aus Kontext / Step */
static uint32_t ctx_track(int *ctx) {
    uintptr_t c = (uintptr_t)ctx;
    if (!RAM_OK(c)) return 8u;
    uint32_t t = (uint32_t)ctx[2];       /* Mixer-Kanal der Spur (FUN_00048910: mixer->vt+0x10(mixer, ctx[2], slot)) */
    return t < 8u ? t : 8u;
}

/* Audio-Spur eines Steps aus seiner Adresse (nur Steps in den zwei Patterns des Engine-Objekts) */
uint32_t sl_step_track(uintptr_t st) {
    const uintptr_t pats[2] = { PAT0, PAT1 };
    for (uint32_t p = 0; p < 2u; p++) {
        uintptr_t b = pats[p] + 0x10u;
        if (st < b || st >= b + PAT_TRACKS) continue;
        uint32_t off = (uint32_t)(st - b), t = off / 0xBA8u, r = off % 0xBA8u;
        if (r < 0x128u || (r - 0x128u) % 42u != 0u) return 8u;
        return t;
    }
    return 8u;
}

/* ------------------------------------------------------------------ Hooks im Wiedergabepfad */
static void slot_write(int *ctx, uint32_t slot, int32_t v);
/* 029: Werte der Spur vor jedem Step in die Slots; gesetzte Step-Parameter ueberschreiben sie danach */
#ifdef HOST_TEST
__attribute__((weak)) int32_t wv_touched_param(uint32_t idx) { (void)idx; return 0; }
__attribute__((weak)) void host_tp_write(int *ctx, uint32_t idx, int32_t v) { (void)ctx; (void)idx; (void)v; }
#define TP_WRITE(ctx, idx, v) host_tp_write((ctx), (idx), (v))
#else
/* Wie AudioStep_applyParam: Art 1 = Slot des Spurknotens, Art 2 = Mixer-Kanal; Ziel in der Tabelle 0xAA114 */
static void tp_write(int *ctx, uint32_t idx, int32_t v) {
    uint32_t kind = *(const volatile uint8_t *)(0xAA115u + idx * 16u);
    uint32_t tgt = *(const volatile uint32_t *)(0xAA118u + idx * 16u);
    void *slot = 0;
    if (kind == 1u) { void *node = (void *)(uintptr_t)(uint32_t)ctx[0]; if (node) slot = NODE_SLOT(node, tgt); }
    else if (kind == 2u) {
        uint32_t mx = (uint32_t)ctx[1];
        if (mx) slot = ((void *(*)(uint32_t, uint32_t, uint32_t))(((const uint32_t *)(uintptr_t)*(volatile uint32_t *)mx)[4]))(mx, (uint32_t)ctx[2], tgt);
    }
    if (slot) SLOT_SET(slot, 1u, v);
}
#define TP_WRITE(ctx, idx, v) tp_write((ctx), (idx), (v))
#endif
#ifdef HOST_TEST
int *sl_track_ctx(uint32_t t) { (void)t; return 0; }
#else
int *sl_track_ctx(uint32_t t) {
    if (t >= 8u) return 0;
    int *c = (int *)(uintptr_t)(PAT0 + 0x10u + t * 0xBA8u + 0x88u);
    uint32_t n = (uint32_t)c[0];
    if (n < 0x1FFF0000u || n >= 0x20030000u || (n & 3u) || *(const volatile uint32_t *)n != 0x0009C59Cu) return 0;
    return c;
}
#endif
/* Vorhoeren: Werte des Standard-Steps (Vorlage) in die Slots */
void sl_preview_params(uint32_t t) {
    static const uint8_t L[SV_PV_N] = { 0, 1, 2, 3, 4, 5, 7, 8, 12, 13, 14 };
    int *c = sl_track_ctx(t);
    if (c && sl_is_granular(SL->mode[t])) {               /* 039: Position und Size wirken live auf die klingende Wolke */
        int32_t a = wv_touched_param(11u); if (a != -32768) TP_WRITE(c, 11u, a);
    }
    if (!c) return;
    for (uint32_t i = 0; i < SV_PV_N; i++) {             /* nur Gesetztes: ein leerer Standard-Step darf das Pad nicht stumm schalten */
        int32_t x = wv_touched_param(L[i]);
        if (x != -32768) TP_WRITE(c, L[i], x);
    }
}
static void sl_begin_track(uint32_t t, int *ctx) {
    if (!sl_slices(SL->mode[t]) && !sl_is_granular(SL->mode[t])) return;
    {
        (void)ctx;
    }
    int32_t smp = sv_sample(t);
    if (smp >= 0) slot_write(ctx, 7u, smp);
}

/* vtable 0xAA338 +0x4C (Original: bx lr), Aufruf in MidiTrack_resolveEvent direkt vor der Parameterschleife */
void sl_begin(int *step, int *ctx, uint8_t *ev) {
    (void)step; (void)ev;
    volatile sl_t *s = SL;
    if (s->magic != SL_MAGIC) return;
    uint32_t t = ctx_track(ctx);
    if (t >= 8u) return;
    s->have10[t] = 0;
    s->node[t] = (uint32_t)ctx[0];
    s->n_begin++;
    sl_begin_track(t, ctx);
}

static void slot_write(int *ctx, uint32_t slot, int32_t v) {
    void *node = (void *)(uintptr_t)(uint32_t)ctx[0];
    if (!node) return;
    void *sl = NODE_SLOT(node, slot);
    if (sl) SLOT_SET(sl, 1u, v);
}

/* ersetzt bei 0x489A4 "bl MidiTrack_getParam" (Pfad fuer Slot-Parameter, kind 1/2) */
__attribute__((used, section(".text.entry_slparam")))
int32_t sv_track_sample(uint32_t t) __attribute__((weak));   /* 17.11. slview.c: Sample der Spur, beim ersten Abspielen festgelegt */
#define TRACK_SMP(t) (sv_track_sample ? sv_track_sample(t) : sv_sample(t))
uint32_t sl_param_hook(int *step, uint32_t idx, int *ctx, uint8_t *ev) {
    uint32_t v = GET_PARAM(step, idx, ctx, ev);
    if (idx != P_START && idx != P_END && idx != 9u) return v;
    volatile sl_t *s = SL;
    if (s->magic != SL_MAGIC) return v;
    uint32_t t = ctx_track(ctx);
    if (t >= 8u) return v;
    uint32_t n = sl_slices(s->mode[t]);
    /* 045: Vorhoeren der Firmware (Sample-Knob scrollen) spielt die Vorlage an. Ausser auf der offenen Slicer-Seite
     * dieser Spur oder bei gezieltem Vorhoeren eines Slices bleibt sie unangetastet (sonst klang immer der Slice der Aufnahme). */
#ifndef HOST_TEST
    if (step && (uintptr_t)step == *(const volatile uint32_t *)(0x1FFF450Cu + 0x7Cu) && sv_page_track() != t && !sv_audition_pending(t)) return v;
#endif
    if (idx == 9u && sl_is_granular(s->mode[t])) { int32_t sg = TRACK_SMP(t); return sg >= 0 ? (uint32_t)sg : v; }   /* 039 */
    if (!n) return v;
    int32_t v10, gate, st, en;
    uint32_t had = s->have10[t];
    if (idx == 9u) { int32_t smp = TRACK_SMP(t); return smp >= 0 ? (uint32_t)smp : v; }   /* 029: Sample der Spur (17.11.: beim ersten Abspielen festgelegt) */
    if (idx == P_START) {
        uint32_t ak;
        if (sv_take_audition(t, &ak)) v = ak << 6;        /* 026: Vorhoeren aus View */
        v10 = (int16_t)v; s->v10[t] = v10; s->have10[t] = 1; gate = 0x7FFF;
    } else {
        gate = (int16_t)v;
        v10 = had ? s->v10[t] : (int16_t)GET_PARAM(step, P_START, ctx, ev);
    }
    uint32_t k = sl_slice_index(v10, n);
    sl_bounds(t, k, n, gate, &st, &en);
    {   /* 045: Feinversatz des Anfangs (nur dieser Step), Ende bleibt (Gate) */
        int32_t fo = sl_fine(v10);
        if (fo) {
            int32_t fa, fb; sl_bounds(t, k, n, 0x7FFF, &fa, &fb);
            st += fo * (fb - fa) / 64;
            if (st < 0) st = 0;
            if (st >= en) st = en > 0 ? en - 1 : 0;
        }
    }
    s->n_slice++;
    if (idx == P_START) {
        slot_write(ctx, 9u, en);           /* Ende = ganzer Slice; ein gesetztes Sample End folgt danach (Index 11) */
        return (uint32_t)st;
    }
    if (!had) slot_write(ctx, 8u, st);     /* Start nicht gesetzt: Slice aus dem Standard-/Zufallswert */
    return (uint32_t)en;
}

/* ------------------------------------------------------------------ Anzeige */
static uint32_t put_u(char *b, uint32_t n, uint32_t max, uint32_t v) {
    char t[10]; uint32_t k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 10u);
    while (k && n + 1u < max) b[n++] = t[--k];
    return n;
}
static uint32_t put_s(char *b, uint32_t n, uint32_t max, const char *s) {
    while (*s && n + 1u < max) b[n++] = *s++;
    return n;
}

/* Text fuer Slicer/Granular oder 0 = Original benutzen */
/* 045: gewaehlte Sample-Spuren (Bit t, 0x1FFF4518 am Geraet bestaetigt) */
#ifdef HOST_TEST
uint32_t host_sel_mask;
#define SEL_AUDIO() (host_sel_mask & 0xFFu)
#else
#define SEL_AUDIO() (*(volatile uint32_t *)0x1FFF4518u & 0xFFu)
#endif
/* 045: Bulk (Spur gewaehlt, relativ): Sample Start verschiebt jeden Step um ganze Slices (64 Einheiten = 1 Raste = 1 Slice).
 * Nur wenn alle gewaehlten Sample-Spuren Slicer sind; gemischt bleibt die Prozent-Anzeige des Originals. */
int32_t wv_first_param(uint32_t t, uint32_t idx) __attribute__((weak));   /* wave.c: Wert im ersten gesetzten Step der Spur */
static int fmt_bulk(uint32_t idx, int32_t value, char *buf, uint32_t size) {
    volatile sl_t *s = SL;
    uint32_t m = SEL_AUDIO();
    if (idx != P_START || !m) return 0;
    for (uint32_t t = 0; t < 8u; t++) if (((m >> t) & 1u) && !sl_slices(s->mode[t])) return 0;
    (void)value;                                       /* 13.11.: Sample Start verschiebt bei markierter Spur nur den Feinversatz (slview.c) */
    uint32_t t0 = 0; while (!((m >> t0) & 1u)) t0++;
    int32_t v = wv_first_param ? wv_first_param(t0, P_START) : -1;
    uint32_t k = put_s(buf, 0, size, "Start ");
    if (v < 0) { k = put_s(buf, k, size, "~"); buf[k] = 0; return 1; }
    int32_t pc = sl_fine(v) * 100 / 64;
    if (k + 6u >= size) return 0;
    buf[k++] = pc < 0 ? '-' : '+';
    k = put_u(buf, k, size, (uint32_t)(pc < 0 ? -pc : pc));
    k = put_s(buf, k, size, "%");
    buf[k] = 0;
    return 1;
}

int sl_format(uint32_t idx, int32_t value, int32_t rel, char *buf, uint32_t size, uintptr_t step) {
    volatile sl_t *s = SL;
    if (s->magic == SL_MAGIC && rel && buf && size >= 12u) return fmt_bulk(idx, value, buf, size);
    if (s->magic != SL_MAGIC || rel || !step || !buf || size < 2u) return 0;
    uint32_t t = sl_step_track(step);
    if (t >= 8u) return 0;
    int32_t m = s->mode[t];
    uint32_t n = sl_slices(m), k = 0;
    if (value < 0) value = 0;
    if (n && idx == P_START) {
        k = put_s(buf, k, size, "Slice ");
        k = put_u(buf, k, size, sl_slice_index(value, n) + 1u);
        int32_t fo = sl_fine(value);
        if (fo) {                                         /* 045: "Slice 3 +12%" (Feinversatz in % des Slices) */
            int32_t pc = fo * 100 / 64;
            k = put_s(buf, k, size, pc < 0 ? " -" : " +");
            k = put_u(buf, k, size, (uint32_t)(pc < 0 ? -pc : pc));
            k = put_s(buf, k, size, "%");
        } else {
            k = put_s(buf, k, size, "/");
            k = put_u(buf, k, size, n);
        }
    } else if (n && idx == P_END) {
        k = put_s(buf, k, size, "Gate ");
        k = put_u(buf, k, size, ((uint32_t)value * 100u + 0x3FFFu) / 0x7FFFu);
        k = put_s(buf, k, size, "%");
    } else if (sl_is_granular(m) && idx == P_END) {
        k = put_s(buf, k, size, "Grain ");
        k = put_u(buf, k, size, gr_grain_ms(value));
        k = put_s(buf, k, size, " ms");
    } else return 0;
    buf[k] = 0;
    return 1;
}

static void fmt_any(void *self, int32_t value, int32_t rel, char *buf, uint32_t size, void *step) {
    volatile sl_t *s = SL;
    uint32_t idx = (self == (void *)&s->fmt10[0]) ? P_START : P_END;
#ifndef HOST_TEST
    wv_note_fmt(idx, value, rel, (uintptr_t)step);   /* Patch 023: Waveform-Ansicht merkt Spur/Step/Wert */
#endif
    if (sl_format(idx, value, rel, buf, size, (uintptr_t)step)) return;
    void *orig = (void *)(uintptr_t)(idx == P_START ? s->orig10 : s->orig11);
    ORIG_FMT(orig, value, rel, buf, size, step);
}

/* 045: Formatter Attack/Decay (13/14, vtable 0xA03E8), Sample (9, 0xA0400), Folder (15, 0xA0458): nur mitlesen (Waveform weiss
 * Spur/Step), Text vom Original. Eigene Objekte {vptr, Wort 1 wie Original} und je Eintrag eine vtable {fmt_gen, Rest wie Original}. */
#ifndef HOST_TEST
#define NFAD 4u
typedef struct { uint32_t orig[NFAD]; uint32_t obj[NFAD][2]; uint32_t vt[NFAD][4]; uint32_t ok; } fad_t;
#define FAD ((volatile fad_t *)0x1FFFB700u)
_Static_assert(0x1FFFB700u + sizeof(fad_t) <= 0x1FFFB780u, "fad_t vor sfc_t");
static const uint8_t FAD_IDX[NFAD] = { 13u, 14u, 9u, 15u };
static const uint32_t FAD_VT[NFAD] = { 0x000A03E8u, 0x000A03E8u, 0x000A0400u, 0x000A0458u };
static void fmt_gen(void *self, int32_t value, int32_t rel, char *buf, uint32_t size, void *step) {
    volatile fad_t *f = FAD;
    uint32_t k = 0;
    while (k < NFAD - 1u && self != (void *)&f->obj[k][0]) k++;
    wv_note_fmt(FAD_IDX[k], value, rel, (uintptr_t)step);
    uint32_t o = f->orig[k];
    ((fmt_fn)(*(const uint32_t *)(uintptr_t)(*(const uint32_t *)(uintptr_t)o)))((void *)(uintptr_t)o, value, rel, buf, size, step);
}
/* Original-Formatterobjekt eines umgebogenen Eintrags (0 = unbekannt) */
uint32_t sl_orig_fmt(uint32_t idx) {
    for (uint32_t k = 0; k < NFAD; k++) if (FAD_IDX[k] == idx && ((FAD->ok >> k) & 1u)) return FAD->orig[k];
    return 0;
}
static void fmt_install_ad(void) {
    volatile fad_t *f = FAD;
    f->ok = 0;
    for (uint32_t k = 0; k < NFAD; k++) {
        volatile uint32_t *en = ENTRY(FAD_IDX[k]);
        uint32_t o = en[4];
        if (o < 0x1FFF0000u || o >= 0x20030000u || *(const uint32_t *)(uintptr_t)o != FAD_VT[k]) continue;
        f->orig[k] = o;
        f->vt[k][0] = (uint32_t)(uintptr_t)fmt_gen;
        for (uint32_t i = 1; i < 4u; i++) f->vt[k][i] = ((const uint32_t *)(uintptr_t)FAD_VT[k])[i];
        f->obj[k][0] = (uint32_t)(uintptr_t)&f->vt[k][0]; f->obj[k][1] = ((volatile uint32_t *)(uintptr_t)o)[1];
        en[4] = (uint32_t)(uintptr_t)&f->obj[k][0];
        f->ok |= 1u << k;
    }
}
#endif

/* Formatter-Objekte der Eintraege 10/11 umbiegen (nur wenn sie die erwartete Original-vtable 0xA0338 haben).
 * Eigene Objekte {vptr, Wort 1 wie im Original} im RAM, gemeinsame vtable {fmt_any, Rest wie 0xA0338}. */
static void fmt_install(void) {
    volatile sl_t *s = SL;
    volatile uint32_t *e10 = ENTRY(P_START), *e11 = ENTRY(P_END);
    uint32_t o10 = e10[4], o11 = e11[4];
    s->fmt_ok = 0;
    if (!o10 || !o11) return;
#ifndef HOST_TEST
    if (*(const uint32_t *)(uintptr_t)o10 != 0x000A0338u || *(const uint32_t *)(uintptr_t)o11 != 0x000A0338u) return;
#endif
    s->orig10 = o10; s->orig11 = o11;
    s->vt[0] = (uint32_t)(uintptr_t)fmt_any; s->vt[1] = FMT_VT1; s->vt[2] = FMT_VT2; s->vt[3] = FMT_VT3;
    s->fmt10[0] = (uint32_t)(uintptr_t)&s->vt[0]; s->fmt10[1] = ((volatile uint32_t *)(uintptr_t)o10)[1];
    s->fmt11[0] = (uint32_t)(uintptr_t)&s->vt[0]; s->fmt11[1] = ((volatile uint32_t *)(uintptr_t)o11)[1];
    e10[4] = (uint32_t)(uintptr_t)&s->fmt10[0];
    e11[4] = (uint32_t)(uintptr_t)&s->fmt11[0];
#ifndef HOST_TEST
    fmt_install_ad();                                     /* 045 */
#endif
    s->fmt_ok = 1;
}

/* ------------------------------------------------------------------ Menue "Engines" */
static const char PAGE_NAME[] = "Engines";
static const char *const N_MODE[SL_NMODES] = {
    "Normal", "Slicer 4", "Slicer 8", "Slicer 12", "Slicer 16", "Slicer 24", "Slicer 32", "Slicer 48", "Slicer 64",
    "Granular", "Granular Rev", "Granular P-P" };
const char *sl_mode_name(int32_t m) { return (m >= 0 && m < SL_NMODES) ? N_MODE[m] : ""; }
static const char *const N_TRACK[8] = { "Track 1", "Track 2", "Track 3", "Track 4", "Track 5", "Track 6", "Track 7", "Track 8" };

/* 045: Menue zeigt je Spur nur noch Normal / Slicer (/ Granular). Die Slice-Zahl stellt man in View ein (Track Length).
 * Menuewert je Spur in eigenem RAM (EnumMenuItem schreibt int32), beim Aendern auf sl_t.mode abgebildet:
 * Slicer neu gewaehlt → 16 Slices; bleibt die Art gleich, bleibt die Slice-Zahl. */
#ifdef HOST_TEST
__attribute__((weak)) int32_t host_menu_kind[8];
#define MENU_KIND host_menu_kind
#else
#define MENU_KIND ((volatile int32_t *)0x1FFFB840u)   /* 0x1FFFB840–0x1FFFB85F */
#endif
#ifdef WITH_GRANULAR
#define N_KINDS 3u
#else
#define N_KINDS 2u
#endif
static const char *const N_KIND[3] = { "Normal", "Slicer", "Granular" };
static int32_t kind_of(int32_t m) { return sl_is_granular(m) ? 2 : sl_slices(m) ? 1 : 0; }
void sl_kind_apply(void) {
    volatile sl_t *s = SL;
    for (uint32_t t = 0; t < 8u; t++) {
        int32_t k = MENU_KIND[t];
        if (k < 0 || k >= (int32_t)N_KINDS || k == kind_of(s->mode[t])) continue;
        s->mode[t] = k == 0 ? 0 : k == 1 ? 4 : SL_GRAN;
    }
}
static void sl_changed(void *any) { (void)any; if (SL->magic == SL_MAGIC) { sl_kind_apply(); SL->t_change = MILLIS(); } }

void sl_build(void *menu, void *parent) {
    (void)menu;
    volatile sl_t *s = SL;
    if (s->magic != SL_MAGIC) return;
    for (uint32_t t = 0; t < 8u; t++) {
        MENU_KIND[t] = kind_of(s->mode[t]);
        stdfn_t fn;
        fn.store[0] = (uint32_t)(uintptr_t)(void *)s; fn.store[1] = 0;
        fn.manager = FN_MANAGER; fn.invoker = FNADDR(sl_changed);
        void *it = HEAP_ALLOC(0x60u);
        if (it) ENUM_ITEM(it, parent, N_TRACK[t], (int32_t *)(uintptr_t)&MENU_KIND[t], N_KINDS - 1u, N_KIND, &fn);
    }
}

/* von um_menu_hook (usbmenu.c) vor "USB Audio" aufgerufen */
void sl_menu_add(void *parent) {
    if (SL->magic != SL_MAGIC) return;
    void *it = HEAP_ALLOC(0x5Cu);
    if (it) MENU_ADD(it, parent, PAGE_NAME, FNADDR(sl_build), 0u);
}

/* ------------------------------------------------------------------ Datei engines.cfg (024: pro Projekt)
 * Die Firmware arbeitet im Ordner "workspace" und kopiert ihn beim Speichern nach <Projekte>/<Name>
 * (FUN_0001def0), beim Oeffnen zurueck (FUN_0001e044); beim Start laedt sie "workspace" (FUN_0001e250).
 *   Aenderung im Menue  → nach 1,5 s workspace/engines.cfg (sl_poll)
 *   Start / Oeffnen      → <Pfad>/engines.cfg lesen (Hook auf FUN_0001d504 in FUN_0001e250/FUN_0001e044), fehlt sie:
 *                          alle Spuren Normal (beim Start einmalig Rueckfall auf das alte /engines.cfg aus 021)
 *   Speichern            → <Projekte>/<Name>/engines.cfg (Hook auf SamplePool_save am Ende von FUN_0001def0)
 *   Neues Projekt        → alle Spuren Normal (Hook auf FUN_0001e268 in Project_createNew) */
static const char CFG_NAME[] = "engines.cfg";
#define CFG_BUF 4096u                                   /* 026: Platz fuer Slice-Zeilen */
static uint32_t cfg_u(char *b, uint32_t n, uint32_t v) { char t[10]; uint32_t k = 0; do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 10u); while (k) b[n++] = t[--k]; return n; }
static const char WS_DIR[] = "workspace";
static const char LEGACY[] = "/engines.cfg";

static void parse_cfg(const char *b, uint32_t n) {
    volatile sl_t *s = SL;
    uint32_t i = 0;
    while (i < n) {
        uint32_t st = i;
        while (i < n && b[i] != '\n') i++;
        uint32_t e = i++;
        if (e > st && b[e - 1] == '\r') e--;
        {                                                    /* 029: Spurwerte gT= aT= dT= oT= pT=; alte Zeilen sT.K= werden ignoriert */
            const char *L = "gadopnwmehkicyxrlzusvj"; uint32_t li = SV_CFG_N;
            for (uint32_t q = 0; q < SV_CFG_N; q++) if (e > st && b[st] == L[q]) li = q;
            if (li < SV_CFG_N && e - st >= 4u && e - st <= 10u && b[st + 1] >= '1' && b[st + 1] <= '8' && b[st + 2] == '=') {
                uint32_t q = st + 3u; int neg = 0; int32_t x = 0; uint32_t d = 0;
                if (b[q] == '-') { neg = 1; q++; }
                while (q < e && b[q] >= '0' && b[q] <= '9' && d < 6u) { x = x * 10 + (b[q++] - '0'); d++; }
                if (d && q == e) sv_cfg_set((uint32_t)(b[st + 1] - '1'), li, neg ? -x : x);
                continue;
            }
        }
        if (e - st >= 4u && b[st] == 'C' && b[st + 1] >= '1' && b[st + 1] <= '8' && b[st + 2] == '=') {   /* 12.11.: Cn=key,k:v,k:v */
            uint32_t t = (uint32_t)(b[st + 1] - '1'), q = st + 3u, key = 0;
            while (q < e && b[q] >= '0' && b[q] <= '9') key = key * 10u + (uint32_t)(b[q++] - '0');
            while (q < e && b[q] == ',') {
                q++; uint32_t kk = 0; int neg = 0; int32_t x = 0;
                while (q < e && b[q] >= '0' && b[q] <= '9') kk = kk * 10u + (uint32_t)(b[q++] - '0');
                if (q >= e || b[q] != ':') break;
                q++; if (q < e && b[q] == '-') { neg = 1; q++; }
                while (q < e && b[q] >= '0' && b[q] <= '9' && x < 100000) x = x * 10 + (b[q++] - '0');
                sv_cut_set(t, key, kk, neg ? -x : x);
            }
            continue;
        }
        /* genau "tN=V" bzw. "tN=VV" */
        if (e - st < 4u || e - st > 5u || b[st] != 't' || b[st + 1] < '1' || b[st + 1] > '8' || b[st + 2] != '=') continue;
        int32_t v = 0; uint32_t ok = 1;
        for (uint32_t k = st + 3u; k < e; k++) { if (b[k] < '0' || b[k] > '9') { ok = 0; break; } v = v * 10 + (b[k] - '0'); }
        if (ok && v < SL_NMODES) s->mode[b[st + 1] - '1'] = SL_CLAMP_MODE(v);
    }
}

/* dir [+ "/" + sub] + "/engines.cfg" (dir = 0: nur name) → p (max. n), 0 = zu lang */
static uint32_t cat_path(char *p, uint32_t n, const char *dir, const char *sub, const char *name) {
    uint32_t i = 0;
    const char *parts[3] = { dir, sub, name };
    for (uint32_t k = 0; k < 3u; k++) {
        const char *q = parts[k];
        if (!q) continue;
        if (i && p[i - 1] != '/') { if (i + 1u >= n) return 0; p[i++] = '/'; }
        for (uint32_t m = 0; q[m]; m++) { if (i + 1u >= n) return 0; p[i++] = q[m]; }   /* zu lang → 0, nie abschneiden */
    }
    p[i] = 0;
    return i;
}

/* Datei lesen: 0 = gelesen, sonst FatFs-Code (bzw. 0x100 = kein Speicher/Pfad zu lang) */
static int load_file(const char *path) {
    void *fil = MALLOC(FIL_SIZE);
    char *buf = (char *)MALLOC(CFG_BUF);
    int r = 0x100;
    if (fil && buf) {
        for (uint32_t i = 0; i < FIL_SIZE; i++) ((volatile uint8_t *)fil)[i] = 0;
        r = F_OPEN(fil, path, FA_READ);
        if (r == 0) {
            uint32_t br = 0;
            if (F_READ(fil, buf, CFG_BUF - 1u, &br) == 0 && br < CFG_BUF) parse_cfg(buf, br);
            F_CLOSE(fil);
        }
    }
    if (fil) FREE(fil);
    if (buf) FREE(buf);
    return r;
}

static int save_file(const char *path, const int32_t *snap) {
    static const char HDR[] = "# Polyend Play Engines (Patch 021/024): 0=Normal 1..8=Slicer 4/8/12/16/24/32/48/64 9=Granular 10=Granular Rev 11=Granular P-P\n";
    void *fil = MALLOC(FIL_SIZE);
    char *buf = (char *)MALLOC(CFG_BUF);
    int ok = 0;
    if (fil && buf) {
        uint32_t n = 0;
        for (const char *p = HDR; *p && n < 190u; p++) buf[n++] = *p;
        for (uint32_t k = 0; k < 8u; k++) {
            buf[n++] = 't'; buf[n++] = (char)('1' + k); buf[n++] = '=';
            if (snap[k] >= 10) buf[n++] = (char)('0' + snap[k] / 10);
            buf[n++] = (char)('0' + snap[k] % 10); buf[n++] = '\n';
        }
        for (uint32_t t = 0; t < 8u; t++)                    /* 029: Spurwerte, nur wenn gesetzt */
            for (uint32_t q = 0; q < SV_CFG_N && n + 16u < CFG_BUF; q++) {
                int32_t x = sv_cfg_get(t, q);
                if (!x) continue;
                buf[n++] = "gadopnwmehkicyxrlzusvj"[q]; buf[n++] = (char)('1' + t); buf[n++] = '=';
                if (x < 0) { buf[n++] = '-'; x = -x; }
                n = cfg_u(buf, n, (uint32_t)x); buf[n++] = '\n';
            }
        for (uint32_t t = 0; t < 8u; t++) {                  /* 12.11.: Handschnitte Cn=key,k:v,... */
            uint32_t n_ = sl_slices(SL->mode[t]);
            if (!n_ || !sv_cut_any(t, n_) || n + 40u >= CFG_BUF) continue;
            buf[n++] = 'C'; buf[n++] = (char)('1' + t); buf[n++] = '=';
            n = cfg_u(buf, n, sv_cut_key(t, n_));
            for (uint32_t k = 0; k < n_ && k < 32u && n + 16u < CFG_BUF; k++) {
                int32_t x = sv_cut(t, k, n_);
                if (!x) continue;
                buf[n++] = ','; n = cfg_u(buf, n, k); buf[n++] = ':';
                if (x < 0) { buf[n++] = '-'; x = -x; }
                n = cfg_u(buf, n, (uint32_t)x);
            }
            buf[n++] = '\n';
        }
        for (uint32_t i = 0; i < FIL_SIZE; i++) ((volatile uint8_t *)fil)[i] = 0;
        uint32_t bw = 0;
        int r = F_OPEN(fil, path, FA_WRITE_NEW);
        if (r == 0) {
            int w = F_WRITE(fil, buf, n, &bw);
            int c = F_CLOSE(fil);
            if (w == 0 && c == 0 && bw == n) ok = 1;
        }
    }
    if (fil) FREE(fil);
    if (buf) FREE(buf);
    return ok;
}

static void stop_granular_now(void) {
    for (uint32_t t = 0; t < 8u; t++) { gr_t *g = GR_STATE(t); if (!g) continue; for (uint32_t j = 0; j < GR_MAXG; j++) g->act[j] = 0; g->rel = 0; g->next = 0; }
}

/* Werte aus <dir>/engines.cfg; ws = 1: Ordner ist "workspace" (Start) */
void sl_load_dir(const char *dir, int ws) {
    volatile sl_t *s = SL;
    if (s->magic != SL_MAGIC) return;
    for (uint32_t k = 0; k < 8u; k++) { s->mode[k] = 0; sv_clear_track(k); }
    char path[300];
    int r = cat_path(path, sizeof path, dir, 0, CFG_NAME) ? load_file(path) : 0x100;
    int legacy = 0;
    if (r != 0 && ws && !s->loaded) { legacy = load_file(LEGACY) == 0; }   /* einmalig: Datei aus Patch 021 */
    stop_granular_now();
    for (uint32_t k = 0; k < 8u; k++) {
        s->seen[k] = s->mode[k];
        s->saved[k] = (ws && r == 0) ? s->mode[k] : -1;      /* -1: workspace/engines.cfg neu schreiben */
    }
    s->load_res = (uint32_t)(r & 0xFF) | (r == 0 ? 0x10000u : 0u) | (legacy ? 0x20000u : 0u) | (ws ? 0x40000u : 0u);
    s->t_change = MILLIS() - SAVE_DELAY_MS;                  /* ohne Wartezeit speichern */
    s->loaded = 1;
}

/* Projekt gespeichert: <dir>/<name>/engines.cfg */
void sl_save_project(const char *dir, const char *name) {
    volatile sl_t *s = SL;
    if (s->magic != SL_MAGIC) return;
    int32_t snap[8];
    for (uint32_t k = 0; k < 8u; k++) snap[k] = s->mode[k];
    char path[300];
    if (cat_path(path, sizeof path, dir, name, CFG_NAME) && save_file(path, snap)) s->saves++;
    else s->save_err++;
}

void sl_new_project(void) {
    volatile sl_t *s = SL;
    if (s->magic != SL_MAGIC) return;
    for (uint32_t k = 0; k < 8u; k++) { s->mode[k] = 0; s->seen[k] = 0; s->saved[k] = -1; sv_clear_track(k); }
    stop_granular_now();
    s->t_change = MILLIS() - SAVE_DELAY_MS;
    s->loaded = 1;
}

/* Sequencer-Stopp: Granular-Spuren blenden im naechsten Block aus (granular.c) */
void sl_transport(int playing) {
    gr_set_playing(playing);
    if (SL->magic != SL_MAGIC || playing) return;
    for (uint32_t t = 0; t < 8u; t++) if (sl_is_granular(SL->mode[t]) && GR_STATE(t) && !gr_cfg(t)->drone) GR_STATE(t)->rel = 1;   /* Drone: laeuft bei Stop weiter */
#ifndef HOST_TEST
    for (uint32_t t = 0; t < 8u; t++) if (sl_slices(SL->mode[t]) && RAM_OK(SL->node[t])) sl_stop_fade((uint8_t *)(uintptr_t)(SL->node[t] + 0xD8u));   /* 045 */
#endif
}

static void save(void) {
    volatile sl_t *s = SL;
    int32_t snap[8];
    for (uint32_t k = 0; k < 8u; k++) snap[k] = s->mode[k];
    char path[64];
    int ok = cat_path(path, sizeof path, WS_DIR, 0, CFG_NAME) && save_file(path, snap);
    if (ok) { s->saves++; s->save_fail = 0; } else { s->save_err++; s->save_fail++; }
    if (ok || s->save_fail >= 5u) for (uint32_t k = 0; k < 8u; k++) s->saved[k] = snap[k];
    s->t_change = MILLIS();
}

#ifndef HOST_TEST
/* 0x1E0D2 (FUN_0001e044, Projekt oeffnen) und 0x1E206 (FUN_0001e250, Start): bl FUN_0001d504(ps, pfad) */
__attribute__((used, section(".text.entry_slload")))
void sl_load_hook(void *ps, const char *path) {
    ((void (*)(void *, const char *))0x0001D505u)(ps, path);
    sl_load_dir(path, path[0] == 'w' && path[1] == 'o' && path[2] == 'r' && path[9] == 0);
}
/* 0x1DFF4 (FUN_0001def0, Projekt speichern): bl SamplePool_save(ps, pfad, x) als letzter Schritt */
__attribute__((used, section(".text.entry_slsave")))
uint32_t sl_save_hook(uint8_t *ps, const char *path, uint32_t x) {
    uint32_t r = ((uint32_t (*)(void *, const char *, uint32_t))0x0001DC2Du)(ps, path, x);
    sl_save_project((const char *)(ps + 0xE4), (const char *)(ps + 0x2C));
    return r;
}
/* 0x41A92 (Project_createNew): bl FUN_0001e268(ps) */
__attribute__((used, section(".text.entry_slnew")))
void sl_new_hook(void *ps) {
    ((void (*)(void *))0x0001E269u)(ps);
    sl_new_project();
}
/* vtable Engine 0xA99B4 +0x20 (FUN_00045ae4 setPlaying(engine, an, x)) */
__attribute__((used, section(".text.entry_slplay")))
void sl_play_hook(void *eng, int playing, uint32_t x) {
    ((void (*)(void *, int, uint32_t))0x00045AE5u)(eng, playing, x);
    sl_transport(playing);
}
#endif

/* Hauptschleife (loop_hook in usbup.c) */
void sl_poll(void) {
    volatile sl_t *s = SL;
#ifndef HOST_TEST
    sv_poll();                          /* Patch 026: View-Slicer → Waveform in View */
    wv_poll();                          /* Patch 023: Waveform rechnen/hochladen (Hauptschleife) */
    lr_poll();                          /* Patch 023: Statusanzeige USB-Aufnahme */
    tt_poll();                          /* 045: Tap Tempo, Kopfzeile im Menue */
#endif
    if (s->magic != SL_MAGIC) return;
    for (uint32_t k = 0; k < 8u; k++) s->mode[k] = SL_CLAMP_MODE(s->mode[k]);
    if (!s->loaded) {                                        /* 024: gelesen wird im Lade-Hook; ohne ihn nach 20 s aufgeben */
        if (MILLIS() >= LOAD_GIVEUP_MS) s->loaded = 1;
        return;
    }
    uint32_t now = MILLIS(), diff = 0, moved = 0;
    for (uint32_t k = 0; k < 8u; k++) {
        if (s->mode[k] != s->seen[k]) { s->seen[k] = s->mode[k]; moved = 1; }
        if (s->mode[k] != s->saved[k]) diff = 1;
    }
    if (moved) s->t_change = now;
    {                                                        /* 026: geaenderte Slice-Werte (View) wie eine Aenderung behandeln */
        volatile uint32_t *dirty = sv_dirty_flag();
        if (dirty && *dirty) { if (*dirty == 1u) { *dirty = 2u; s->t_change = now; } diff = 1; }
    }
    if (diff && now - s->t_change >= SAVE_DELAY_MS) { volatile uint32_t *dirty = sv_dirty_flag(); if (dirty) *dirty = 0; save(); }
}
