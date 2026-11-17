/* Patch 045 – Tap Tempo im Menue (tap.c) */
#ifndef TAP_H
#define TAP_H
#include <stdint.h>

typedef struct {
    uint32_t magic;             /* 00 */
    uint32_t seq;               /* 04 SeqCtl (0x1FFF44E0), aus ui+0xC */
    uint32_t ui;                /* 08 UI-Zuhoerer, aus den Knob-Hooks */
    uint32_t armed;             /* 0C 1 = "TAP" (erste Beruehrung war da) */
    uint32_t t_last, t_show;    /* 10 letzte Beruehrung, letzte Anzeige */
    uint32_t n;                 /* 18 Zahl der Abstaende */
    uint32_t iv[4];             /* 1C [0] 12.11.: zuletzt gesetztes Tempo der Serie (Zehntel-BPM, 0 = keins), Rest frei */
    uint32_t held;              /* 2C Knobs, deren Druck im Menue verbraucht wurde */
    uint32_t shown_x10, shown_armed, flip;   /* 30 */
    char     txt0[12], txt1[12];             /* 3C Kopfzeilentext im Wechsel */
    uint32_t ntot;                           /* 54 045: alle Abstaende seit Start (Index in log) */
    uint16_t log[16];                        /* 58 045: letzte 16 Roh-Abstaende (ms), Ring, per PEEK lesbar */
} tt_t;                         /* 0x78 */
_Static_assert(sizeof(tt_t) <= 0x80, "tt_t");
#ifndef HOST_TEST
_Static_assert(0x1FFFB600u + sizeof(tt_t) <= 0x1FFFB900u, "tt_t vor NIS 0x1FFFB900");
#endif

#define TT_SEQ_DEFAULT 0x1FFF44E0u   /* SeqCtl: Auswahl 0x1FFF450C = +0x2C, Perform-Seite 0x1FFF69B8 = +0x24D8 */

void tt_reset(void);
void hd_reset(void);   /* hdr.c */
void tt_menu_draw(uint32_t scr);
int  tt_touch(uint32_t ui, uint32_t knob, int ev);
int  tt_turn(uint32_t ui, uint32_t knob, int delta);
void tt_poll(void);
#endif
