/* Patch 023 – Live Rec lang = USB-Aufnahme (Rueckruf in usbrec.c aus Patch 024), Statusanzeige; RAM 0x1FFF9EB0 */
#ifndef LREC_H
#define LREC_H
#include <stdint.h>

#define LR_MAGIC    0x11BEC023u
#define LR_BTN      7u          /* Taste "Live Rec" (SeqEdit_action case 7; Shift + Live Rec = Undo) */
#define LR_LONG_MS  1000u
#define LR_PILL_W   140u
#define LR_FWD      1u          /* Ereignis unveraendert weitergeben */
#define LR_DEFERRED 2u          /* zurueckgehaltenes Druecken + Loslassen weitergeben (kurzer Druck) */
#define LR_TOGGLE   4u          /* lang gehalten: rec_request */

typedef struct {
    uint32_t magic;             /* 00 */
    uint8_t  down, fired, shown, _r;   /* 04 */
    uint32_t t;                 /* 08 Zeit des Drueckens (millis) */
    uint32_t n_long, n_short, n_req;   /* 0C Zaehler */
    int32_t  req_res;           /* 18 letzte Rueckgabe von rec_request */
    uint32_t key;               /* 1C zuletzt angezeigter Stand */
    uint32_t slot;              /* 20 eigener Manager-Platz */
    uint32_t obj[16];           /* 24 Widget-Objekt */
    uint32_t vt[10];            /* 64 */
    char     txt[24];           /* 8C */
} lr_t;                         /* 0xA4 */
_Static_assert(sizeof(lr_t) == 0xA4, "lr_t");
_Static_assert(0x1FFF9EB0u + sizeof(lr_t) <= 0x1FFFA000u, "RAM");

uint32_t lr_step(volatile lr_t *s, uint32_t btn, uint32_t ev, uint32_t shift, uint32_t now, int enabled);
void     lr_text(volatile char *b, uint32_t st, uint32_t ds, uint32_t drops);
void     lr_reset(void);
void     lr_poll(void);
uint32_t lr_button_hook(void *ui, uint32_t btn, uint32_t ev);
#endif
