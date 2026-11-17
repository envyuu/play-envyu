/* Patch 021 – Granular-Wiedergabe fuer Audio-Spuren (ersetzt fuer diese Spur Sampler_render 0x19BC8)
 *
 * Einhaengen: vtable der Sampler-Stimme 0x9CED0 [0] (TrackVoice_update 0x18DD0) → gr_update. Fuer alle Spuren, die
 * nicht auf Granular stehen (oder deren Stimme nicht spielt / nicht mono ist), wird das Original aufgerufen.
 *
 * Klang (039): bis zu 8 Grain-Plaetze je Stimme. Ein Grain ist glen Ausgabe-Samples lang, blendet linear ueber fade Samples ein und aus.
 * Abstand der Grains: glen − fade geteilt durch n − 1 (bei 2 Grains wie bisher, Fenstersumme 1). Die Lautstaerke gleicht die Ueberlappung aus.
 * Parameter aus den Spur-Slots (Perform und Steps wirken mit):
 *   Slot 8  Sample Start → Position · Slot 9 Sample End → Grain Size 5…1000 ms
 *   Slot 10 Attack, Slot 11 Decay → Huellkurve der Wolke bis 4 s. Decay 0 und 100 % = klingt durch (bis zum naechsten Step, Pad los, Stop).
 * Je Spur (gr_cfg_t): Grains, Window, Spray, Pitch Spray, Scan, LFO (Rate, Depth, Ziel Position/Spray/Size/Pitch).
 * Tonhoehe und Lautstaerke wie im Original. Richtung aus dem Menue bzw. Play Mode.
 */
#include <stdint.h>
#include "slicer.h"

#ifdef HOST_TEST
extern uint8_t host_sl_ram[];
extern uint8_t host_gr_ram[];
#define SL       ((volatile sl_t *)(void *)host_sl_ram)
#define CFG(t)   ((gr_cfg_t *)(void *)(host_gr_ram + 0x20u * (t)))
#define GRP      (*(gr_t **)(void *)(host_gr_ram + 0x100u))
#include <stdlib.h>
#define GR_ALLOC(n) calloc(1, (n))
#else
#define SL       ((volatile sl_t *)0x1FFF9800u)
#define CFG(t)   ((gr_cfg_t *)(0x1FFF9900u + 0x20u * (t)))
#define GRP      (*(gr_t **)0x1FFF9A00u)                           /* Heap-Block mit 8 gr_t */
#define GR_ALLOC(n) (((void *(*)(uint32_t))0x00056F5Du)(n))
#define GAIN_TAB (*(const int16_t *volatile *)0x20001C28u)        /* Pegeltabelle 10001 × s16 (FUN_00018DFC) */
#define ORIG_UPDATE  ((void (*)(uint8_t *))0x00018DD1u)           /* TrackVoice_update */
#define BLOCK_ALLOC  ((uint8_t *(*)(uint32_t))0x00017195u)        /* AudioBlock_alloc */
#define BLOCK_FREE   ((void (*)(uint8_t *))0x00017245u)           /* AudioBlock_release */
#define TRANSMIT     ((void (*)(uint8_t *, uint8_t *, uint32_t))0x00017295u)   /* AudioStream_transmit */
#endif

#define FS        44100u
#define MIN_FADE  32u
#define MAX_INC   (64u << 16)

/* Grain-Laenge aus Sample End (0…0x7FFF): 5 + 995·x² ms */
uint32_t gr_grain_ms(int32_t v) {
    if (v < 0) v = 0;
    if (v > 0x7FFF) v = 0x7FFF;
    uint32_t x = (uint32_t)v + ((uint32_t)v >> 14);         /* 0x7FFF → 0x8000, damit 0x7FFF genau 1000 ms ergibt */
    return 5u + (uint32_t)(((uint64_t)995u * x * x) >> 30);
}

