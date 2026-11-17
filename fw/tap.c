/* Patch 045 – Tap Tempo im Menue. Polyend Play Rev A, Firmware 1.6.0.
 *
 * Bedienung (Wunsch des Nutzers 09.11.2026):
 *   Menue offen (Encoder-Klick) → rechts in der Kopfzeile steht das Tempo, z. B. "120 BPM".
 *   Tempo-Knob (Knob 5) beruehren:
 *     1. Beruehrung: nichts aendert sich, die Kopfzeile springt auf "TAP 120" (bereit).
 *     jede weitere: Ausgleichsgerade ueber bis zu 16 Abstaende (045), gesetzt ab dem 6. Tap (12.11.), danach mit Hysterese
 *     0,7 BPM; Ausreisser > 30 % = neue Serie.
 *     2,5 s ohne Beruehrung: zurueck auf "124 BPM".
 *   Tempo-Knob drehen: +-1 BPM je Raste, Kopfzeile folgt.
 *   Uebrige Knobs schliessen das Menue nicht mehr (Beruehren und Drehen werden im Menue ignoriert). Zurueck geht ueber "Back".
 *
 * Firmware (Rev A):
 *   Menue-Bildschirm vtable 0xA97E0: +0x0C Zeichnen 0x44639 (setzt Kopfzeile rechts auf ""), +0x1C/+0x20 Knob drehen/
 *     beruehren geben 0 zurueck → Ereignis faellt zum Sequencer durch, der die Parameterseite zeigt und das Menue verdraengt.
 *   Aktiver Bildschirm: *(*(ui+8)+0x2FC). Kopfzeile: *(Bildschirm+0x10), rechter Text per FUN_000409e4(kopf, text).
 *   SeqCtl = *(ui+0xC) (0x1FFF44E0). Tempo float bei *(SeqCtl+0x1C)+0x1C8, gesetzt ueber FUN_00021030(s0 = BPM, r0 = SeqCtl)
 *     (Hard-Float), danach Projekt geaendert: Byte *(SeqCtl+0x1C)+0x1C4 = 1 (wie Knob-Funktion FUN_000210e8).
 *   Bereich wie die Firmware: 10…400 BPM.
 * Hooks: Wort 0xA97EC (Menue vt+0x0C) → tt_menu_draw. Knob-Ereignisse kommen aus sv_touch_hook/sv_knob_hook (slview.c).
 * RAM: tt_t bei 0x1FFFB600 (frei zwischen Namenspuffern 0x1FFFB400–0x1FFFB5FF und NIS 0x1FFFB900).
 */
#include <stdint.h>
#include "tap.h"

#define TT_MAGIC   0x7A9C0451u
#define KNOB_TEMPO 5u
#define TAP_MS     2500u          /* so lange bleibt "TAP" stehen */
#define MIN_IV     150u           /* schneller als 400 BPM: Prellen, ignorieren */
#define MAX_IV     6000u          /* langsamer als 10 BPM: neu anfangen */
#define MENU_VT    0x000A97E0u

