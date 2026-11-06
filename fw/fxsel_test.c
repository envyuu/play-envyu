/* Spurauswahl der FX-Suite: Drums auf MIDI-Spur 0 (Kanal 1), Synth auf MIDI-Spur 3 (Kanal 4). ASan/UBSan. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef struct { uint32_t type; uint8_t ch, pad, d1, d2; } midi_msg_t;
void usb_send_hook(void *self, const midi_msg_t *m);
void tick_hook(void *queue, int ticks);
int fx_track_from_ctx(uint32_t ctx);
void fx_set_track(int track);

uint8_t host_ram[16384] __attribute__((aligned(8)));
uint32_t host_perform_page = 0x10000000u;
uint32_t host_sel_raw = 0, host_pattern = 0x20000000u;
static uint32_t slot[16];
uint32_t host_read32(uint32_t a) {
    if (a >= 0x10000064u && a < 0x10000064u + 64u) return slot[(a - 0x10000064u) / 4u];
    fprintf(stderr, "unerwarteter Lesezugriff %#x\n", a); exit(1);
}
#define DELAY(i) (0x000A86B4u + 20u * (i))
#define LOOP(i)  (0x000A8434u + 20u * (i))
#define CCM(i)   (0x000A8E34u + 20u * (i))
#define SEL(t)   (1u << (8 + (t)))
#define DRUM 0
#define SYN  3

static uint32_t now; static int sounding[16][128]; static int fails;
static int ons[16], offs[16], ccs[16]; static int bad_vel;
static int pitch_seen[16][128];
static void reset_counts(void) { memset(ons, 0, sizeof ons); memset(ccs, 0, sizeof ccs); memset(offs, 0, sizeof offs); memset(pitch_seen, 0, sizeof pitch_seen); }
void host_usb(void *self, const midi_msg_t *m) {
    (void)self;
    if (m->type == 4 && m->d2) { ons[m->ch]++; sounding[m->ch][m->d1]++; pitch_seen[m->ch][m->d1]++; if (m->d2 > 127) bad_vel++; }
    else if (m->type == 5 || m->type == 4) { offs[m->ch]++; sounding[m->ch][m->d1]--; }
    else if (m->type == 6 && m->d1 != 123) ccs[m->ch]++;
}
void host_din(void *self, const midi_msg_t *m) { (void)self; (void)m; }

/* Sequencer: Drum auf jedem Step-Viertel (Note 36), Synth auf jedem 2. Step (Note 60); Note-Off nach 12 Ticks */
static int off_d, off_s, running = 1;
static void note(int track, int ch, int n) { midi_msg_t m = { 4, (uint8_t)ch, 0, (uint8_t)n, 100 }; fx_set_track(track); usb_send_hook((void *)1, &m); fx_set_track(-1); }
static void noff(int ch, int n) { midi_msg_t m = { 5, (uint8_t)ch, 0, (uint8_t)n, 64 }; usb_send_hook((void *)1, &m); }
void host_tick(void *q, int t) {
    (void)q;
    for (int k = 0; k < t; k++) {
        if (!running) continue;
        if (off_d && --off_d == 0) noff(DRUM, 36);
        if (off_s && --off_s == 0) noff(SYN, 60);
        uint32_t tt = now + (uint32_t)k;
        if (tt % 96 == 0) { note(0, DRUM, 36); off_d = 12; }
        if (tt % 48 == 0) { note(3, SYN, 60); off_s = 12; }
    }
}
static void run(uint32_t ticks) { for (uint32_t i = 0; i < ticks; i++) { tick_hook(0, 1); now++; } }
#define EXPECT(c, ...) do { int ok_ = (c); printf("%s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static void clear_all(void) { memset(slot, 0, sizeof slot); host_sel_raw = 0; run(600); }
static int hanging(void) { int h = 0; for (int c = 0; c < 16; c++) for (int n = 0; n < 128; n++) if (sounding[c][n] > 0) h++; return h; }

