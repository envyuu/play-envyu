/* Host test of the Live Rec long-press state machine and status text (lrec.c), ASan/UBSan, 32 bit */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lrec.h"

uint8_t host_lr_ram[0xA4] __attribute__((aligned(8)));
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FEHLER "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    volatile lr_t *s = (volatile lr_t *)(void *)host_lr_ram;
    lr_reset();
    CHECK(lr_step(s, 7, 0, 0, 1000, 0) == LR_FWD && lr_step(s, 3, 0, 0, 1000, 1) == LR_FWD, "ohne rec_request bzw. andere Taste: weiter");
    CHECK(lr_step(s, 7, 0, 1, 1000, 1) == LR_FWD && lr_step(s, 7, 1, 1, 1100, 1) == LR_FWD, "Shift + Live Rec (Undo): weiter");
    /* kurz: Druecken zurueckhalten, Wiederholung bei 400 ms schluckt, Loslassen bei 600 → aufgeschoben */
    CHECK(lr_step(s, 7, 0, 0, 2000, 1) == 0, "kurz: Druecken zurueckgehalten");
    CHECK(lr_step(s, 7, 2, 0, 2401, 1) == 0, "kurz: Wiederholung 401 ms geschluckt");
    CHECK(lr_step(s, 7, 1, 0, 2600, 1) == LR_DEFERRED, "kurz: Loslassen → Druecken+Loslassen ans Original");
    /* lang: Wiederholungen wie FUN_000065B4: 401, +300, +200, +150, +100, +80 … */
    lr_step(s, 7, 0, 0, 10000, 1);
    uint32_t t = 10000, dt[] = { 401, 300, 200, 150, 100, 80, 80, 80 }, toggles = 0, at = 0;
    for (uint32_t i = 0; i < 8; i++) { t += dt[i]; uint32_t r = lr_step(s, 7, 2, 0, t, 1); if (r & LR_TOGGLE) { toggles++; at = t - 10000; } else if (r) toggles += 100; }
    CHECK(toggles == 1 && at >= 1000 && at < 1200, "lang: genau eine Umschaltung, nach %u ms", at);
    CHECK(lr_step(s, 7, 1, 0, t + 5, 1) == 0, "lang: Loslassen geschluckt (kein Live Rec)");
    CHECK(s->n_long == 1 && s->n_short == 1, "Zaehler");
    CHECK(lr_step(s, 7, 1, 0, t + 50, 1) == LR_FWD && lr_step(s, 7, 2, 0, t + 60, 1) == LR_FWD, "Loslassen/Wiederholung ohne Druecken: weiter");
    char b[24];
    lr_text(b, 2, 37, 0); CHECK(!strcmp(b, "USB REC 0:03"), "Text %s", b);
    lr_text(b, 2, 6015, 1); CHECK(!strcmp(b, "USB REC 10:01 !"), "Text %s", b);
    lr_text(b, 1, 0, 0); CHECK(!strcmp(b, "USB REC ..."), "Text %s", b);
    lr_text(b, 3, 655, 0); CHECK(!strcmp(b, "USB REC 1:05 OK"), "Text %s", b);
    printf("%d Pruefungen, %d Fehler\n", checks, fails);
    return fails ? 1 : 0;
}
