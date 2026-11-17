/* Patch 045 – Color Mode (Farbschwaeche). Polyend Play Rev A, Firmware 1.6.0.
 *
 * Alle Pad- und Tasten-LEDs laufen ueber Leds_setPixel 0x6CD4 (obj, i, r, g, b) (7 Aufrufe, u. a. aus Leds_setColor 0x4DC4,
 * auch Blinken). Die 7 bl werden auf cb_pixel umgebogen: Farbe umrechnen, dann das Original.
 * Umrechnung = feste Kanalmischung, gesucht ueber die echten Pad-Farben der Firmware (0x9DC70…) unter simulierter
 * Farbschwaeche (Vienot 1999), Ziel: kleinster Abstand unterscheidbarer Farbpaare moeglichst gross:
 *   1 Red-Green (Deutan/Protan): R' = R + B, G' = R, B' = (G + B) / 2   Rot → Gelb, Gruen → Violett, Weiss bleibt   (31 → 59)
 *   2 Blue-Yellow (Tritan):      R' = R, G' = (R + G) / 2, B' = B       Rot → Orange, Gruen → Petrol               (81 → 117)
 * Einstellung: Menue-Zeile "Color Mode" (Hauptmenue), gespeichert in usbaudio.cfg als color= (usbmenu.c).
 * RAM: cb_t bei 0x1FFFB860 (frei nach dem Menuewert der Engines 0x1FFFB840–0x1FFFB85F).
 */
#include <stdint.h>
#include "cb.h"

#ifdef HOST_TEST
uint8_t host_cb_ram[0x10];
uint32_t host_px[5];
#define CB ((volatile cb_t *)(void *)host_cb_ram)
static void orig_pixel(void *o, uint32_t i, uint32_t r, uint32_t g, uint32_t b) { host_px[0] = (uint32_t)(uintptr_t)o; host_px[1] = i; host_px[2] = r; host_px[3] = g; host_px[4] = b; }
#else
#define CB ((volatile cb_t *)0x1FFFB860u)
#define orig_pixel ((void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))0x00006CD5u)
#endif

static uint32_t sat(uint32_t v) { return v > 255u ? 255u : v; }

void cb_map(uint32_t mode, uint32_t *r, uint32_t *g, uint32_t *b) {
    uint32_t R = *r, G = *g, B = *b;
    if (mode == 1u) { *r = sat(R + B); *g = R; *b = (G + B) >> 1; }
    else if (mode == 2u) { *g = (R + G) >> 1; }
}

__attribute__((used, section(".text.entry_cbpx")))
void cb_pixel(void *obj, uint32_t i, uint32_t r, uint32_t g, uint32_t b) {
    int32_t m = CB->mode;                                  /* SRAM beim Kaltstart undefiniert: nur 1/2 wirken */
    if (m == 1 || m == 2) { r &= 0xFFu; g &= 0xFFu; b &= 0xFFu; cb_map((uint32_t)m, &r, &g, &b); }
    orig_pixel(obj, i, r, g, b);
}