int main(void) {
    memset(host_ram, 0xA5, sizeof host_ram);
    run(192);

    /* Spurnummer aus dem Kontext, beide Pattern-Objekte */
    { uint32_t A = 0x200025D0u, P = 0x2000E060u; host_pattern = P;
      EXPECT(fx_track_from_ctx(P + 0x5D50u + 0x88u) == 0, "P: MIDI-Spur 0");
      EXPECT(fx_track_from_ctx(P + 0x5D50u + 0xBA8u * 7u + 0x88u) == 7, "P: MIDI-Spur 7");
      EXPECT(fx_track_from_ctx(A + 0x5D50u + 0xBA8u * 3u + 0x88u) == 3, "A (vor P): MIDI-Spur 3");
      host_pattern = A;
      EXPECT(fx_track_from_ctx(P + 0x5D50u + 0xBA8u * 5u + 0x88u) == 5, "Zeiger auf A, Spur in P: 5");
      EXPECT(fx_track_from_ctx(A + 0x10u + 0x88u) == -1, "Audio-Spur = keine MIDI-Spur");
      EXPECT(fx_track_from_ctx(A + 0x5D50u + 0x88u + 4u) == -1, "krummer Zeiger");
      EXPECT(fx_track_from_ctx(0u) == -1, "ctx 0");
      host_pattern = 0u;
      EXPECT(fx_track_from_ctx(A + 0x5D50u + 0x88u) == -1, "kein Pattern");
      host_pattern = P; }

    /* Echo */
    reset_counts(); run(384); int d0 = ons[DRUM], s0 = ons[SYN];
    EXPECT(d0 == 4 && s0 == 8, "ohne Effekt: Drums %d, Synth %d", d0, s0);
    slot[13] = DELAY(1); reset_counts(); run(384);
    EXPECT(ons[DRUM] > d0 && ons[SYN] > s0, "Echo, nichts gewaehlt: wirkt auf beide (%d/%d)", ons[DRUM], ons[SYN]);
    clear_all();
    host_sel_raw = SEL(3); slot[13] = DELAY(1); reset_counts(); run(384);
    EXPECT(ons[DRUM] == d0, "Echo, Synth gewaehlt: Drums unveraendert (%d)", ons[DRUM]);
    EXPECT(ons[SYN] > s0, "Echo, Synth gewaehlt: Synth hat Echos (%d)", ons[SYN]);
    clear_all();
    host_sel_raw = SEL(0); slot[13] = DELAY(8); reset_counts(); run(384);
    EXPECT(ons[SYN] == s0 && pitch_seen[DRUM][48] > 0, "Echo Oct Up, Drums gewaehlt: nur Drums (%d, Oktave %d)", ons[SYN], pitch_seen[DRUM][48]);
    clear_all();
    host_sel_raw = 0x00FFu; slot[13] = DELAY(1); reset_counts(); run(384);
    EXPECT(ons[DRUM] > d0 && ons[SYN] > s0, "nur Audio-Spuren gewaehlt: MIDI wie ohne Auswahl");
    clear_all();
    host_sel_raw = SEL(3); slot[13] = DELAY(1); reset_counts();
    { midi_msg_t m = { 4, 9, 0, 70, 100 }, o = { 5, 9, 0, 70, 64 }; usb_send_hook((void *)1, &m); run(5); usb_send_hook((void *)1, &o); run(379); }
    EXPECT(ons[9] == 1, "Note ohne Spur (z. B. Keyboard) bei Auswahl: kein Echo, kommt durch (%d)", ons[9]);
    clear_all();

    /* Loop */
    while (now % 384u) run(1);
    run(200);                                            /* Pad neben der Taktzeit druecken */
    host_sel_raw = SEL(3); slot[7] = LOOP(2); reset_counts(); run(384);
    EXPECT(ons[DRUM] == d0, "Loop 2 Steps, Synth gewaehlt: Drums spielen normal (%d)", ons[DRUM]);
    EXPECT(ons[SYN] == 8, "Loop 2 Steps, Synth gewaehlt: Synth loopt alle 48 Ticks (%d)", ons[SYN]);
    slot[7] = LOOP(5); reset_counts(); run(384);
    EXPECT(ons[DRUM] == d0, "kurzer Loop: Drums weiter normal (%d)", ons[DRUM]);
    clear_all();
    while (now % 384u) run(1);
    run(200);                                            /* Pad neben der Taktzeit druecken */
    host_sel_raw = SEL(0); slot[7] = LOOP(3); reset_counts(); run(384);
    EXPECT(ons[SYN] == s0, "Loop, Drums gewaehlt: Synth spielt normal (%d)", ons[SYN]);
    clear_all();
    while (now % 384u) run(1);
    run(200);                                            /* Pad neben der Taktzeit druecken */
    slot[7] = LOOP(0); reset_counts(); run(384);
    EXPECT(ons[DRUM] == d0 && ons[SYN] == s0, "Loop 4 Steps, nichts gewaehlt: beide im Loop (%d/%d)", ons[DRUM], ons[SYN]);
    /* Auswahl waehrend des Haltens aendern */
    host_sel_raw = SEL(3); reset_counts(); run(384);
    EXPECT(ons[DRUM] == d0 && ons[SYN] >= 7, "Loop, Auswahl aendert sich: Drums wieder normal (%d/%d)", ons[DRUM], ons[SYN]);
    host_sel_raw = SEL(0); reset_counts(); run(384);
    EXPECT(ons[SYN] == s0, "Auswahl gewechselt: Synth wieder normal (%d)", ons[SYN]);
    clear_all();
    EXPECT(hanging() == 0, "nach Loop keine haengenden Noten");

    /* CC-Motion */
    run(200);
    host_sel_raw = SEL(3); reset_counts(); slot[14] = CCM(1); run(384);
    EXPECT(ccs[SYN] > 10 && ccs[DRUM] == 0, "CC Motion, Synth gewaehlt: nur Kanal 4 (%d/%d)", ccs[SYN], ccs[DRUM]);
    reset_counts(); host_sel_raw = SEL(0); run(96);
    EXPECT(ccs[DRUM] > 3 && ccs[SYN] == 1, "Auswahl wechselt: Kanal 4 einmal zurueckgesetzt, jetzt Kanal 1 (%d/%d)", ccs[DRUM], ccs[SYN]);
    reset_counts(); slot[14] = 0; run(10);
    EXPECT(ccs[DRUM] == 1 && ccs[SYN] == 0, "Pad los: Kanal 1 zurueckgesetzt (%d/%d)", ccs[DRUM], ccs[SYN]);
    host_sel_raw = 0; reset_counts(); slot[14] = CCM(1); run(192);
    EXPECT(ccs[DRUM] > 3 && ccs[SYN] > 3, "CC Motion, nichts gewaehlt: beide (%d/%d)", ccs[DRUM], ccs[SYN]);
    clear_all();

    /* Zufall: Pads und Auswahl wild wechseln, keine haengenden Noten */
    srand(1);
    for (int i = 0; i < 40000; i++) {
        int r = rand() % 100;
        if (r < 8) slot[13] = (rand() % 3) ? 0u : DELAY(rand() % 16);
        else if (r < 16) slot[7] = (rand() % 3) ? 0u : LOOP(rand() % 16);
        else if (r < 24) slot[14] = (rand() % 3) ? 0u : CCM(rand() % 16);
        else if (r < 40) host_sel_raw = (uint32_t)(rand() & 0xFFFF);
        run((uint32_t)(rand() % 40 + 1));
    }
    clear_all();
    EXPECT(hanging() == 0, "Zufallstest: keine haengenden Noten");
    EXPECT(bad_vel == 0, "Zufallstest: gueltige Velocity");
    (void)running;
    printf("GESAMT: %s\n", fails ? "FEHLER" : "alles OK");
    return fails != 0;
}