/* float (Bitmuster, positiv) → Q16, ohne FPU-/Bibliotheksaufruf */
uint32_t gr_f2q16(uint32_t b) {
    if (b >> 31) return 0;
    int32_t e = (int32_t)((b >> 23) & 0xFFu);
    if (e == 0) return 0;
    if (e == 0xFF) return MAX_INC;
    uint32_t m = (b & 0x7FFFFFu) | 0x800000u;
    int32_t sh = e - 127 - 7;              /* Wert = m·2^(e−150), Q16 = m·2^(e−134) */
    uint32_t q;
    if (sh >= 8) return MAX_INC;
    if (sh >= 0) q = m << sh;
    else if (sh > -32) q = m >> (uint32_t)(-sh);
    else q = 0;
    return q > MAX_INC ? MAX_INC : q;
}

void gr_params(gr_par_t *p, int32_t v_pos, int32_t v_len, int32_t v_shape, int32_t v_spray, int32_t dirmode) {
    if (v_pos < 0) v_pos = 0;
    if (v_pos > 0x7FFF) v_pos = 0x7FFF;
    if (v_shape < 0) v_shape = 0;
    if (v_shape > 10000) v_shape = 10000;
    if (v_spray < 0) v_spray = 0;
    if (v_spray > 10000) v_spray = 10000;
    uint32_t len = p->len;
    p->center = (uint32_t)(((uint64_t)len * (uint32_t)v_pos) >> 15);
    if (len && p->center >= len) p->center = len - 1u;
    uint32_t ms = gr_grain_ms(v_len);
    p->glen = ms * 441u / 10u;
    uint32_t f = p->glen * (uint32_t)v_shape / 20000u;       /* glen <= 44100 → kein Ueberlauf */
    if (f < MIN_FADE) f = MIN_FADE;
    if (f > p->glen / 2u) f = p->glen / 2u;
    p->fade = f;
    p->spray = (uint32_t)v_spray * (FS / 2u) / 10000u;
    p->dirmode = dirmode;
    p->pspray = 0u;
    p->rate = 100u;
    gr_set_count(p, 2u);
}

/* alle 8 Zustaende loeschen (aus sl_reset beim Start; SRAM_L ist nach dem Einschalten nicht definiert) */
#ifdef HOST_TEST
#define GR_PLAYING (*(volatile uint32_t *)(void *)(host_gr_ram + 0x104u))
#else
#define GR_PLAYING (*(volatile uint32_t *)0x1FFF9A04u)
#endif
void gr_set_playing(int playing) { GR_PLAYING = playing ? 1u : 0u; }
int sv_steps_held_now(void) __attribute__((weak));
gr_t *gr_state(uint32_t t) { return GRP ? &GRP[t & 7u] : 0; }
gr_cfg_t *gr_cfg(uint32_t t) { return CFG(t & 7u); }
void gr_cfg_default(uint32_t t) {
    volatile gr_cfg_t *c = CFG(t & 7u);
    c->n = 4; c->win = 50; c->spray = 0; c->pspray = 0; c->scan = 0; c->lrate = 12; c->ldepth = 0; c->ltgt = 0; c->pmode = 0; c->drone = 0; c->speed = 100; c->rate = 10;
}

/* 043: Nur beim Start (sl_init). Der Zeiger GRP steht im SRAM und ist nach dem Einschalten oder nach einer anderen Firmware NICHT definiert:
 * nie auswerten, immer neu anlegen. Der alte Code schrieb sonst an eine zufaellige Adresse, die Firmware startete nicht. */
void gr_clear(void) {
    GR_PLAYING = 0u;                                     /* 044: SRAM ist beim Start nicht definiert */
#ifdef HOST_TEST
    if (GRP) { volatile uint8_t *h = (volatile uint8_t *)GRP; for (uint32_t i = 0; i < 8u * sizeof(gr_t); i++) h[i] = 0; }
    else
#endif
    {
        volatile uint8_t *p = (volatile uint8_t *)GR_ALLOC(8u * sizeof(gr_t));
        if (p) for (uint32_t i = 0; i < 8u * sizeof(gr_t); i++) p[i] = 0;
        GRP = (gr_t *)(uintptr_t)p;
    }
    for (uint32_t t = 0; t < 8u; t++) gr_cfg_default(t);
}

