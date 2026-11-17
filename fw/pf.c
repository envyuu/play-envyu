/* 045 (12.11.2026) – Perform: Spurauswahl bleibt beim Wechsel erhalten
 *
 * Befund (Rev A, ✅ Code): Perform-Seite (vtable 0x9DE0C) ruft beim Betreten (vt+4 = FUN_00022398, bl @0x223B6) und
 * Verlassen (vt+8 = FUN_0002229c, bl @0x222D4) FUN_0003a974(Funktionspads) auf. Die setzt Pads/LEDs zurueck und ruft
 * FUN_0002dfb4(Auswahl) = Auswahl loeschen (Bitmaske +0xC, Step-Liste +4/+0x800, Spurobjekte vt+0x38(0)).
 * Hook: Bitmaske vorher merken, Original laufen lassen, gemerkte Spuren ueber den Original-Setter
 * FUN_0002e77c(auswahl, spur, 1) wieder waehlen (Bit + alle 64 Steps + Spurobjekt), danach FUN_00020bb0(SeqCtl)
 * (Fusszeile/Zustand "Spuren gewaehlt") wie am Ende von FUN_0003a974.
 * Funktionspads: +4 = SeqController (0x1FFF44E0), +8 = Auswahl (0x1FFF450C).
 */
#include <stdint.h>

#define RAM_OK(p)   ((p) >= 0x1FFF0000u && (p) < 0x20030000u && ((p) & 3u) == 0u)
#define FP_RESET    ((void (*)(uint32_t))0x0003A975u)              /* FUN_0003a974 */
#define SEL_TRACK   ((void (*)(uint32_t, uint32_t, int))0x0002E77Du)  /* FUN_0002e77c(auswahl, spur, an) */
#define FOOTER      ((void (*)(uint32_t))0x00020BB1u)              /* FUN_00020bb0(SeqController) */

uint32_t pf_keep_mask(uint32_t m) { return m & 0xFFFFu; }          /* Spuren 0–7 Audio, 8–15 MIDI */

__attribute__((used, section(".text.entry_pfkeep")))
int um_keep_sel(void) __attribute__((weak));               /* usbmenu.c (12.11.): Menue "Keep Selection" */
void pf_reset_keep(uint32_t fp) {
    if (um_keep_sel && !um_keep_sel()) { FP_RESET(fp); return; }   /* Off = Original: Auswahl wird geloescht */
    uint32_t ctl = RAM_OK(fp) ? *(const volatile uint32_t *)(fp + 4u) : 0u;
    uint32_t sel = RAM_OK(fp) ? *(const volatile uint32_t *)(fp + 8u) : 0u;
    uint32_t m = RAM_OK(sel) ? pf_keep_mask(*(const volatile uint32_t *)(sel + 0xCu)) : 0u;
    FP_RESET(fp);
    if (!m || !RAM_OK(ctl)) return;
    for (uint32_t t = 0; t < 16u; t++) if (m & (1u << t)) SEL_TRACK(sel, t, 1);
    FOOTER(ctl);
}

/* Betreten (bl @0x223B6): FUN_0002144c hat die Funktionspads der neuen Seite schon VOR vt+4 gezeichnet (vt+0x30,
 * 8 Reihen × 4 Arten) – mit leerer Auswahl. Nach dem Wiederherstellen also wie dort neu zeichnen, sonst ist die
 * Auswahl da, aber unsichtbar (Geraet 12.11.: Maske 0x80 in Perform, Pads dunkel). Beim Verlassen zeichnet die
 * neue Seite danach selbst. */
__attribute__((used, section(".text.entry_pfenter")))
void pf_reset_keep_enter(uint32_t fp) {
    uint32_t sel = RAM_OK(fp) ? *(const volatile uint32_t *)(fp + 8u) : 0u;
    uint32_t had = RAM_OK(sel) ? pf_keep_mask(*(const volatile uint32_t *)(sel + 0xCu)) : 0u;
    pf_reset_keep(fp);
    uint32_t ctl = RAM_OK(fp) ? *(const volatile uint32_t *)(fp + 4u) : 0u;
    if (!had || !RAM_OK(ctl)) return;
    uint32_t page = *(const volatile uint32_t *)(ctl + 0x24B4u);
    if (!RAM_OK(page)) return;
    uint32_t vt = *(const volatile uint32_t *)page;
    void (*draw)(uint32_t, uint32_t, uint32_t, uint32_t) = (void (*)(uint32_t, uint32_t, uint32_t, uint32_t))(*(const volatile uint32_t *)(vt + 0x30u));
    for (uint32_t row = 0; row < 8u; row++)
        for (uint32_t k = 0; k < 4u; k++) draw(page, row, k, 0u);
}

