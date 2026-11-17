/* Patch 023 – Waveform-Ansicht: Zustand im RAM 0x1FFF9B00 (bis 0x1FFF9FFF) */
#ifndef WAVE_H
#define WAVE_H
#include <stdint.h>

#define WV_MAGIC  0x57A7E023u
#define WV_X      8u          /* Box der Waveform (logische Koordinaten 320×240) */
#define WV_Y      64u
#define WV_W      304u
#define WV_H      120u
#define WV_STRIDE (WV_W / 2u) /* 045: 4-Bit-Bitmap (L4, 16 Stufen): 152 Byte je Zeile, 18 240 Byte im RAM_G */
#define WV_TY     27u         /* obere Textzeile (Formatter-Texte) */
#define WV_IY     188u        /* Infozeile (Engine, Laengen) */

typedef struct {
    uint32_t magic;                         /* 000 */
    uint32_t seq;                           /* 004 Aufrufe von wv_view_hook */
    uint8_t  on, upl, rec_ok, rec_have;     /* 008 Ansicht an, Bitmap im RAM_G, Formatter-Notiz gueltig, Bits 10/11 */
    uint32_t todo;                          /* 00C wv_poll hat noch Arbeit */
    uint32_t slot;                          /* 010 eigener Platz im Widget-Manager (0…49, sonst 0xFFFFFFFF) */
    uint32_t view, mgr;                     /* 014 Seite (Klasse 0xA0888), Manager */
    uint32_t rec_t, rec_step;               /* 01C Notiz aus dem Formatter: Spur, Step */
    int32_t  rec_v10, rec_v11;              /* 024 angezeigte Werte */
    int32_t  mode; uint32_t track;          /* 02C */
    int32_t  v10, v11, v13, v14;            /* 034 Start, Ende, Attack, Decay */
    uint32_t data, frames, rate;            /* 044 Sample (SDRAM) */
    uint32_t c_data, c_frames, col, row;    /* 050 Cache-Schluessel, naechste Spalte / Bitmap-Zeile */
    uint16_t hl0, hl1, br0, br1, mk, sp0, sp1;   /* 060 Pixel relativ: Hintergrund hell, Waveform hell, Marke, Spray */
    uint8_t  kidx, nsl;                     /* 06E aktueller Slice, Anzahl */
    uint32_t vobj[6];                       /* 070 Widget-Objekte der Seite beim Einschalten */
    uint32_t obj[16];                       /* 088 eigenes Widget (+0 vptr, +4 Liste da, +8 Laenge, +0xC Flags) */
    uint32_t vt[10];                        /* 0C8 dessen vtable */
    char     t0[24], t1[24], i0[24], i1[24];/* 0F0 Texte */
    int8_t   mn[WV_W], mx[WV_W];            /* 150 Min/Max je Spalte (>>8) */
} wv_t;                                     /* 0x3B0 */
_Static_assert(sizeof(wv_t) == 0x3B0, "wv_t");
_Static_assert(sizeof(wv_t) <= 0x500, "RAM 0x1FFF9B00-0x1FFF9FFF");

void     wv_column_range(uint32_t frames, uint32_t c, uint32_t *s0, uint32_t *s1);
void     wv_column(const int16_t *d, uint32_t frames, uint32_t c, int8_t *mn, int8_t *mx);
uint8_t  wv_column_rms(const int16_t *d, uint32_t frames, uint32_t c);
void     wv_row(const volatile int8_t *mn, const volatile int8_t *mx, const volatile uint8_t *rms, uint32_t y, uint8_t *out);
uint32_t wv_x(int32_t u);
uint32_t wv_f2u(uint32_t b);
void     wv_layout(volatile wv_t *w);
void     wv_reset(void);
void     wv_poll(void);
void     wv_note_fmt(uint32_t idx, int32_t value, int32_t rel, uintptr_t step);
void     wv_kb(int on, uint32_t t, int32_t k, int32_t gate, const char *top_right, const char *info_left, const char *info_right, uint32_t foc);
int32_t  wv_touched_slice(uint32_t n);             /* 030: Slice des zuletzt beruehrten Steps (Standard-Step) */
int32_t  wv_touched_gate(void);
void     wv_cur_name(char *d, uint32_t max);         /* 041: Name des angezeigten Samples */
int32_t  wv_touched_note(void);
int32_t  wv_touched_param(uint32_t idx);
uint32_t wv_bpm10(uint32_t t, uint32_t n);          /* 031: BPM × 10, ein Slice = 1/16, normiert auf 70…180 */
int      wv_sample_data(uint32_t t, uint32_t *data, uint32_t *frames, uint32_t *rate);
uint32_t wv_off_ms(uint32_t t, uint32_t n);        /* 030: Betrag des Raster-Versatzes in ms */
int      wv_sample_classes(uint8_t *cls);          /* 029: cls[255] 0 leer, 1 Sample, 2 Live Recordings; Rueckgabe 1 = Aufnahmen da */
int32_t  wv_track_sample(uint32_t t);              /* 029: Sample des ersten Steps der Spur */
void     wv_view_hook(void *view, void *names, void *vals, void *prog, void *max, void *flags, void *styles);
#endif
