/* PC-Testlauf fuer fx.c: simulierter Sequencer, Attrappen fuer Firmware-Funktionen */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef struct { uint32_t type; uint8_t ch, pad, d1, d2; } midi_msg_t;
void usb_send_hook(void *self, const midi_msg_t *m);
void tick_hook(void *queue, int ticks);

uint8_t host_ram[16384] __attribute__((aligned(8)));
uint32_t host_perform_page = 0x10000000u;
static uint32_t slot[16];
uint32_t host_read32(uint32_t a) {
    if (a >= 0x10000064u && a < 0x10000064u + 64u) return slot[(a - 0x10000064u) / 4u];
    fprintf(stderr, "unerwarteter Lesezugriff %#x\n", a); exit(1);
}
static uint32_t now = 0; static int on_count[2][16][128];
static int log_on = 1;
typedef struct { uint32_t t; int on; int note; int vel; } rec_t; static rec_t rec[4096]; static int nrec = 0;
uint32_t host_pending(void); int host_lfo_pad(void);
typedef struct { uint32_t t; int ch, cc, val; } ccrec_t; static ccrec_t ccr[8192]; static int nccr = 0;
void host_usb(void *self, const midi_msg_t *m) {
    (void)self;
    if (m->type == 4 && m->d2) on_count[0][m->ch][m->d1]++;
    if (m->type == 5 || (m->type == 4 && !m->d2)) on_count[0][m->ch][m->d1]--;
    if (m->type == 6 && m->d1 != 123 && nccr < 8192) { ccr[nccr].t = now; ccr[nccr].ch = m->ch; ccr[nccr].cc = m->d1; ccr[nccr].val = m->d2; nccr++; }
    if ((m->type == 4 || m->type == 5) && nrec < 4096) { rec[nrec].t = now; rec[nrec].on = (m->type==4 && m->d2); rec[nrec].note = m->d1; rec[nrec].vel = m->d2; nrec++; }
    if (log_on && (m->type == 4 || m->type == 5))
        printf("  t=%4u %s %3u v%3u\n", now, m->type == 4 ? "ON " : "off", m->d1, m->d2);
}
void host_din(void *self, const midi_msg_t *m) { (void)self; (void)m; }
/* Sequencer: Pattern 16 Steps, Step = 24 Ticks; E5 auf Step 0, C4 auf Step 8; Note-Off nach 12 Ticks */
static int pending_off[128];
static int seq_running = 1;
void host_tick(void *q, int t) {
    (void)q;
    for (int k = 0; k < t; k++) {
        if (!seq_running) continue;
        for (int n = 0; n < 128; n++) if (pending_off[n] && --pending_off[n] == 0) {
            midi_msg_t m = {5, 0, 0, (uint8_t)n, 64}; usb_send_hook((void*)1, &m);
        }
        uint32_t tt = now + k;
        if (tt % 24 == 0) {
            int step = (tt / 24) % 16;
            int note = step == 0 ? 76 : step == 8 ? 60 : step == 4 ? 64 : step == 12 ? 67 : -1;
            if (note >= 0) { midi_msg_t m = {4, 0, 0, (uint8_t)note, 100}; usb_send_hook((void*)1, &m); pending_off[note] = 12; }
        }
    }
}
static void run(uint32_t ticks) { for (uint32_t i = 0; i < ticks; i++) { tick_hook(0, 1); now++; } }
static void stop(void) {
    seq_running = 0;
    for (int n = 0; n < 128; n++) if (pending_off[n]) { midi_msg_t m = {5,0,0,(uint8_t)n,64}; usb_send_hook((void*)1,&m); pending_off[n]=0; }
    midi_msg_t m = {6, 0, 0, 123, 0}; usb_send_hook((void*)1, &m);
}
static int hanging(void) { int h = 0; for (int n = 0; n < 128; n++) if (on_count[0][0][n] > 0) { printf("  HAENGT: Note %d (%d)\n", n, on_count[0][0][n]); h++; } return h; }