#ifdef HOST_TEST
__attribute__((weak)) uint8_t host_tt_ram[0x80];
#define TT          ((volatile tt_t *)(void *)host_tt_ram)
__attribute__((weak)) uint32_t host_millis_tt;
#define MILLIS()    (host_millis_tt)
uint32_t host_bpm_bits;
uint32_t host_hdr_calls; const char *host_hdr_txt;
uint32_t host_set_calls; int32_t host_set_bpm;
uint32_t host_orig_draw;
static uint32_t rd_bpm_x10(uint32_t seq) { (void)seq; return host_bpm_bits; }   /* Test: direkt BPM x 10 */
static void set_bpm(uint32_t seq, int32_t bpm) { (void)seq; host_set_calls++; host_set_bpm = bpm; host_bpm_bits = (uint32_t)bpm * 10u; }
static void hdr_right(uint32_t hdr, const char *t) { (void)hdr; host_hdr_calls++; host_hdr_txt = t; }
static void orig_draw(uint32_t scr) { (void)scr; host_orig_draw++; }
#undef TT_SEQ_DEFAULT
#define TT_SEQ_DEFAULT 0x1000u
#define R32(a)      (host_r32(a))
__attribute__((weak)) uint32_t host_r32(uint32_t a) { (void)a; return 0; }
#else
#define TT          ((volatile tt_t *)0x1FFFB600u)
#define MILLIS()    (*(volatile uint32_t *)0x20000D7Cu)
#define R32(a)      (*(volatile uint32_t *)(uintptr_t)(a))
uint32_t tt_f2x10(uint32_t fbits);            /* asm unten: (int)(f * 10 + 0,5) */
void     tt_set_tempo(uint32_t seq, int32_t bpm);   /* asm unten: FUN_00021030(s0 = (float)bpm, r0 = seq) */
static uint32_t rd_bpm_x10(uint32_t seq) {
    uint32_t st = R32(seq + 0x1Cu);
    if (st < 0x1FFF0000u || st >= 0x20030000u) return 0;
    return tt_f2x10(R32(st + 0x1C8u));
}
static void set_bpm(uint32_t seq, int32_t bpm) {
    uint32_t st = R32(seq + 0x1Cu);
    if (st < 0x1FFF0000u || st >= 0x20030000u) return;
    tt_set_tempo(seq, bpm);
    *(volatile uint8_t *)(uintptr_t)(st + 0x1C4u) = 1;      /* Projekt geaendert */
}
static void hdr_right(uint32_t hdr, const char *t) { ((void (*)(uint32_t, const char *))0x000409E5u)(hdr, t); }
static void orig_draw(uint32_t scr) { ((void (*)(uint32_t))0x00044639u)(scr); }

__asm__(
    ".syntax unified\n.thumb\n.fpu fpv4-sp-d16\n"
    ".section .text.tt_asm,\"ax\",%progbits\n"
    ".global tt_f2x10\n.type tt_f2x10,%function\n.thumb_func\n"
    "tt_f2x10:\n"
    "  vmov s0, r0\n"
    "  vmov.f32 s1, #10.0\n"
    "  vmul.f32 s0, s0, s1\n"
    "  vcvtr.s32.f32 s0, s0\n"
    "  vmov r0, s0\n"
    "  bx lr\n"
    ".global tt_set_tempo\n.type tt_set_tempo,%function\n.thumb_func\n"
    "tt_set_tempo:\n"
    "  vmov s0, r1\n"
    "  vcvt.f32.s32 s0, s0\n"
    "  ldr r2, =0x00021031\n"
    "  bx r2\n"                                  /* Endaufruf: kehrt direkt zu unserem Aufrufer zurueck */
    ".ltorg\n"
    ".text\n");
#endif

#ifndef HOST_TEST
void tw_set(int on, uint32_t x10, uint32_t armed);   /* tapw.c: eigene Tempo-Anzeige (feste Ziffernfelder, Punkt beim Tippen) */
void tw_reset(void);
void tw_keep(void);
#endif
uint32_t sv_ui(void) __attribute__((weak));   /* slview.c: UI-Objekt aus dem Tasten-Hook */
void tt_reset(void) {
    volatile uint32_t *z = (volatile uint32_t *)(void *)TT;
    for (uint32_t i = 0; i < sizeof(tt_t) / 4u; i++) z[i] = 0;
    TT->seq = TT_SEQ_DEFAULT;
    TT->magic = TT_MAGIC;
#ifndef HOST_TEST
    tw_reset();
#endif
}

/* ganze BPM, eine Nachkommastelle nur wenn noetig: "120", "120.5" */
static uint32_t put_bpm(char *b, uint32_t n, uint32_t x10) {
    uint32_t w = x10 / 10u, f = x10 % 10u;
    char t[6]; uint32_t k = 0;
    do { t[k++] = (char)('0' + w % 10u); w /= 10u; } while (w && k < 5u);
    while (k) b[n++] = t[--k];
    if (f) { b[n++] = '.'; b[n++] = (char)('0' + f); }
    return n;
}

