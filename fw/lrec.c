/* Patch 023 – "Live Rec" lange halten (~1 s) = USB-Aufnahme an/aus, Statusanzeige "USB REC m:ss"
 *
 * Tastenweg der Firmware (Rev A, siehe Vault Firmware/Display_und_UI.md):
 *   Tasten-Scanner FUN_00006610 (Multiplexer, alle 0,5 ms eine Taste) → FUN_00006560 (Flanke) / FUN_000065B4
 *   (Wiederholung: erstes Ereignis nach 401 ms, dann nach 300/200/150/100/80/80 ms) → Rueckrufe 0x4BF8/0x4C14/0x4C30
 *   → Zuhoerer (*(0x200004A0))+0xB80 → vt[1](zuhoerer, taste, ereignis) mit ereignis 0 = gedrueckt, 1 = losgelassen,
 *   2 = gehalten (Wiederholung). Zuhoerer = UI-Objekt (vtable 0xA0158), vt[1] = FUN_0002A774 (Tasten-Verteiler):
 *   Taste 10 = Shift (UI+0x18), sonst aktiver Bildschirm → Menues FUN_00032E04 → SeqEdit_action 0x2165C.
 *   Live Rec = Taste 7: wirkt schon beim DRUECKEN (SeqEdit_action case 7, Ereignis 0); mit Shift = Undo.
 *
 * Umsetzung (nur wenn rec_request vorhanden ist, d. h. zusammen mit usbrec.c aus Patch 024; allein ist 023 hier
 * Original-Verhalten): vtable-Wort 0xA015C (vt[1] des UI-Objekts) → lr_button_hook.
 *   - Live Rec ohne Shift gedrueckt: Ereignis zurueckhalten, Zeit merken.
 *   - Wiederholungs-Ereignis (2) und >= 1000 ms gehalten: rec_request(an/aus) — Live Rec wird NICHT ausgeloest.
 *   - Losgelassen vor 1000 ms: gemerktes Druecken + Loslassen an das Original → normales Live Rec (beim Loslassen
 *     statt beim Druecken, also bis zu 1 s spaeter).
 *   - Alle anderen Tasten, Shift + Live Rec (Undo) und alles ohne rec_request: unveraendert.
 * Statusanzeige: eigenes Widget (wie wave.c) im niedrigsten freien Manager-Platz (wird zuletzt angehaengt = oben),
 *   roter Balken oben Mitte "USB REC m:ss", aktualisiert in lr_poll (Hauptschleife) bei jeder neuen Sekunde.
 *
 * RAM: lr_t bei 0x1FFF9EB0 (hinter wv_t, bis 0x1FFF9FFF).
 */
#include <stdint.h>
#include "lrec.h"

#ifdef HOST_TEST
extern uint8_t host_lr_ram[];
#define LR ((volatile lr_t *)(void *)host_lr_ram)
#else
#define LR ((volatile lr_t *)0x1FFF9EB0u)
int  rec_request(int on) __attribute__((weak));
void rec_last_import(int32_t *slot, int32_t *res) __attribute__((weak));   /* usbrec.c (026) */
void rec_status(uint32_t *st, uint32_t *ds, uint32_t *drops, uint32_t *num, int32_t *err) __attribute__((weak));
#define MILLIS()     (*(volatile uint32_t *)0x20000D7Cu)
#define ORIG_BUTTON  ((uint32_t (*)(void *, uint32_t, uint32_t))0x0002A775u)    /* FUN_0002A774 Tasten-Verteiler */
#define MGR          0x1FFF42CCu                         /* HAL 0x1FFF3740 + 0xB8C: Widget-Manager */
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
#define RAM_OK(p)       ((p) >= 0x1FFF0000u && (p) < 0x2003FF00u && ((p) & 3u) == 0u)
#define TOAST(t)        do { if (*(const volatile uint32_t *)0x1FFF6CF8u == MGR) F(0x0002A395u, void (*)(uint32_t, const char *))(0x1FFF6CF8u, (t)); } while (0)   /* Toast_show(App+0x2850, Text) wie "Session saved"; Objekt [0] = Widget-Manager, Plaetze 7-9 (oben) */
#endif

/* reine Zustandsmaschine (Host-Test) */
uint32_t lr_step(volatile lr_t *s, uint32_t btn, uint32_t ev, uint32_t shift, uint32_t now, int enabled) {
    if (!enabled || btn != LR_BTN) return LR_FWD;
    if (ev == 0u) {
        if (shift) { s->down = 0; return LR_FWD; }           /* Shift + Live Rec = Undo: unveraendert */
        s->down = 1; s->fired = 0; s->t = now;
        return 0;
    }
    if (!s->down) return LR_FWD;
    if (ev == 2u) {
        if (!s->fired && now - s->t >= LR_LONG_MS) { s->fired = 1; s->n_long++; return LR_TOGGLE; }
        return 0;
    }
    if (ev == 1u) {
        s->down = 0;
        if (s->fired) return 0;
        s->n_short++;
        return LR_DEFERRED;
    }
    return LR_FWD;
}