void gr_reset(gr_t *g, uint32_t seed) {
    for (uint32_t j = 0; j < GR_MAXG; j++) { g->act[j] = 0; g->age[j] = 0; }
    g->slot = 0; g->pp = 0; g->next = 0;
    if (!g->live) { g->rnd = seed ? seed : 0x2545F491u; g->live = 1; }
}

/* 039: Ausgleich der Ueberlappung Q15 ~ 1/sqrt(n/2) */
void gr_set_count(gr_par_t *p, uint32_t n) {
    static const uint16_t G[9] = { 32768, 32768, 32768, 26755, 23170, 20724, 18919, 17515, 16384 };
    if (n < 1u) n = 1u;
    if (n > GR_MAXG) n = GR_MAXG;
    p->n = n; p->gain = G[n];
}
/* 2^(k/12) Q16 fuer k = 0…24 */
static uint32_t semi_ratio(int32_t k) {
    static const uint32_t R[25] = { 65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
        131072, 138866, 147123, 155872, 165140, 174960, 185364, 196386, 208064, 220436, 233544, 247431, 262144 };
    if (k >= 0) return R[k > 24 ? 24 : k];
    return (uint32_t)(0x80000000u / R[-k > 24 ? 24 : -k]) << 1;
}
/* LFO: Dreieck -32768…32767 */
int gr_lfo_tri(uint32_t ph) {
    uint32_t u = ph >> 16;                                   /* 0…65535 */
    int32_t v = u < 32768u ? (int32_t)(u * 2u) - 32768 : 32767 - (int32_t)((u - 32768u) * 2u);
    return v > 32767 ? 32767 : v;
}
/* LFO-Rate 0…31: 0,02 Hz … 30 Hz, Millihertz */
uint32_t gr_lfo_mhz(uint32_t i) {
    static const uint16_t H[32] = { 20, 30, 40, 50, 70, 100, 130, 170, 220, 300, 400, 500, 650, 800, 1000, 1300, 1600, 2000, 2500,
        3000, 4000, 5000, 6500, 8000, 10000, 12000, 15000, 18000, 21000, 24000, 27000, 30000 };
    return H[i > 31u ? 31u : i];
}
/* Huellkurve: Slot-Wert 0…10000 → Bloecke zu 2,9 ms (x² bis 4 s); 0 = sofort */
uint32_t gr_attack_blocks(int32_t v) {
    if (v <= 0) return 0u;
    if (v > 10000) v = 10000;
    uint32_t x = (uint32_t)v / 100u;
    uint32_t ms = x * x * 4u / 10u;
    uint32_t b = ms * 10u / 29u;
    return b ? b : 1u;
}

static uint32_t xr(gr_t *g) { uint32_t x = g->rnd; x ^= x << 13; x ^= x >> 17; x ^= x << 5; g->rnd = x ? x : 1u; return x; }