/* ---------------------------------------------------------------------------------------------------------------
 * 045 (12.11.2026) – Anschlag-Feedback in Perform: Mute-Pad (1. Spalte rechts neben den Steps) blinkt bei jedem Step
 *
 * ✅ Code: Spalten rechts je Reihe (Pad = reihe·20 + 16 + art): Art 0 Mute (Funktionspads +0x11C), 1 Solo (+0x120),
 *   2 Variation (+0x118), 3 Select. Perform-Seite vt[12] = FUN_000222ec(seite, reihe, art) zeichnet ein Pad;
 *   gemutet = hell (Audio 6AFF5D, MIDI FFB9FF), sonst fast aus. MIDI-Seite (+0x112) zeigt Spuren 8–15.
 * Anschlag: MidiTrack_triggerStep 0x4904C (alle Spuren, auch gemutete; ev+0x16 = stumm) wird an den drei Aufrufern
 *   0x49D64/0x49EAE/0x49FBE durch pf_trigger_hook ersetzt (Befehl fuer Befehl nachgebaut) + Zeitstempel je Spur.
 * Anzeige: pf_poll (Hauptschleife) setzt das Mute-Pad fuer PF_BLINK_MS auf die Engine-Farbe (Slicer gelb wie die
 *   Slices, Audio gruen, MIDI rosa wie die Seite) – ungemutet hell, gemutet gedimmt –, danach zeichnet die Firmware
 *   das Pad selbst neu (vt[12]). Nur wenn Perform aktiv ist und keine Tastatur-Ueberlagerung (FUN_000230a4) steht.
 * RAM: pf_t bei 0x1FFF9F80 (bis 0x1FFF9FFF frei; 0x1FFF9F60–7B nur Test-Attrappe rec_stub).
 */
#define PF_MAGIC     0x50463031u
#define PF_BLINK_MS  70u
typedef struct {
    uint32_t magic;           /* 00 */
    uint32_t t[16];           /* 04 letzter Anschlag je Spur (millis | 1, 0 = nie) */
    uint32_t shown;           /* 44 Reihen, deren Mute-Pad gerade blinkt */
    uint32_t key;             /* 48 Seite (Perform-Zeiger ^ MIDI) beim Blinken */
    uint32_t n;               /* 4C Anschlaege gezaehlt */
    uint32_t n_off;           /* 50 12.11.: Aufrufe fuer leere Steps */
    uint32_t n_prev;          /* 54 12.11.: Vorschau-Umleitungen (Swap/Scatter/Thin Out) */
    uint32_t rz_key;          /* 58 12.11.: Stand der sichtbaren Randomize-Vorschau (nis.c rz_poll) */
} pf_t;
_Static_assert(0x1FFF9F80u + sizeof(pf_t) <= 0x1FFFA000u, "pf_t");

#ifdef HOST_TEST
extern uint8_t host_pf_ram[];
#define PF ((volatile pf_t *)(void *)host_pf_ram)
#else
#define PF           ((volatile pf_t *)0x1FFF9F80u)
#define MILLIS()     (*(volatile uint32_t *)0x20000D7Cu)
#define RESOLVE      ((int (*)(void *, uint32_t, void *, int))0x00048D01u)   /* MidiTrack_resolveEvent */
#define SEQCTL       0x1FFF44E0u
#define KB_ACTIVE(k) (((uint32_t (*)(uint32_t))0x000230A5u)(k) & 0xFFu)      /* FUN_000230a4: Tastatur ueberlagert */
#define PAD_REDRAW   ((void (*)(uint32_t, uint32_t, uint32_t))0x000222EDu)   /* Perform vt[12] */
#endif
#define PAT_A        0x200025D0u
#define PAT_B        0x2000E060u

/* Spur aus ctx (= Spurobjekt + 0x88; Spuren ab Pattern + 0x10 im Abstand 0xBA8, 0–7 Audio, 8–15 MIDI) */
int pf_track_of(uint32_t ctx) {
    uint32_t obj = ctx - 0x88u;
    for (uint32_t b = 0; b < 2u; b++) {
        uint32_t off = obj - ((b ? PAT_B : PAT_A) + 0x10u);
        if (off < 16u * 0xBA8u && off % 0xBA8u == 0u) return (int)(off / 0xBA8u);
    }
    return -1;
}

/* Engine-Farbe: 0 Audio gruen, 1 Slicer gelb, 2 MIDI rosa; hell (ungemutet) bzw. gedimmt (gemutet) */
static const uint8_t PF_COL[3][2][3] = {
    { { 0x6A, 0xFF, 0x5D }, { 0x1A, 0x40, 0x17 } },
    { { 0xFF, 0xFF, 0x61 }, { 0x40, 0x40, 0x18 } },
    { { 0xFF, 0xB9, 0xFF }, { 0x40, 0x2E, 0x40 } },
};
const uint8_t *pf_color(uint32_t kind, int muted) { return PF_COL[kind > 2u ? 0u : kind][muted ? 1 : 0]; }