int main(void) {
    memset(host_ram, 0xA5, sizeof host_ram);   /* Zufallsinhalt wie nach Power-On */
    printf("== 1) ohne Pads, 1 Takt (Pattern: E5@0, E4@4, C4@8, G4@12)\n"); run(384);
    printf("== 2) Delay-Pad L3 (Echo 3/16) aktiv, 1 Takt\n");
    slot[13] = 0x000A86B4u + 20u*2u; run(384);
    printf("== 3) Delay aus, Loop-Pad '2 Steps' (48 Ticks) ab t=%u fuer 1 Takt\n", now + 10);
    slot[13] = 0; run(10); slot[7] = 0x000A8434u + 20u*2u; run(384);
    printf("== 4) Loop-Pad LR-Variante '2 Steps LR' (rueckwaerts)\n");
    slot[7] = 0; run(30); slot[7] = 0x000A8434u + 20u*10u; run(192);
    printf("== 5) Loop + Echo 'Oct Up' zusammen\n");
    slot[13] = 0x000A86B4u + 20u*8u; run(192);
    printf("== 6a) CC-Motion: Play sendet CC74=90, dann Pad 'Wobble 1/8' 2 Takte, dann loslassen\n");
    slot[13] = 0; slot[7] = 0; run(40);
    { midi_msg_t m = {6, 0, 0, 74, 90}; usb_send_hook((void*)1, &m); }
    log_on = 0; uint32_t lfo_t0 = now; slot[14] = 0x000A8E34u + 20u*1u; run(768); slot[14] = 0; run(10); log_on = 1;
    printf("== 7) Loop lang (4 Steps) → kurz (1/8 Step) → wieder lang: Original muss zurueckkommen\n");
    slot[13] = 0; slot[7] = 0; run(40);
    run((384u + 100u - now % 384u) % 384u);            /* Fenster [t-96, t) enthaelt E4 (64) bei rel 92 */
    log_on = 0; slot[7] = 0x000A8434u + 20u*0u; run(200);
    slot[7] = 0x000A8434u + 20u*7u; run(150);          /* 1/8 Step: Fenster ohne Note → still (laenger als 96 Ticks) */
    slot[7] = 0x000A8434u + 20u*0u; uint32_t t7 = now; run(300); slot[7] = 0; run(10); log_on = 1;
    printf("== 8) Classic (Patch 017): Delay-, Loop- und Reverb-Pad ohne Wirkung\n");
    extern int host_creative;
    slot[13] = 0; slot[7] = 0; slot[14] = 0; run(40);
    log_on = 0; host_creative = 0; uint32_t t8 = now; int n8 = nrec;
    slot[13] = 0x000A86B4u + 20u*8u; slot[7] = 0x000A8434u + 20u*2u; slot[14] = 0x000A8E34u + 20u*1u; run(384);
    slot[13] = 0; slot[7] = 0; slot[14] = 0; host_creative = 1; run(10); log_on = 1;
    printf("== 6) alles aus, Stopp, Nachlauf\n");
    slot[13] = 0; slot[7] = 0; log_on = 0; stop(); run(600); log_on = 1;
    int h = hanging(); printf("haengende Noten: %d | geplante Events uebrig: ", h);
    printf("%u\n", host_pending());
    int fail = h != 0 || host_pending() != 0;
    /* Pruefung A: Echo 3/16 (Szenario 2): E5 bei t=384 → Echos bei 456/528/600 mit v60/36/21 */
    int exp_t[3] = {456, 528, 600}, exp_v[3] = {60, 36, 21};
    for (int k = 0; k < 3; k++) { int ok = 0; for (int i = 0; i < nrec; i++) if (rec[i].on && rec[i].note == 76 && rec[i].t == (uint32_t)exp_t[k] && rec[i].vel == exp_v[k]) ok = 1;
        printf("Pruefung Echo %d (t=%d v=%d): %s\n", k+1, exp_t[k], exp_v[k], ok ? "OK" : "FEHLER"); fail |= !ok; }
    /* Pruefung B: Loop-Perioden – im Loop-Szenario 3 muessen ON-Ereignisse von 76 exakt alle 48 Ticks kommen */
    { uint32_t last = 0; int n = 0, bad = 0;
      for (int i = 0; i < nrec; i++) if (rec[i].on && rec[i].note == 76 && rec[i].t > 780 && rec[i].t < 1160 && rec[i].vel == 100) { if (n && rec[i].t - last != 48) bad++; last = rec[i].t; n++; }
      printf("Pruefung Loop-Periode (%d Wiederholungen): %s\n", n, (n >= 6 && !bad) ? "OK" : "FEHLER"); fail |= (n < 6 || bad); }
    /* Pruefung C: LR-Loop (Szenario 4) muss Noten spielen */
    { int n = 0; for (int i = 0; i < nrec; i++) if (rec[i].on && rec[i].t >= 1192 && rec[i].t < 1384) n++;
      printf("Pruefung LR-Loop spielt Noten (%d): %s\n", n, n > 0 ? "OK" : "FEHLER"); fail |= (n == 0); }
    /* Pruefung D: Loop+OctUp (Szenario 5) muss Noten mit +12 enthalten */
    { int n = 0; for (int i = 0; i < nrec; i++) if (rec[i].on && rec[i].t >= 1384 && rec[i].t < 1600 && rec[i].note == 88) n++;
      printf("Pruefung Loop+Oct-Up (E6 erzeugt: %d): %s\n", n, n > 0 ? "OK" : "FEHLER"); fail |= (n == 0); }
    /* Pruefung E: CC-Motion – Werte im Bereich 20..120, Minimum und Maximum erreicht, Periode 48 Ticks, Wiederherstellung auf 90 */
    { int n = 0, lo = 999, hi = -1, okr = 1, restore = -1; uint32_t tmax[64]; int nmax = 0;
      for (int i = 0; i < nccr; i++) if (ccr[i].cc == 74 && ccr[i].t >= lfo_t0) {
          if (ccr[i].t < lfo_t0 + 768) { n++; if (ccr[i].val < lo) lo = ccr[i].val; if (ccr[i].val > hi) hi = ccr[i].val;
              if (ccr[i].val < 20 || ccr[i].val > 120) okr = 0; if (ccr[i].val == 20 && nmax < 64) tmax[nmax++] = ccr[i].t; }
          else restore = ccr[i].val; }
      int per_ok = nmax >= 3; for (int i = 1; i < nmax; i++) if (tmax[i] - tmax[i-1] != 48) per_ok = 0;
      printf("Pruefung CC-Motion: %d Werte, Bereich %d..%d, Periode %s, Wiederherstellung=%d: %s\n", n, lo, hi, per_ok ? "48" : "FALSCH", restore,
             (okr && lo == 20 && hi == 120 && per_ok && restore == 90 && n < 400) ? "OK" : "FEHLER");
      fail |= !(okr && lo == 20 && hi == 120 && per_ok && restore == 90 && n < 400); }
    /* Pruefung F: nach lang→kurz→lang spielt der lange Loop wieder E4 (64) alle 96 Ticks */
    { int n = 0, bad = 0; uint32_t last = 0;
      for (int i = 0; i < nrec; i++) if (rec[i].on && rec[i].note == 64 && rec[i].t >= t7 && rec[i].t < t7 + 300) { if (n && rec[i].t - last != 96) bad++; last = rec[i].t; n++; }
      printf("Pruefung Loop lang→kurz→lang (%d x E4, Abstand 96): %s\n", n, (n >= 3 && !bad) ? "OK" : "FEHLER"); fail |= (n < 3 || bad); }
    /* Pruefung G: Classic – im Fenster nur die Pattern-Noten (4 pro Takt, nichts transponiert), keine CCs */
    { int n = 0, odd = 0, cc = 0;
      for (int i = n8; i < nrec; i++) if (rec[i].on && rec[i].t >= t8 && rec[i].t < t8 + 384) { n++; if (rec[i].note != 76 && rec[i].note != 64 && rec[i].note != 60 && rec[i].note != 67) odd++; }
      for (int i = 0; i < nccr; i++) if (ccr[i].t >= t8 && ccr[i].t < t8 + 384) cc++;
      printf("Pruefung Classic: %d Noten (erwartet 4), fremde %d, CCs %d: %s\n", n, odd, cc, (n == 4 && !odd && !cc) ? "OK" : "FEHLER");
      fail |= !(n == 4 && !odd && !cc); }
    printf("GESAMT: %s\n", fail ? "FEHLER" : "alles OK");
    return fail;
}
