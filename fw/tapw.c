/* Patch 045 – Tempo-Anzeige im Menue (Kopfzeile rechts) und feste Ziffernfelder. Polyend Play Rev A, Firmware 1.6.0.
 *
 * Wunsch Nutzer 10.11.2026: Die Zahl steht dauerhaft ("120 BPM", 3 Stellen reserviert, BPM dahinter). Beim Tippen erscheint
 * links ein kleiner Punkt, der nicht an der Zahl haengt. Der Font ist proportional ('1' = 6 px, '0' = 10 px), darum zeichnet
 * ein eigenes Widget jede Ziffer zentriert in einem festen Feld (tw_cells). Den Text der Firmware rechts leert tap.c.
 * Widget wie lrec.c: eigener Platz im Widget-Manager 0x1FFF42CC, vt[0] = Zeichnen, vt[1]/[2] = Firmware 0x877D/0x8751.
 * RAM: tw_t bei 0x1FFFB870 (bis 0x1FFFB8FF, vor NIS 0x1FFFB900).
 */
#include <stdint.h>

void tw_cells(uint32_t x, uint32_t y, uint32_t font, const char *s, uint32_t cw);

#ifndef HOST_TEST
#define F(a, T) ((T)(a))
#define CO_BEGIN()      F(0x00000809u, void (*)(void))()
#define CO_END()        F(0x00000829u, void (*)(void))()
#define CMD_DLSTART()   F(0x00000DD5u, void (*)(void))()
#define CMD_TEXT(x, y, f, o, s) F(0x00000C71u, void (*)(uint32_t, int32_t, uint32_t, int32_t, const char *))((x), (y), (f), (o), (s))
#define DL_COLOR(c)     F(0x00000971u, void (*)(uint32_t))(c)
#define DL_V2F(x, y)    F(0x00000991u, void (*)(uint32_t, uint32_t))((x), (y))
#define DL_BEGIN(p)     F(0x00000B9Du, void (*)(uint32_t))(p)
#define DL_END()        F(0x00000BBDu, void (*)(void))()
#define DL_LINEW(w)     F(0x00000B25u, void (*)(uint32_t))(w)
#define DL_POINTSZ(w)   F(0x00000B05u, void (*)(uint32_t))(w)
#define MGR             0x1FFF42CCu
#define RAM_OK(p)       ((p) >= 0x1FFF0000u && (p) < 0x2003FF00u && ((p) & 3u) == 0u)
#define OPT_CENTER      0x600
#define OPT_CENTERY     0x400

typedef struct {
    uint32_t obj[16];           /* 00 Widget-Objekt (+0 vptr) */
    uint32_t vt[10];            /* 40 */
    uint32_t slot;              /* 68 */
    uint32_t shown;             /* 6C 1 = sichtbar */
    uint32_t x10, armed;        /* 70 angezeigtes Tempo (BPM × 10), Punkt */
    uint32_t cover, users;      /* 78 045: ausgeblendete Kopfzeilen-Balken (Bit = Platz), wer sie braucht (1 Tempo, 2 REC) */
} tw_t;                         /* 0x80 */
_Static_assert(0x1FFFB870u + sizeof(tw_t) <= 0x1FFFB900u, "tw_t vor NIS");
#define TW ((volatile tw_t *)0x1FFFB870u)

static uint32_t mgr_call(uint32_t off, uint32_t a) {
    const uint32_t *vt = (const uint32_t *)(*(const uint32_t *)MGR);
    return ((uint32_t (*)(uint32_t, uint32_t))vt[off / 4u])(MGR, a);
}

/* Zeichen einzeln, jedes zentriert in einem festen Feld (cw px; ':' und '.' halb so breit) */
void tw_cells(uint32_t x, uint32_t y, uint32_t font, const char *s, uint32_t cw) {
    char c[2]; c[1] = 0;
    for (; *s; s++) {
        uint32_t w = (*s == ':' || *s == '.') ? cw / 2u : cw;
        if (*s != ' ') { c[0] = *s; CMD_TEXT(x + w / 2u, (int32_t)y, font, OPT_CENTER, c); }
        x += w;
    }
}

/* Pille rechts in der Kopfzeile: [Punkt] [3 Ziffernfelder] BPM */
static uint32_t tw_draw(void *self) {
    (void)self;
    volatile tw_t *t = TW;
    const uint32_t x0 = 232u, x1 = 318u, y0 = 3u, y1 = 21u, ym = (y0 + y1) / 2u;
    uint32_t f0 = ((const uint16_t *)0x0009A924u)[4];
    CO_BEGIN(); CMD_DLSTART();
    DL_COLOR(0x111111u); DL_LINEW(16u * 6u); DL_BEGIN(9u);
    DL_V2F(x0 + 6u, y0 + 6u); DL_V2F(x1 - 6u, y1 - 6u);
    DL_END();
    if (t->armed) { DL_COLOR(0xFF5540u); DL_POINTSZ(16u * 3u); DL_BEGIN(2u); DL_V2F(x0 + 8u, ym); DL_END(); }
    char d[4]; uint32_t b = (t->x10 + 5u) / 10u;       /* ganze BPM, 3 Stellen rechtsbuendig */
    if (b > 999u) b = 999u;
    d[0] = b >= 100u ? (char)('0' + b / 100u) : ' ';
    d[1] = b >= 10u ? (char)('0' + b / 10u % 10u) : ' ';
    d[2] = (char)('0' + b % 10u); d[3] = 0;
    DL_COLOR(0xFFFFFFu);
    tw_cells(x0 + 15u, ym, f0, d, 10u);
    CMD_TEXT(x0 + 48u, (int32_t)ym, f0, OPT_CENTERY, "BPM");
    CO_END();
    return 0;
}
static uint32_t tw_nop(void *self) { (void)self; return 0; }

