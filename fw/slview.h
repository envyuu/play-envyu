#ifndef SLVIEW_H
#define SLVIEW_H
#include <stdint.h>
/* Patch 026/029 – Slicer in View: Einstellungen je Spur (slview.c) */
typedef struct {
    uint32_t magic;             /* 00 */
    uint16_t gate[8];           /* 04 Gate der Spur, 0 = 100 %, sonst 1…0x7FFF */
    int16_t  frei_[16];         /* 14 bis 030 Attack/Decay, seit 031 in sv_x_t.tv */
    int16_t  sample[8];         /* 34 Sample der Spur + 1 (0 = Sample des Steps) */
    int8_t   off[8];            /* 44 Raster-Versatz in 1/64 Slice-Breite, -63…+63 */
    uint8_t  sel[8];            /* 4C gewaehlter Slice + 1 (0 = keiner) */
    uint8_t  page[8];           /* 54 Seite 0…3 */
    uint8_t  aud[8];            /* 5C Vorhoeren: Slice + 1 fuer den naechsten Trigger */
    uint32_t ui;                /* 64 UI-Objekt (Shift = +0x18) */
    uint32_t dirty;             /* 68 Werte geaendert (speichern) */
    uint32_t last_t;            /* 6C zuletzt eindeutig gewaehlte Spur + 1 */
    uint32_t aud_node;          /* 70 Stimme, die gerade vorhoert */
    uint32_t knob;              /* 74 zuletzt beruehrter/gedrehter Knob + 1 (Fokuszeile) */
    uint32_t foc_until;         /* 78 Fokuszeile bis (ms) */
    uint32_t fw_until;          /* 7C Firmware-Anzeige bis (ms): Master, Tempo, Random, Play Mode haben keine Step-Seite */
    uint32_t cap_until;         /* 80 Fremdparameter (Step-Seite der Firmware) bis (ms) */
    uint8_t  cap_sel, pad_[3];  /* 84 gewaehlter Block 0/1 */
    char     cap0[18], cap1[18];/* 88 "Name Wert" */
    uint32_t x;                 /* AC Heap-Block sv_x_t (031), 0 = noch keiner */
    uint32_t play_px;           /* B0 Play-Marker: Pixel + 1, 0 = keiner (038) */
    uint32_t play_t;            /* B4 letztes Neuzeichnen (ms) */
    uint8_t  kmode[8];          /* B8 045: Pad-Modus je Slicer-Spur (gruene Pads): 0 Slices, 1 Keys */
} sv_t;
_Static_assert(sizeof(sv_t) <= 0xC0, "sv_t passt in 0x1FFFB940-0x1FFFB9FF");
#define SV_CFG_N 22u            /* engines.cfg: g a d o p n w m e h k i c y x r l z u s v j + Spur = Wert (j = 045 Beat-Nudge) */
#define SV_PEAKS 64u
#define SV_PV_N 11u             /* Vorhoeren: Volume, Pan, Cutoff, Reso, Bit Depth, Overdrive, Delay, Reverb, Microtune, Attack, Decay */
/* 031: Grid-Breite, Transponierung, Slice-Modus, Beat-Erkennung je Spur (Heap, erst bei Bedarf) */
typedef struct {
    uint32_t data, frames;      /* analysiertes Sample */
    uint16_t pos[SV_PEAKS];     /* gefundene Anschlaege: Position 0…0x7FFF */
    uint16_t str[SV_PEAKS];     /* Spitze des Anschlags */
    uint16_t tab[SV_PEAKS + 1u];/* Slice-Grenzen fuer die aktuelle Slice-Zahl */
    uint8_t  npk, nt, nfor, done;   /* Anschlaege, Slices in tab, tab gilt fuer Genauigkeit, Analyse fertig */
    uint32_t rate;
    uint16_t thr;               /* aktuelle Schwelle (Spitze) fuer die Anzeige */
} sv_beat_t;
typedef struct {
    int16_t  wid[8];            /* Grid-Breite: Faktor (1024 + wid) / 1024, -512…+1024 */
    uint8_t  smode[8];          /* 0 Grid, 1 Beat */
    uint8_t  fine[8];           /* Sensitivity 1…16 im Modus Beat, 0 = Standard 8 */
    uint8_t  gmode[8];          /* Slice-Zahl (Menuewert) vor dem Wechsel zu Beat */
    uint16_t gen[8];            /* Aenderungszaehler fuer die Anzeige */
    int8_t   nudge[8];          /* 045: Beat-Nudge in ms (-50…+50): alle Beat-Grenzen frueher/spaeter */
    uint32_t wid_t;             /* letzte Raste Grid-Breite (Beschleunigung) */
    int8_t   fitp[8];           /* 045: Rasten am Fit-Punkt (erst die zweite verlaesst Fit) */
    uint8_t  rms[304];          /* 045: RMS je Waveform-Spalte (0…127) fuer die 4-Bit-Waveform (wave.c) */
    uint32_t sc_t, sc_i, sc_prev, sc_last, sc_hop, sc_n, sc_dist;   /* laufende Analyse: Spur (8 = keine), Hop, Energie, letzter Fund+1 */
    int16_t  cut[8][32];        /* 12.11.: Handschnitte – Verschiebung des Anfangs von Slice k (Einheiten 0…0x7FFF des Samples) */
    uint32_t ckey[8];           /* 12.11.: Schnitte gelten nur fuer dieses Raster (Slice-Zahl, Grid/Beat, Sample-Laenge); 0 = keine */
    uint32_t cut_t;             /* 12.11.: letzte Raste (Beschleunigung) */
    sv_beat_t b[8];
    uint8_t  hold[8];           /* 17.11.: gehaltenes Slice-Pad + 1 (0 = keins): nur dann schneidet Sample Start */
} sv_x_t;
void sv_reset(void);
void sv_poll(void);
void sv_set_ui(uint32_t ui);
void sv_grid_fit(uint32_t t);
int32_t sv_cut(uint32_t t, uint32_t k, uint32_t n);      /* 12.11.: Handschnitt (0 = keiner / Raster geaendert) */
int32_t sv_cut_any(uint32_t t, uint32_t n);
uint32_t sv_cut_key(uint32_t t, uint32_t n);
void    sv_cut_set(uint32_t t, uint32_t key, uint32_t k, int32_t v);   /* aus engines.cfg */
uint32_t sv_fit_count(uint32_t t, uint32_t n);
int  sv_shift(void);
void sv_shift_changed(void);
uint32_t sv_cur_track(void);
int  sv_multi_held(void);
int  sv_assign_sample(int slot);
volatile uint32_t *sv_dirty_flag(void);
void sv_clear_track(uint32_t t);
int32_t sv_off(uint32_t t);
int32_t sv_wid(uint32_t t);
int     sv_beat_on(uint32_t t);
uint32_t sv_grid_gen(uint32_t t);
/* Grenzen im Beat-Modus: 1 = Tabelle gilt und a, b sind gesetzt (0x7FFF-Rand), k ausserhalb → *a >= 0x7FFE */
int     sv_beat_bounds(uint32_t t, uint32_t k, uint32_t n, int32_t *a, int32_t *b);
int     sv_beat_ready(uint32_t t);
void    sv_scan_start(sv_x_t *x, uint32_t t, uint32_t data, uint32_t frames, uint32_t rate);
int     sv_scan_run(sv_x_t *x, uint32_t max_hops);      /* 1 = fertig */
void    sv_beat_pick(sv_beat_t *b, uint32_t level);  /* Sensitivity 1 … 16 */
uint32_t sv_beat_thr(uint32_t t);
uint32_t sv_focus_knob(void);
volatile uint32_t *sv_play_state(void);                 /* {px + 1, ms} */
uint32_t sv_beat_level(uint32_t t);
uint32_t sv_beat_count(uint32_t t);
void    sv_beat_toggle(uint32_t t, int on, volatile int32_t *mode);
void    sv_fine_step(uint32_t t, int dir);
sv_x_t *sv_xget(int create);
int32_t sv_sample(uint32_t t);  /* Sample-Index der Spur oder -1 */
int  sv_take_audition(uint32_t t, uint32_t *k);
void sv_apply(uint32_t t, uint32_t idx, int dir);
int32_t sv_step_mode(int32_t mode, int dir);
uint32_t sv_page_track(void);
int sv_audition_pending(uint32_t t);   /* Spur der Slicer-Seite in View oder 8 */
void sv_capture(uint32_t i0, uint32_t i1, const volatile char *n0, const volatile char *v0, const volatile char *n1, const volatile char *v1, uint32_t sel);
void sv_join(char *d, uint32_t max, const volatile char *name, const volatile char *val);
int  sv_own_param(uint32_t idx);
int32_t sv_next_sample(const uint8_t *cls, int32_t cur, int dir);
int32_t sv_cfg_get(uint32_t t, uint32_t i);
void sv_cfg_set(uint32_t t, uint32_t i, int32_t v);
#endif