/* "USB REC m:ss" (+ " !" bei Aussetzern), ds = Zehntelsekunden */
void lr_text(volatile char *b, uint32_t st, uint32_t ds, uint32_t drops) {
    static const char H[] = "USB REC ";
    uint32_t k = 0;
    for (const char *p = H; *p; p++) b[k++] = *p;
    if (st == 1u) { const char *p = "..."; while (*p) b[k++] = *p++; b[k] = 0; return; }
    uint32_t sec = ds / 10u, m = sec / 60u, ss = sec % 60u;
    if (m > 99u) m = 99u;
    if (m >= 10u) b[k++] = (char)('0' + m / 10u);
    b[k++] = (char)('0' + m % 10u);
    b[k++] = ':'; b[k++] = (char)('0' + ss / 10u); b[k++] = (char)('0' + ss % 10u);
    if (st == 3u) { b[k++] = ' '; b[k++] = 'O'; b[k++] = 'K'; }
    if (drops) { b[k++] = ' '; b[k++] = '!'; }
    b[k] = 0;
}

void lr_reset(void) {
    volatile lr_t *s = LR;
    volatile uint32_t *z = (volatile uint32_t *)(void *)s;
    for (uint32_t i = 0; i < sizeof(lr_t) / 4u; i++) z[i] = 0;
    s->slot = 0xFFFFFFFFu;
    s->magic = LR_MAGIC;
}

#ifndef HOST_TEST
static int enabled(void) { return LR->magic == LR_MAGIC && rec_request != 0; }

/* ersetzt vt[1] des UI-Objekts (vtable 0xA0158, Wort 0xA015C) */
__attribute__((used, section(".text.entry_lrbtn")))
void sv_set_ui(uint32_t ui) __attribute__((weak));   /* slview.c (026): Shift fuer die Slice-Seiten */
uint32_t lr_button_hook(void *ui, uint32_t btn, uint32_t ev) {
    volatile lr_t *s = LR;
    if (sv_set_ui) sv_set_ui((uint32_t)(uintptr_t)ui);
    uint32_t shift = RAM_OK((uint32_t)(uintptr_t)ui) ? *(const volatile uint8_t *)((uintptr_t)ui + 0x18u) : 0u;
    uint32_t r = lr_step(s, btn, ev, shift, MILLIS(), enabled());
    if (r & LR_TOGGLE) {
        uint32_t st = 0, ds = 0, dr = 0, num = 0; int32_t err = 0;
        if (rec_status) rec_status(&st, &ds, &dr, &num, &err);
        s->req_res = rec_request(st == 0u ? 1 : 0);
        s->n_req++;
#ifndef HOST_TEST
        if (!s->req_res && st == 0u) TOAST("USB REC: Audio Out = Stereo + In");
#endif
        return 1;
    }
    if (r & LR_DEFERRED) { ORIG_BUTTON(ui, LR_BTN, 0u); return ORIG_BUTTON(ui, LR_BTN, 1u); }
    if (r & LR_FWD) return ORIG_BUTTON(ui, btn, ev);
    return 1;
}

static uint32_t mgr_call(uint32_t mgr, uint32_t off, uint32_t a) {
    const uint32_t *vt = (const uint32_t *)(*(const uint32_t *)mgr);
    return ((uint32_t (*)(uint32_t, uint32_t))vt[off / 4u])(mgr, a);
}

static uint32_t lr_draw(void *self) {
    (void)self;
    volatile lr_t *s = LR;
    const uint32_t x0 = 160u - LR_PILL_W / 2u, x1 = 160u + LR_PILL_W / 2u, y0 = 3u, y1 = 21u;
    uint32_t f0 = ((const uint16_t *)0x0009A924u)[4];       /* Font 0 (16×22), Handle aus der Tabelle */
    CO_BEGIN(); CMD_DLSTART();
    DL_COLOR(0x111111u); DL_LINEW(16u * 6u); DL_BEGIN(9u);  /* RECTS, Radius 6 px */
    DL_V2F(x0 + 6u, y0 + 6u); DL_V2F(x1 - 6u, y1 - 6u);
    DL_END();
    DL_COLOR(0xFF2025u); DL_POINTSZ(16u * 4u); DL_BEGIN(2u);  /* POINTS: roter Punkt */
    DL_V2F(x0 + 12u, (y0 + y1) / 2u); DL_END();
    DL_COLOR(0xFFFFFFu);
    CMD_TEXT(x0 + 22u, (y0 + y1) / 2u, f0, 0x400, (const char *)s->txt);   /* OPT_CENTERY */
    CO_END();
    return 0;
}
static uint32_t lr_nop(void *self) { (void)self; return 0; }