static void spawn(gr_t *g, const gr_par_t *p) {
    uint32_t j = g->slot; g->slot = (uint8_t)((j + 1u) % p->n);
    int32_t base = (int32_t)p->center;
    if (p->spray) {
        uint32_t r = xr(g) % (2u * p->spray + 1u);
        base += (int32_t)r - (int32_t)p->spray;
    }
    int8_t d = 1;
    if (p->dirmode == 1) d = -1;
    else if (p->dirmode == 2) { d = g->pp ? -1 : 1; g->pp ^= 1u; }
    else if (p->dirmode == 3) d = (xr(g) & 0x10000u) ? -1 : 1;   /* 040: Random */
    uint32_t ginc = p->inc;
    if (p->pspray) {                                         /* 039: Pitch Spray je Grain */
        int32_t r = (int32_t)(xr(g) % (2u * p->pspray + 1u)) - (int32_t)p->pspray;
        ginc = (uint32_t)(((uint64_t)p->inc * semi_ratio(r)) >> 16);
        if (ginc > MAX_INC) ginc = MAX_INC;
    }
    uint32_t span = (uint32_t)(((uint64_t)p->glen * ginc) >> 16);
    if (d < 0) base += (int32_t)span;              /* rueckwaerts: von hinten zur Position hin */
    if (base < 0) base = 0;
    if ((uint32_t)base >= p->len) base = (int32_t)p->len - 1;
    g->ipos[j] = base; g->frac[j] = 0; g->age[j] = 0; g->dir[j] = d; g->act[j] = 1;
    g->gl[j] = p->glen; g->gf[j] = p->fade; g->ginc[j] = ginc;
}

static inline int32_t ssat16(int32_t x) {
#ifdef HOST_TEST
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
#else
    int32_t r; __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(x)); return r;
#endif
}

/* Grain j fuer n Samples auf out addieren; Fenster abschnittsweise (Anstieg / voll / Abfall) mit konstanter Steigung */
static void grain_run(gr_t *g, uint32_t j, const gr_par_t *p, int16_t *out, uint32_t n) {
    const int16_t *data = p->data;
    const uint32_t lim = p->len - 1u, glen = g->gl[j], fade = g->gf[j];
    const int32_t winc = (int32_t)((1u << 30) / fade);                      /* Fenster Q30 (Summe zweier Fenster = 1 auf 2^-24 genau) */
    const int64_t sinc = g->dir[j] > 0 ? (int64_t)g->ginc[j] : -(int64_t)g->ginc[j];
    const int32_t gn = p->gain;
    int64_t pos = (int64_t)g->ipos[j] * 65536 + g->frac[j];
    uint32_t age = g->age[j];
    while (n) {
        uint32_t seg; int32_t w, dw;
        if (age < fade) { seg = fade - age; w = (int32_t)age * winc; dw = winc; }
        else if (age < glen - fade) { seg = glen - fade - age; w = 1 << 30; dw = 0; }
        else { seg = glen - age; w = (int32_t)(glen - age) * winc; dw = -winc; }
        if (seg > n) seg = n;
        for (uint32_t k = 0; k < seg; k++) {
            int32_t ip = (int32_t)(pos >> 16), s = 0;
            if ((uint32_t)ip < lim) {
                int32_t s0 = data[ip], s1 = data[ip + 1];
                s = s0 + (((s1 - s0) * (int32_t)(((uint32_t)pos & 0xFFFFu) >> 1)) >> 15);
            }
            s = (s * gn) >> 15;
            out[k] = (int16_t)ssat16(out[k] + ((s * (w >> 15)) >> 15));
            pos += sinc; w += dw;
        }
        out += seg; n -= seg; age += seg;
        if (age >= glen) { g->act[j] = 0; break; }
    }
    g->ipos[j] = (int32_t)(pos >> 16); g->frac[j] = (uint32_t)pos & 0xFFFFu; g->age[j] = age;
}

/* n Samples Grain-Summe (ohne Pegel) nach out (Fenstersumme <= 1, gesaettigt) */
void gr_render(gr_t *g, const gr_par_t *p, int16_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) out[i] = 0;
    if (!p->data || p->len < 4u || p->glen < 2u * MIN_FADE) return;
    uint32_t i = 0;
    while (i < n) {
        if (g->next == 0) {
            spawn(g, p);
            uint32_t d = p->n > 1u ? (p->glen - p->fade) / (p->n - 1u) : p->glen;   /* bei 2 Grains: glen − fade wie bisher */
            if (p->rate > 100u) d = d * p->rate / 100u;      /* 044: Grain Rate: mehr Abstand (d <= 44100, Rate <= 1000: passt in 32 Bit) */
            if (d < 32u) d = 32u;                            /* nie mehr als ein Grain je 32 Samples: begrenzt die Last */
            g->next = d;
        }
        uint32_t chunk = n - i;
        if (chunk > g->next) chunk = g->next;
        for (uint32_t j = 0; j < GR_MAXG; j++) if (g->act[j]) grain_run(g, j, p, out + i, chunk);
        g->next -= chunk;
        i += chunk;
    }
}