static int menu_open(uint32_t ui, uint32_t *scr_out) {
    if (ui < 0x1FFF0000u || ui >= 0x20030000u) return 0;
    uint32_t mgr = R32(ui + 8u);
    if (mgr < 0x1FFF0000u || mgr >= 0x20030000u) return 0;
    uint32_t scr = R32(mgr + 0x2FCu);
    if (scr < 0x1FFF0000u || scr >= 0x20030000u || R32(scr) != MENU_VT) return 0;
    if (scr_out) *scr_out = scr;
    return 1;
}

/* Text bauen und in die Kopfzeile des Menues schreiben (zwei Puffer im Wechsel, falls das Widget nur Zeiger vergleicht) */
static void show(uint32_t scr) {
    volatile tt_t *s = TT;
    uint32_t x10 = rd_bpm_x10(s->seq);
    if (x10 < 100u || x10 > 4000u) return;             /* kein gueltiges Tempo (Zeiger noch nicht gesetzt): nichts anzeigen */
    char *b = (char *)(uintptr_t)(s->flip ? s->txt1 : s->txt0);
    s->flip ^= 1u;
    uint32_t n = 0;
    if (s->armed) { b[n++] = 'T'; b[n++] = 'A'; b[n++] = 'P'; b[n++] = ' '; n = put_bpm(b, n, x10); }
    else { n = put_bpm(b, n, x10); b[n++] = ' '; b[n++] = 'B'; b[n++] = 'P'; b[n++] = 'M'; }
    b[n] = 0;
    s->shown_x10 = x10; s->shown_armed = s->armed;
    uint32_t hdr = R32(scr + 0x10u);
#ifdef HOST_TEST
    if (hdr >= 0x1FFF0000u && hdr < 0x20030000u) hdr_right(hdr, b);
#else
    (void)b;                                             /* 045: Text der Firmware leer, Zahl + Punkt zeichnet tapw.c */
    if (hdr >= 0x1FFF0000u && hdr < 0x20030000u) hdr_right(hdr, "");
    tw_set(1, x10, s->armed);
#endif
}

/* Menue vt+0x0C (Wort 0xA97EC): Original zeichnet die Seite, danach Tempo rechts in die Kopfzeile */
__attribute__((used, section(".text.entry_ttdraw")))
void tt_menu_draw(uint32_t scr) {
    orig_draw(scr);
    if (TT->magic != TT_MAGIC || !TT->seq) return;
    show(scr);
}