static uint32_t ensure_slot(void) {
    volatile tw_t *t = TW;
    uint32_t me = (uint32_t)(uintptr_t)&t->obj[0];
    uint32_t arr = *(const volatile uint32_t *)(MGR + 8u);
    if (!RAM_OK(arr)) return 0xFFFFFFFFu;
    if (t->slot < 50u && ((volatile uint32_t *)arr)[t->slot] == me) return t->slot;
    for (uint32_t k = 0; k < 50u; k++) {
        if (((volatile uint32_t *)arr)[k] == 0u) {
            for (uint32_t i = 0; i < 16u; i++) t->obj[i] = 0;
            for (uint32_t i = 0; i < 10u; i++) t->vt[i] = (uint32_t)(uintptr_t)tw_nop | 1u;
            t->vt[0] = (uint32_t)(uintptr_t)tw_draw | 1u;
            t->vt[1] = 0x0000877Du; t->vt[2] = 0x00008751u;
            t->obj[0] = (uint32_t)(uintptr_t)&t->vt[0];
            t->obj[13] = 13u;
            ((volatile uint32_t *)arr)[k] = me;
            t->slot = k;
            return k;
        }
    }
    return 0xFFFFFFFFu;
}

void tw_reset(void) { volatile tw_t *t = TW; t->slot = 0xFFFFFFFFu; t->shown = 0; t->x10 = 0; t->armed = 0; t->cover = 0; t->users = 0; }

/* 045: Der Kopfzeilen-Balken der Firmware (Platz 12 am Geraet: Flags 0x6000|sichtbar, x 2, y -6, Breite 316) liegt im Stapel
 * ueber unseren Pillen (gezeichnet wird von Platz 49 nach 0, alle unteren Plaetze sind belegt). Solange eine Pille steht, wird
 * er ausgeblendet und danach wieder eingeblendet; zeigt die Firmware ihn zwischendurch wieder, blendet der naechste Aufruf ihn
 * erneut aus. Die Texte (Plaetze 10/11) bleiben. who: 1 Tempo, 2 REC. */
void tw_cover(uint32_t who, int on) {
    volatile tw_t *t = TW;
    if (*(const volatile uint32_t *)MGR != 0x0009AA1Cu) return;
    uint32_t arr = *(const volatile uint32_t *)(MGR + 8u);
    if (!RAM_OK(arr)) return;
    if (t->users > 3u) { t->users = 0; t->cover = 0; }
    t->users = on ? (t->users | who) : (t->users & ~who);
    int changed = 0;
    for (uint32_t k = 0; k < 32u; k++) {
        uint32_t o = ((volatile uint32_t *)arr)[k];
        if (!RAM_OK(o) || o == (uint32_t)(uintptr_t)&t->obj[0]) continue;
        const volatile int32_t *w = (const volatile int32_t *)(uintptr_t)o;
        uint32_t fl = (uint32_t)w[3];
        int bar = (fl & 0x6000u) == 0x6000u && w[7] <= 4 && w[8] <= 0 && w[9] >= 300;   /* +0x1C x, +0x20 y, +0x24 Breite */
        if (t->users && bar && (fl & 1u)) { mgr_call(0x28u, k); t->cover |= 1u << k; changed = 1; }
        else if (!t->users && (t->cover & (1u << k))) { mgr_call(0x24u, k); changed = 1; }
    }
    if (!t->users) t->cover = 0;
    if (changed) mgr_call(0x0Cu, 0);
}

/* 045: jede Hauptschleife bei offenem Menue: Balken draussen halten */
void tw_keep(void) { if (TW->shown == 1u) tw_cover(1u, 1); }

/* on = Menue offen. Neu gezeichnet nur bei Aenderung. */
void tw_set(int on, uint32_t x10, uint32_t armed) {
    volatile tw_t *t = TW;
    if (*(const volatile uint32_t *)MGR != 0x0009AA1Cu || *(const volatile uint32_t *)(MGR + 4u) != 1u) return;
    if (t->shown > 1u) tw_reset();                       /* SRAM beim Kaltstart undefiniert */
    if (!on) {
        if (t->shown && t->slot < 50u) { mgr_call(0x28u, t->slot); mgr_call(0x08u, t->slot); mgr_call(0x0Cu, 0); }
        if (t->shown || (t->users & 1u)) tw_cover(1u, 0);
        t->shown = 0;
        return;
    }
    tw_cover(1u, 1);                                     /* 045: Kopfzeilen-Balken weg, sonst liegt er ueber der Pille */
    if (t->shown && t->x10 == x10 && t->armed == armed) return;
    if (ensure_slot() >= 50u) return;
    t->x10 = x10; t->armed = armed ? 1u : 0u;
    if (!t->shown) mgr_call(0x24u, t->slot);
    t->shown = 1;
    mgr_call(0x08u, t->slot);
    mgr_call(0x0Cu, 0);
}
#endif
