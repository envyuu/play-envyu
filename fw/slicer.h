/* Patch 021 – gemeinsame Typen fuer slicer.c / granular.c (RAM 0x1FFF9800–0x1FFF9FFF) */
#ifndef SLICER_H
#define SLICER_H
#include <stdint.h>

#define SL_MAGIC   0x511CE021u
#define SL_NMODES  12
#define SL_GRAN    9            /* 9 Granular, 10 Granular Rev, 11 Granular P-P */
/* 045: Release ohne Granular. Menue, engines.cfg und Pruefung kennen nur Normal + Slicer (0…8); Granular-Code bleibt tot. */
/* 045: groesste Slicer-Stufe im Menue (6 = Slicer 32, 8 = Slicer 64). Release: 32, darueber haette man drei Seiten. */
#ifndef SL_MAXSLMODE
#define SL_MAXSLMODE 6
#endif
#ifdef WITH_GRANULAR
#define SL_NMENU   SL_NMODES
#else
#define SL_NMENU   (SL_MAXSLMODE + 1)
#endif
/* Slices der groessten Stufe (4, 8, 12, 16, 24, 32, 48, 64) */
#define SL_MAXSLICES (SL_MAXSLMODE >= 8 ? 64u : SL_MAXSLMODE == 7 ? 48u : SL_MAXSLMODE == 6 ? 32u : SL_MAXSLMODE == 5 ? 24u : 16u)
/* 045: Feinversatz des Slice-Anfangs je Step in den unteren 6 Bit von Sample Start (Slice = Wert >> 6).
 * 0, 32 und 63 = kein Versatz (bisher geschrieben: k*64+32, Vielfache von 64, Knob am Anschlag 0x7FFF),
 * 1…31 = +1…+31, 33…62 = −31…−2 (in 1/64 Slice); −1 gibt es nicht. */
static inline int32_t sl_fine(int32_t v) { uint32_t l = (uint32_t)v & 63u; return (l == 0u || l == 32u || l == 63u) ? 0 : l < 32u ? (int32_t)l : (int32_t)l - 64; }
static inline int32_t sl_fine_enc(int32_t o) { return o == 0 ? 32 : o > 0 ? o : 64 + o; }
/* Wert aus Datei/RAM auf das Menue abbilden: 48/64 → groesste Stufe, Unbekanntes → Normal */
#define SL_CLAMP_MODE(v) (((v) > SL_MAXSLMODE && (v) < SL_GRAN) ? SL_MAXSLMODE : ((v) < 0 || (v) >= SL_NMENU) ? 0 : (v))

typedef struct {
    uint32_t magic;             /* 000 */
    int32_t  mode[8];           /* 004 Menuewerte je Spur (EnumMenuItem schreibt int32) */
    int32_t  saved[8];          /* 024 Stand der Datei */
    int32_t  seen[8];           /* 044 zuletzt gesehen (Speicherverzoegerung) */
    uint32_t loaded, t_load, t_change, load_res;   /* 064 */
    uint32_t saves, save_err, save_fail, fmt_ok;   /* 074 */
    uint32_t fmt10[2], fmt11[2];                   /* 084 eigene Formatter-Objekte {vptr, Wort 1} */
    uint32_t vt[4];                                /* 094 deren vtable */
    uint32_t orig10, orig11;                       /* 0A4 Original-Formatter-Objekte */
    uint32_t node[8];                              /* 0AC Spur-Knoten (ctx[0]) je Spur, gemerkt in sl_begin */
    int32_t  v10[8];                               /* 0CC Sample Start des laufenden Steps */
    uint8_t  have10[8];                            /* 0EC 1 = v10 gilt fuer den laufenden Step */
    uint32_t n_begin, n_slice, n_gr;               /* 0F4 Zaehler (Diagnose per PEEK) */
} sl_t;                                            /* 0x100 */
_Static_assert(sizeof(sl_t) == 0x100, "sl_t");

/* 039: Einstellungen je Spur (RAM 0x1FFF9900, 8 × 0x20) */
#define GR_MAXG 8u
typedef struct {
    uint8_t  n;                 /* Grains 1…8 */
    uint8_t  win;               /* Window 0…100 */
    uint8_t  spray;             /* Spray 0…100 (bis +/-500 ms) */
    uint8_t  pspray;            /* Pitch Spray 0…24 Halbtoene */
    int8_t   scan;              /* Scan -100…+100 % der Abspielgeschwindigkeit, 0 = Freeze */
    uint8_t  lrate;             /* LFO Rate: Index 0…31 */
    uint8_t  ldepth;            /* LFO Depth 0…100 */
    uint8_t  ltgt;              /* LFO Ziel: 0 Position, 1 Spray, 2 Size, 3 Pitch */
    uint8_t  pmode;             /* 040: 0 Richtung aus dem Menue, 1 Random, 2 Scan Bounce, 3 Random Walk */
    uint8_t  drone;             /* 040: Pad rastet ein (nicht gespeichert) */
    uint8_t  speed;             /* 044: Grain Speed 5…200 %, Lesegeschwindigkeit (Tonhoehe folgt) */
    uint8_t  rate;              /* 044: Grain Rate 10…100 = Abstand der Grains 100…1000 % */
    uint8_t  pad_[20];
} gr_cfg_t;
_Static_assert(sizeof(gr_cfg_t) == 0x20, "gr_cfg_t");