#ifndef HOST_TEST
/* Slot-Wert wie FUN_00014658, aber ohne das Dirty-Flag (+0xE) zu loeschen */
static int32_t slot_eval(const uint8_t *s) {
    const int16_t *src = (const int16_t *)(const void *)s;
    uint8_t mask = s[0xC], mode = s[0xD];
    int32_t v = 0;
    if (mode == 2) {
        if (mask & 4u) { v = src[2]; if (mask & 8u) v += src[3]; }
        else for (uint32_t i = 0; i < 4u; i++) if (mask & (1u << i)) v += src[i];
    } else if (mode == 0) {
        for (uint32_t i = 0; i < 4u; i++) if (mask & (1u << i)) v += src[i];
    } else if (mode == 1) {
        v = 0x8000;
        for (uint32_t i = 0; i < 4u; i++) if (mask & (1u << i)) { int32_t t = v * src[i]; if (t < 0) t += 0x7FFF; v = t >> 15; }
    }
    int32_t lo = ((const int16_t *)(const void *)s)[4], hi = ((const int16_t *)(const void *)s)[5];
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


/* vtable 0x9CED0 [0] */
__attribute__((used, section(".text.entry_grupd")))
/* 036: Die Firmware rechnet die Huellkurve je Audio-Block (Index = Position/128 + 1) und rampt ueber den ganzen Block. Endet der Slice
 * mitten im Block, wird der Rest hart auf 0 gesetzt: Knacken, je nach Lage des Endes im Raster und staerker bei hoeheren Noten. Darum
 * liegt das Ziel der Huellkurve (Stimme +0x34) einen Audio-Block vor dem Ende, die Decay-Laenge bleibt. Vorwaerts; nur Slicer-Spuren. */
void sl_fade_fix(uint8_t *st) {
    if (st[0x39]) return;
    uint32_t end = *(uint32_t *)(st + 0x2C), adv = ((128u * (gr_f2q16(*(uint32_t *)(st + 0x3C)) >> 4)) >> 12) + 1u;
    int32_t b0 = *(int32_t *)(st + 0x30), e0 = *(int32_t *)(st + 0x34), d0 = *(int32_t *)(st + 0x5C), a0 = *(int32_t *)(st + 0x58);
    int32_t e = (int32_t)((end > 2u * adv ? end - 2u * adv : 0u) >> 7) + 1;   /* jeder Block ab end - 2·adv hat Ziel 0 und endet vor dem Ende */
    if (e > e0) e = e0;
    if (e <= b0) e = b0 + 1;
    int32_t d = e - (e0 - d0);
    if (d >= e) d = e - 1;                               /* d == e hiesse: gar keine Ausblendung */
    if (d < b0) d = b0;
    if (a0 > d) a0 = d;
    *(int32_t *)(st + 0x58) = a0;
    *(int32_t *)(st + 0x5C) = d;
    *(int32_t *)(st + 0x34) = e;
}

/* 045: Sequencer-Stopp auf einer Slicer-Spur: Ausblenden ab jetzt ueber 3 Audio-Bloecke (~9 ms) statt bis zum Slice-Ende.
 * Reihenfolge d, e, Ende: der Audio-Interrupt sieht zwischendurch nie d > e. sl_fade_fix haelt danach Decay-Laenge = 3 Bloecke. */
void sl_stop_fade(uint8_t *st) {
    if (!st[0x18] || st[0x39]) return;
    uint32_t pos = *(volatile uint32_t *)(st + 0x44), end = *(volatile uint32_t *)(st + 0x2C);
    uint32_t adv = ((128u * (gr_f2q16(*(uint32_t *)(st + 0x3C)) >> 4)) >> 12) + 1u;
    uint32_t nend = pos + 5u * adv;
    if (nend >= end) return;                             /* endet ohnehin gleich */
    int32_t cur = (int32_t)(pos >> 7) + 1, b0 = *(volatile int32_t *)(st + 0x30);
    int32_t d = cur < b0 ? b0 : cur, e = (int32_t)((nend - 2u * adv) >> 7) + 1;
    if (e <= d) e = d + 1;
    if (*(volatile int32_t *)(st + 0x58) > d) *(volatile int32_t *)(st + 0x58) = d;
    *(volatile int32_t *)(st + 0x5C) = d;
    *(volatile int32_t *)(st + 0x34) = e;
    *(volatile uint32_t *)(st + 0x2C) = nend;
}

void gr_update(uint8_t *st) {
    volatile sl_t *s = SL;
    const int16_t *gt = GAIN_TAB;
    if (!gt) return;                                         /* wie TrackVoice_update */
    uint32_t t = 8u;
    if (s->magic == SL_MAGIC)
        for (uint32_t k = 0; k < 8u; k++) if (s->node[k] && s->node[k] + 0xD8u == (uint32_t)(uintptr_t)st) { t = k; break; }
    if (t < 8u && st[0x54] && st[0x18] && sl_slices(s->mode[t])) sl_fade_fix(st);   /* 036: Slicer: Ausblendung vor dem Ende fertig */
    if (t >= 8u || !sl_is_granular(s->mode[t]) || !GRP || !st[0x18] || *(uint32_t *)(st + 0x20) != 1u ||
        !*(uint32_t *)(st + 0x1C) || *(uint32_t *)(st + 0x24) < 4u) { ORIG_UPDATE(st); return; }
    uint8_t *blk = BLOCK_ALLOC(1u);
    if (!blk) return;
    gr_t *g = gr_state(t);
    const volatile gr_cfg_t *c = CFG(t);
    /* 040: Neue Note reisst die Wolke nicht ab: laufende Grains klingen aus, Huelle laeuft vom jetzigen Pegel weiter. Drone: Scan laeuft weiter. */
    if (st[0x54]) { g->rel = 0; g->rel2 = 0; g->stage = 0; if (!c->drone) { g->scan = 0; g->sdir = 0; } }
    /* 044: Bei gestopptem Sequencer klingt eine Wolke nur, solange Pad oder Step gehalten sind oder Drone an ist */
    if (!GR_PLAYING && !g->hold && !c->drone && !sv_steps_held_now()) g->rel2 = 1u;
    if (!g->live) gr_reset(g, 0x9E3779B9u ^ (uint32_t)(uintptr_t)st ^ s->n_begin);
    const uint8_t *node = st - 0xD8;
    gr_par_t p;
    p.data = (const int16_t *)(uintptr_t)*(uint32_t *)(st + 0x1C);
    p.len = *(uint32_t *)(st + 0x24);
    p.inc = gr_f2q16(*(uint32_t *)(st + 0x3C));
    int32_t v_pos = slot_eval(node + 4 + 16 * 8), v_size = slot_eval(node + 4 + 16 * 9), v_spray = (int32_t)c->spray * 100;
    /* LFO: einmal je Block */
    g->lphase += gr_lfo_mhz(c->lrate) * 12466u;              /* 2^32 / (344,53 Bloecke/s × 1000) */
    int32_t lv = c->ldepth ? gr_lfo_tri(g->lphase) * (int32_t)c->ldepth / 100 : 0;     /* -32768…32767 */
    if (lv && c->ltgt == 1u) { int32_t y = v_spray + (lv + 32768) * 10000 / 65536 * (int32_t)c->ldepth / 100; v_spray = y > 10000 ? 10000 : y; }
    if (lv && c->ltgt == 2u) { int32_t y = v_size + lv; v_size = y < 0 ? 0 : y > 0x7FFF ? 0x7FFF : y; }
    gr_params(&p, v_pos, v_size, (int32_t)c->win * 100, v_spray, c->pmode == 1u ? 3 : c->pmode ? 0 : s->mode[t] - SL_GRAN);
    gr_set_count(&p, c->n);
    p.pspray = c->pspray;
    p.rate = (uint32_t)(c->rate ? c->rate : 10u) * 10u;
    if (c->speed && c->speed != 100u) {                      /* 044: Grain Speed: Grains laufen langsamer oder schneller */
        p.inc = (uint32_t)(((uint64_t)p.inc * c->speed) >> 7) * 128u / 100u;
        if (p.inc > MAX_INC) p.inc = MAX_INC;
    }
    if (lv && c->ltgt == 3u) { int32_t k = lv * 12 / 32768; p.inc = (uint32_t)(((uint64_t)p.inc * semi_ratio(k)) >> 16); if (p.inc > MAX_INC) p.inc = MAX_INC; }
    if (lv && c->ltgt == 0u) {                               /* Position: bis +/- halbes Sample bei Depth 100 */
        int64_t off = ((int64_t)p.len * lv) >> 16;
        int64_t cn = (int64_t)p.center + off;
        for (uint32_t q = 0; cn < 0 && q < 4u; q++) cn += p.len;
        for (uint32_t q = 0; cn >= (int64_t)p.len && q < 4u; q++) cn -= p.len;
        p.center = (cn < 0 || cn >= (int64_t)p.len) ? 0u : (uint32_t)cn;
    }
    if (c->scan || c->pmode == 3u) {                         /* Scan: wandert rund; Bounce: pendelt zwischen den Enden; Walk: driftet zufaellig */
        int32_t step = (int32_t)(p.inc >> 9) * c->scan / 100;   /* Frames je Block: 128 × Schrittweite bei 100 % */
        if (c->pmode == 3u) {
            int32_t mag = (c->scan < 0 ? -c->scan : c->scan) + 5;
            int32_t rr = (int32_t)(xr(g) & 0xFFFFu) - 0x8000;
            step = (int32_t)(p.inc >> 9) * mag / 100 * rr / 0x4000;
        }
        if (c->pmode == 2u && g->sdir) step = -step;
        /* 042: scan ist vorzeichenbehaftet (Bounce legt negative Versaetze ab). Ausserhalb des Samples → neu anfangen: nie lange Schleifen im Audio-Interrupt. */
        int32_t sc0 = (int32_t)g->scan;
        if (sc0 <= -(int32_t)p.len || sc0 >= (int32_t)p.len) sc0 = 0;
        int64_t sc = (int64_t)sc0 + step;
        if (c->pmode == 2u) {                                /* Bounce: an den Enden umkehren */
            int64_t abs_ = (int64_t)p.center + sc;
            if (abs_ < 0) { sc = -(int64_t)p.center; g->sdir ^= 1u; }
            else if (abs_ >= (int64_t)p.len) { sc = (int64_t)p.len - 1 - (int64_t)p.center; g->sdir ^= 1u; }
            g->scan = (uint32_t)(int32_t)sc;
            p.center = (uint32_t)((int64_t)p.center + sc);
        } else {
            uint32_t lm = p.len;                             /* step <= 8192 Frames: bei kurzem Sample auf 16 Runden begrenzt */
            for (uint32_t q = 0; sc < 0 && q < 16u; q++) sc += lm;
            for (uint32_t q = 0; sc >= (int64_t)lm && q < 16u; q++) sc -= lm;
            if (sc < 0 || sc >= (int64_t)lm) sc = 0;
            g->scan = (uint32_t)sc;
            uint32_t cn = p.center + g->scan;
            p.center = cn >= p.len ? cn - p.len : cn;
        }
    }
    g->cshow = p.center; g->sshow = p.spray;
    int16_t *out = (int16_t *)(void *)(blk + 8);
    gr_render(g, &p, out, 128u);
    {                                                        /* 039: Huellkurve der Wolke, je Block linear */
        int32_t dv = slot_eval(node + 4 + 16 * 11);
        uint32_t ab = gr_attack_blocks(slot_eval(node + 4 + 16 * 10)), db = dv >= 10000 ? 0u : gr_attack_blocks(dv);   /* 100 % = klingt durch */
        uint32_t l0 = g->lvl, l1 = l0;
        if (g->rel2 || g->stage == 2u) {
            g->stage = 2u;
            uint32_t rb = db ? db : 7u;                      /* Pad losgelassen, Decay 0: 20 ms */
            uint32_t dd = 65536u / rb;
            l1 = l0 > dd ? l0 - dd : 0u;
        } else if (g->stage == 0u) {
            uint32_t da = ab ? 65536u / ab : 65536u;
            l1 = l0 + da >= 65536u ? 65536u : l0 + da;
            if (l1 >= 65536u) g->stage = 1u;
        } else if (db && !g->hold && !c->drone) g->stage = 2u; /* ab jetzt ausklingen; Pad gehalten oder Drone: bleibt */
        g->lvl = l1;
        int32_t dl = ((int32_t)l1 - (int32_t)l0) / 128, lv2 = (int32_t)l0;
        for (uint32_t i = 0; i < 128u; i++) { out[i] = (int16_t)(((int32_t)out[i] * (lv2 >> 1)) >> 15); lv2 += dl; }
        if (g->stage == 2u && l1 == 0u) {                    /* Wolke zu Ende */
            st[0x18] = 0; g->rel2 = 0; g->next = 0;
            for (uint32_t j = 0; j < GR_MAXG; j++) g->act[j] = 0;
        }
    }
    /* Pegel wie Sampler_render: Bit-Depth-Maske, Tabellenindex rampt von +0x52 auf +0x50 (Huellkurve = 1) */
    uint16_t mask = *(uint16_t *)(st + 0x64);
    int32_t gi = *(int16_t *)(st + 0x52), d = (int32_t)*(int16_t *)(st + 0x50) - gi;
    if (d < 0) d += 0x7F;
    d >>= 7;
    for (uint32_t i = 0; i < 128u; i++) {
        int32_t x = (int16_t)((uint16_t)out[i] & mask);
        int32_t gidx = gi < 0 ? 0 : gi > 10000 ? 10000 : gi;
        out[i] = (int16_t)ssat16((gt[gidx] * x) >> 14);
        gi += d;
    }
    *(int16_t *)(st + 0x52) = (int16_t)gi;
    if (st[0x54]) {                                          /* Knackschutz beim Neustart wie im Original */
        int32_t a = *(int16_t *)(st + 0x56);
        if (a) {
            int32_t q = a < 0 ? a + 0x1F : a; q >>= 5;
            for (uint32_t i = 0; i < 64u; i += 2u) {
                out[i] = (int16_t)ssat16(out[i] + a); out[i + 1] = (int16_t)ssat16(out[i + 1] + a);
                a = ssat16(a - q);
            }
        }
    }
    if (g->rel) {                                            /* 024: Sequencer gestoppt → ueber einen Block ausblenden */
        for (uint32_t i = 0; i < 128u; i++) out[i] = (int16_t)(((int32_t)out[i] * (int32_t)(127u - i)) >> 7);
        st[0x18] = 0;                                        /* Stimme aus (wie TrackNode_stop 0x166E4) */
        g->rel = 0; g->next = 0; g->lvl = 0;                  /* naechste Note startet sofort einen Grain */
        for (uint32_t j = 0; j < GR_MAXG; j++) g->act[j] = 0;
        g->n_rel++;
    }
    *(int16_t *)(st + 0x56) = out[127];
    st[0x54] = 0;
    g->blocks++; s->n_gr++;
    TRANSMIT(st, blk, 0u);
    BLOCK_FREE(blk);
}
#endif