#ifndef HOST_TEST
#include "slicer.h"
#define SLR ((volatile sl_t *)0x1FFF9800u)
static uint32_t kind_of(uint32_t t) {
    if (t >= 8u) return 2u;
    return (SLR->magic == SL_MAGIC && sl_slices(SLR->mode[t])) ? 1u : 0u;
}

int rz_preview_src(uint32_t trk, uint32_t t, uint32_t j) __attribute__((weak));   /* nis.c (12.11.) */
__attribute__((used, section(".text.entry_pftrig")))
void pf_trigger_hook(void *step, uint32_t ctx, uint8_t *ev) {
    {   /* 12.11.: Vorschau Swap/Scatter/Thin Out – an Position j spielt der Step, der nach dem Einbacken dort steht */
        int t = pf_track_of(ctx);
        uint32_t trk = ctx - 0x88u, off = (uint32_t)(uintptr_t)step - (trk + 0x128u);
        if (t >= 0 && rz_preview_src && off < 64u * 42u && off % 42u == 0u && PF->magic == PF_MAGIC) {
            int src = rz_preview_src(trk, (uint32_t)t, off / 42u);
            if (src == -2) { PF->n_prev++; return; }
            if (src >= 0) { step = (void *)(uintptr_t)(trk + 0x128u + 42u * (uint32_t)src); PF->n_prev++; }
        }
    }
    int8_t f = *(const volatile int8_t *)((uintptr_t)step + 0x29u);
    if (f >= 0) { if (PF->magic == PF_MAGIC) PF->n_off++; return; }   /* Step aus (Zaehler: ruft der Sequencer leere Steps?) */
    int r = RESOLVE(step, ctx, ev, f);
    if (r) {
        volatile pf_t *p = PF;
        int t = pf_track_of(ctx);
        if (p->magic == PF_MAGIC && t >= 0) { p->t[t] = MILLIS() | 1u; p->n++; }
        if (ev[0x16] == 0u) {
            uint32_t vt = *(const volatile uint32_t *)step;
            ((void (*)(void *, uint32_t, void *, int))(*(const volatile uint32_t *)(vt + 0x54u)))(step, ctx, ev, r);
        }
    }
}

void rz_poll(void) __attribute__((weak));   /* nis.c (12.11.): sichtbare Vorschau */
void pf_poll(void) {
    volatile pf_t *p = PF;
    if (p->magic == PF_MAGIC && rz_poll) rz_poll();
    if (p->magic != PF_MAGIC) {
        volatile uint32_t *z = (volatile uint32_t *)(void *)p;
        for (uint32_t i = 0; i < sizeof(pf_t) / 4u; i++) z[i] = 0;
        p->magic = PF_MAGIC;
        return;
    }
    uint32_t cur = *(const volatile uint32_t *)(SEQCTL + 0x24B4u);
    uint32_t perf = *(const volatile uint32_t *)(SEQCTL + 0x24D8u);
    uint32_t fp = *(const volatile uint32_t *)(SEQCTL + 0x24CCu);
    if (!RAM_OK(perf) || cur != perf || !RAM_OK(fp)) { p->shown = 0; return; }   /* Firmware zeichnet beim Wechsel selbst */
    uint32_t midi = *(const volatile uint8_t *)(fp + 0x112u) ? 1u : 0u;
    uint32_t key = perf ^ midi;
    if (key != p->key) { p->key = key; p->shown = 0; }
    if (KB_ACTIVE(fp + 0x14u)) { p->shown = 0; return; }
    uint32_t pads = *(const volatile uint32_t *)(fp + 0xCu);
    if (!RAM_OK(pads)) return;
    uint32_t mute = *(const volatile uint32_t *)(fp + 0x11Cu);
    uint32_t now = MILLIS();
    for (uint32_t row = 0; row < 8u; row++) {
        uint32_t t = row + 8u * midi, ts = p->t[t];
        int want = ts != 0u && now - (ts & ~1u) < PF_BLINK_MS;
        int on = (p->shown >> row) & 1u;
        if (want && !on) {
            const uint32_t *lvt = (const uint32_t *)(uintptr_t)*(const volatile uint32_t *)pads;
            ((void (*)(uint32_t, int, const uint8_t *, int, int))lvt[1])(pads, (int)(row * 0x14u + 0x10u), pf_color(kind_of(t), (mute >> t) & 1u), 0, 0);
            p->shown |= 1u << row;
        } else if (!want && on) {
            PAD_REDRAW(perf, row, 0u);                           /* Firmware-Zustand (gemutet hell / aus) */
            p->shown &= ~(1u << row);
        }
    }
}
#endif