/* Granular-Zustand je Spur (Heap, einmal beim Start) */
typedef struct {
    int32_t  ipos[GR_MAXG];     /* Leseposition (Frames) je Grain */
    uint32_t frac[GR_MAXG];     /* Nachkomma Q16 */
    uint32_t age[GR_MAXG];      /* Ausgabe-Samples seit Grain-Start */
    uint32_t gl[GR_MAXG], gf[GR_MAXG];   /* Laenge/Ueberblendung des Grains (beim Start festgelegt) */
    uint32_t ginc[GR_MAXG];     /* Schrittweite Q16 des Grains */
    int8_t   dir[GR_MAXG];      /* +1 vorwaerts, -1 rueckwaerts */
    uint8_t  act[GR_MAXG];      /* Grain laeuft */
    uint8_t  slot, pp, live, rel;  /* naechster Grain-Platz, Ping-Pong-Umschalter, 1 = initialisiert, 1 = Stop: ausblenden */
    uint8_t  hold, rel2, stage, sdir;   /* Pad gehalten, Pad losgelassen, Huelle: 0 Attack, 1 Halten, 2 Decay; Scan Bounce: 1 = rueckwaerts */
    uint32_t next;              /* Samples bis zum naechsten Grain */
    uint32_t rnd;               /* xorshift32 */
    uint32_t blocks;            /* Diagnose */
    uint32_t n_rel;             /* Ausblendungen nach Stop (Diagnose) */
    uint32_t lvl;               /* Huelle Q16 */
    uint32_t scan;              /* Versatz durch Scan (Frames) */
    uint32_t lphase;            /* LFO-Phase */
    uint32_t cshow, sshow;      /* Anzeige: Position, Spray (Frames) */
} gr_t;

typedef struct {
    const int16_t *data;        /* Mono-Sampledaten (Stimme +0x1C) */
    uint32_t len;               /* Frames (Stimme +0x24) */
    uint32_t inc;               /* Schrittweite Q16 (aus Stimme +0x3C float) */
    uint32_t center;            /* Position (Frames) */
    uint32_t glen;              /* Grain-Laenge in Ausgabe-Samples */
    uint32_t fade;              /* Ein-/Ausblendung in Samples (<= glen/2) */
    uint32_t spray;             /* max. Versatz in Frames (+/-) */
    int32_t  dirmode;           /* 0 vorwaerts, 1 rueckwaerts, 2 Ping-Pong */
    uint32_t n;                 /* Grains */
    uint32_t pspray;            /* Pitch Spray in Halbtoenen */
    int32_t  gain;              /* Q15, gleicht die Ueberlappung aus */
    uint32_t rate;              /* 044: Abstand der Grains in % (100 = dicht) */
} gr_par_t;

/* slicer.c */
uint32_t sl_slices(int32_t m);
#if defined(WITH_GRANULAR) || defined(HOST_TEST)
int      sl_is_granular(int32_t m);
#else
/* 045: Granular ausgeblendet → zur Uebersetzungszeit 0, der Compiler entfernt die Granular-Zweige, --gc-sections den Code */
static inline int sl_is_granular(int32_t m) { (void)m; return 0; }
#endif
uint32_t sl_slice_index(int32_t v, uint32_t n);
void     sl_slice_bounds(uint32_t k, uint32_t n, int32_t gate, int32_t *start, int32_t *end);
void     sl_bounds(uint32_t t, uint32_t k, uint32_t n, int32_t gate, int32_t *start, int32_t *end);   /* 029: mit Raster-Versatz der Spur; 031: Grid-Breite, Beat */
int      sl_slice_outside(uint32_t t, uint32_t k, uint32_t n);
int     *sl_track_ctx(uint32_t t);                                  /* 035: Wiedergabe-Kontext aus dem Spurobjekt oder 0 */
void     sl_preview_params(uint32_t t);                             /* 032: Standard-Step + Spur fuers Vorhoeren */
uint32_t sl_step_track(uintptr_t step);
int      sl_format(uint32_t idx, int32_t value, int32_t rel, char *buf, uint32_t size, uintptr_t step);
void     sl_reset(void);
void     sl_init(void);
void     sl_begin(int *step, int *ctx, uint8_t *ev);
uint32_t sl_param_hook(int *step, uint32_t idx, int *ctx, uint8_t *ev);
void     sl_build(void *menu, void *parent);
void     sl_menu_add(void *parent);
void     sl_poll(void);
const char *sl_mode_name(int32_t m);
void sl_kind_apply(void);

/* granular.c */
uint32_t gr_grain_ms(int32_t v);
void     gr_params(gr_par_t *p, int32_t v_pos, int32_t v_len, int32_t v_shape, int32_t v_spray, int32_t dirmode);   /* p->len, p->inc vorher setzen */
void     gr_reset(gr_t *g, uint32_t seed);
void     gr_clear(void);
gr_t    *gr_state(uint32_t t);
gr_cfg_t *gr_cfg(uint32_t t);
void     gr_cfg_default(uint32_t t);
void     gr_set_count(gr_par_t *p, uint32_t n);
int      gr_lfo_tri(uint32_t phase);
void     gr_set_playing(int playing);
uint32_t gr_lfo_mhz(uint32_t idx);
uint32_t gr_attack_blocks(int32_t v);
/* 024: Engines pro Projekt, Stopp */
void     sl_load_dir(const char *dir, int ws);
void     sl_save_project(const char *dir, const char *name);
void     sl_new_project(void);
void     sl_transport(int playing);
void     gr_render(gr_t *g, const gr_par_t *p, int16_t *out, uint32_t n);
uint32_t gr_f2q16(uint32_t fbits);
void     gr_update(uint8_t *stream);
void     sl_fade_fix(uint8_t *stream);
void     sl_stop_fade(uint8_t *stream);
#endif