static uint32_t slot_ptr(uint32_t k) {
    uint32_t arr = *(const volatile uint32_t *)(MGR + 8u);
    if (!RAM_OK(arr) || k >= 50u) return 0;
    return ((const volatile uint32_t *)arr)[k];
}

/* niedrigster freier Platz = wird zuletzt angehaengt = liegt oben */
static uint32_t ensure_slot(void) {
    volatile lr_t *s = LR;
    uint32_t me = (uint32_t)(uintptr_t)&s->obj[0];
    if (s->slot < 50u && slot_ptr(s->slot) == me) return s->slot;
    uint32_t arr = *(const volatile uint32_t *)(MGR + 8u);
    if (!RAM_OK(arr)) return 0xFFFFFFFFu;
    for (uint32_t k = 0; k < 50u; k++) {
        if (((volatile uint32_t *)arr)[k] == 0u) {
            for (uint32_t i = 0; i < 16u; i++) s->obj[i] = 0;
            for (uint32_t i = 0; i < 10u; i++) s->vt[i] = (uint32_t)(uintptr_t)lr_nop | 1u;
            s->vt[0] = (uint32_t)(uintptr_t)lr_draw | 1u;
            s->vt[1] = 0x0000877Du; s->vt[2] = 0x00008751u;
            s->obj[0] = (uint32_t)(uintptr_t)&s->vt[0];
            s->obj[13] = 13u;
            ((volatile uint32_t *)arr)[k] = me;
            s->slot = k;
            return k;
        }
    }
    return 0xFFFFFFFFu;
}

/* Hauptschleife */
void lr_poll(void) {
    volatile lr_t *s = LR;
    if (s->magic != LR_MAGIC || !rec_status) return;
    if (*(const volatile uint32_t *)MGR != 0x0009AA1Cu || *(const volatile uint32_t *)(MGR + 4u) != 1u) return;
    uint32_t st = 0, ds = 0, dr = 0, num = 0; int32_t err = 0;
    rec_status(&st, &ds, &dr, &num, &err);
    {                                                    /* 026: sichtbare Meldung bei Start/Ende ueber den Toast der Firmware */
        uint32_t prev = s->_r;
        if (st == 2u && prev != 2u) { char t[24]; uint32_t k = 0; const char *p = "USB REC  Rec_"; while (*p) t[k++] = *p++;
            t[k++] = (char)('0' + num / 100u % 10u); t[k++] = (char)('0' + num / 10u % 10u); t[k++] = (char)('0' + num % 10u); t[k] = 0; TOAST(t); }
        else if (st == 0u && prev >= 2u) {
            int32_t sl = -1, ir = -1;
            if (rec_last_import) rec_last_import(&sl, &ir);
            if (err) TOAST("USB REC error");
            else if (sl >= 0) { char t[24]; uint32_t k = 0; const char *p = "Rec -> Sample "; while (*p) t[k++] = *p++;
                uint32_t n = (uint32_t)sl + 1u; if (n >= 100u) t[k++] = (char)('0' + n / 100u); if (n >= 10u) t[k++] = (char)('0' + n / 10u % 10u);
                t[k++] = (char)('0' + n % 10u); t[k] = 0; TOAST(t); }
            else if (ir == 1) TOAST("Rec: sample memory full");
            else if (ir == 2) TOAST("Rec: 255 samples reached");
            else if (ir == 3) TOAST("Rec empty: no USB audio");
            else TOAST("USB REC saved");
        }
        else if (st == 0u && prev == 1u && err) TOAST("USB REC: Stereo + In?");
        s->_r = (uint8_t)st;
    }
    uint32_t want = st != 0u;
    uint32_t key = (st << 28) ^ (ds / 10u) ^ (dr ? 0x08000000u : 0u);
    if (want == s->shown && (!want || key == s->key)) return;
    if (ensure_slot() >= 50u) return;
    s->key = key;
    if (want) { lr_text(s->txt, st, ds, dr); mgr_call(MGR, 0x24u, s->slot); }
    else mgr_call(MGR, 0x28u, s->slot);
    s->shown = (uint8_t)want;
    mgr_call(MGR, 0x08u, s->slot);                      /* neu zeichnen + Bild */
    mgr_call(MGR, 0x0Cu, 0);
}
#endif