/* Knob beruehrt (aus sv_touch_hook). 1 = verbraucht (Original nicht aufrufen). */
int tt_touch(uint32_t ui, uint32_t knob, int ev) {
    volatile tt_t *s = TT;
    if (s->magic != TT_MAGIC || knob >= 32u) return 0;
    uint32_t bit = 1u << knob, scr = 0;
    if (!menu_open(ui, &scr)) {
        if (ev != 0 && (s->held & bit)) { s->held &= ~bit; return 1; }   /* Druck im Menue verbraucht → Loslassen auch */
        return 0;
    }
    s->seq = R32(ui + 0xCu); s->ui = ui;
    if (ev != 0) { s->held &= ~bit; return 1; }
    s->held |= bit;
    if (knob != KNOB_TEMPO) return 1;                  /* andere Knobs schliessen das Menue nicht mehr */
    uint32_t now = MILLIS();
    if (!s->armed || now - s->t_last > MAX_IV) {       /* erste Beruehrung: nur bereit machen */
        s->armed = 1; s->n = 0; s->iv[0] = 0; s->t_last = now; s->t_show = now;
        show(scr);
        return 1;
    }
    uint32_t iv = now - s->t_last;
    if (iv < MIN_IV) return 1;
    s->t_last = now; s->t_show = now;
    s->log[s->ntot & 15u] = (uint16_t)iv; s->ntot++;
    /* 045: Serie = die letzten n Abstaende im Ring. Weicht ein Abstand > 30 % vom Mittel der Serie ab, beginnt eine neue. */
    if (s->n) {
        uint32_t k0 = s->n < 15u ? s->n : 15u, sum0 = 0;
        for (uint32_t i = 0; i < k0; i++) sum0 += s->log[(s->ntot - 2u - i) & 15u];
        uint32_t avg = sum0 / k0;
        if (iv * 10u > avg * 13u || iv * 10u < avg * 7u) { s->n = 0; s->iv[0] = 0; }
    }
    if (s->n < 16u) s->n++;
    uint32_t k = s->n;
    /* Ausgleichsgerade durch die k+1 Taps: t_j (ms ab dem ersten Tap der Serie) ueber j = 0..k; Steigung = ms je Schlag */
    uint32_t t = 0, st = 0, sjt = 0, sj = 0, sjj = 0;
    for (uint32_t j = 1; j <= k; j++) {
        t += s->log[(s->ntot - 1u - k + j) & 15u];
        st += t; sjt += j * t; sj += j; sjj += j * j;
    }
    uint32_t np = k + 1u;
    int32_t num = (int32_t)(np * sjt) - (int32_t)(sj * st), den = (int32_t)(np * sjj) - (int32_t)(sj * sj);
    if (num <= 0 || den <= 0) return 1;
    uint32_t x10 = (600000u * (uint32_t)den + (uint32_t)num / 2u) / (uint32_t)num;   /* 12.11.: Zehntel-BPM */
    if (x10 < 100u) x10 = 100u;
    if (x10 > 4000u) x10 = 4000u;
    /* 12.11. (Messung am Geraet: Tipp-Streuung ±24 ms → nach 4 Taps ±3–6 BPM): erst ab dem 6. Tap setzen, danach nur,
     * wenn die Schaetzung >= 0,7 BPM vom zuletzt gesetzten Wert der Serie abweicht (kein Flackern 120 ↔ 121) */
    if (k < 5u) { s->shown_x10 = 0; show(scr); return 1; }
    uint32_t last = s->iv[0];
    if (last && (x10 > last ? x10 - last : last - x10) < 7u) { show(scr); return 1; }
    s->iv[0] = x10;
    int32_t bpm = (int32_t)((x10 + 5u) / 10u);
    set_bpm(s->seq, bpm);
    show(scr);
    return 1;
}

/* Knob gedreht (aus sv_knob_hook). 1 = verbraucht. */
int tt_turn(uint32_t ui, uint32_t knob, int delta) {
    volatile tt_t *s = TT;
    uint32_t scr = 0;
    if (s->magic != TT_MAGIC || !menu_open(ui, &scr)) return 0;
    s->seq = R32(ui + 0xCu); s->ui = ui;
    if (knob != KNOB_TEMPO || !delta) return 1;
    int32_t bpm = (int32_t)((rd_bpm_x10(s->seq) + 5u) / 10u) + delta;
    if (bpm < 10) bpm = 10;
    if (bpm > 400) bpm = 400;
    set_bpm(s->seq, bpm);
    s->armed = 0;
    show(scr);
    return 1;
}

/* Hauptschleife: "TAP" nach 2,5 s zuruecknehmen, Tempo von aussen (MIDI-Clock, Projekt) nachziehen */
void tt_poll(void) {
    volatile tt_t *s = TT;
    uint32_t scr = 0;
    if (s->magic == TT_MAGIC && !s->ui && sv_ui) s->ui = sv_ui();   /* 045: vor dem ersten Knob-Ereignis kennt tap.c das UI-Objekt nicht */
    if (s->magic != TT_MAGIC || !s->seq || !menu_open(s->ui, &scr)) {
        if (s->magic == TT_MAGIC) s->armed = 0;
#ifndef HOST_TEST
        tw_set(0, 0, 0);                                 /* 045: Menue zu → Tempo-Anzeige weg */
#endif
        return;
    }
#ifndef HOST_TEST
    tw_keep();
#endif
    if (s->armed && MILLIS() - s->t_show > TAP_MS) s->armed = 0;
    if (s->armed != s->shown_armed || rd_bpm_x10(s->seq) != s->shown_x10) show(scr);
}
