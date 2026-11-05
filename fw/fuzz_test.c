/* Zufallstest: zufaellige Noten (inkl. Live-Noten), Pad-Wechsel, Tick-Schritte 1..4, Stopps */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef struct { uint32_t type; uint8_t ch, pad, d1, d2; } midi_msg_t;
void usb_send_hook(void *self, const midi_msg_t *m);
void din_send_hook(void *self, const midi_msg_t *m);
void tick_hook(void *queue, int ticks);
uint32_t host_pending(void);
uint8_t host_ram[16384] __attribute__((aligned(8)));
uint32_t host_perform_page = 0x10000000u;
static uint32_t slot[16];
uint32_t host_read32(uint32_t a) { return slot[(a - 0x10000064u) / 4u]; }
static int bal[2][16][128]; static long sent = 0; static long gtick = 0;
typedef struct { long t; int port, ch, note, on, src; } tr_t; static tr_t tr[64]; static int ntr = 0; static int WP=-1,WC=-1,WN=-1; static int cur_src = 0;
static void acc(int port, const midi_msg_t *m) {
    sent++;
    if (port==WP && m->ch==WC && m->d1==WN && (m->type==4||m->type==5)) { tr_t *x=&tr[ntr%64]; x->t=gtick; x->on=(m->type==4&&m->d2); x->src=cur_src; ntr++; }
    if (m->type==6 && m->d1==123 && port==WP && m->ch==WC) { tr_t *x=&tr[ntr%64]; x->t=gtick; x->on=9; x->src=cur_src; ntr++; }
    if (m->type == 4 && m->d2) bal[port][m->ch][m->d1]++;
    else if (m->type == 5 || (m->type == 4 && !m->d2)) { if (bal[port][m->ch][m->d1] > 0) bal[port][m->ch][m->d1]--; }
    else if (m->type == 6 && m->d1 == 123) {}                 /* All Notes Off zaehlt NICHT als Ausschalten */
}
void host_usb(void *s, const midi_msg_t *m) { (void)s; acc(0, m); }
void host_din(void *s, const midi_msg_t *m) { (void)s; acc(1, m); }
static int pend[2][16][128]; static int gen_on = 1;
static void seq_tick(void) {
    for (int p = 0; p < 2; p++) for (int c = 0; c < 4; c++) for (int n = 0; n < 128; n++)
        if (pend[p][c][n] && --pend[p][c][n] == 0) { midi_msg_t m = {5,(uint8_t)c,0,(uint8_t)n,64}; (p?din_send_hook:usb_send_hook)((void*)1,&m); }
    for (int k = 0; k < 3; k++) if (gen_on && rand() % 6 == 0) {
        int p = rand() % 2, c = rand() % 4, n = 20 + rand() % 90;
        if (pend[p][c][n]) continue;
        midi_msg_t m = {4,(uint8_t)c,0,(uint8_t)n,(uint8_t)(1 + rand() % 127)}; (p?din_send_hook:usb_send_hook)((void*)1,&m);
        pend[p][c][n] = 1 + rand() % 40;
    }
}
static int pending_ticks;
void host_tick(void *q, int t) { (void)q; cur_src = 1; for (int i = 0; i < t; i++) seq_tick(); cur_src = 0; pending_ticks = t; }
int main(void) {
    srand(8); memset(host_ram, 0x5A, sizeof host_ram);
    for (long step = 0; step < 300000; step++) {
        if (rand() % 400 == 0) slot[13] = (rand() % 3) ? 0x000A86B4u + 20u * (rand() % 16) : 0;
        if (rand() % 500 == 0) slot[7] = (rand() % 2) ? 0x000A8434u + 20u * (rand() % 16) : 0;
        if (rand() % 450 == 0) slot[14] = (rand() % 2) ? 0x000A8E34u + 20u * (rand() % 16) : 0;
        if (rand() % 300 == 0) { midi_msg_t m = {6,(uint8_t)(rand()%4),0,(uint8_t)((rand()%2)?74:1),(uint8_t)(rand()%128)}; usb_send_hook((void*)1,&m); }
        if (rand() % 50000 == 0) { midi_msg_t m = {6,0,0,123,0}; usb_send_hook((void*)1,&m); din_send_hook((void*)1,&m); }
        { int tt = 1 + rand() % 4; tick_hook(0, tt); gtick += tt; }
    }
    /* Ende: Pads aus, Sequencer-Noten beenden, Nachlauf */
    slot[13] = slot[7] = slot[14] = 0; gen_on = 0;
    for (int i = 0; i < 2000; i++) { tick_hook(0, 1); }
    for (int p = 0; p < 2; p++) for (int c = 0; c < 4; c++) for (int n = 0; n < 128; n++) if (pend[p][c][n]) { midi_msg_t m={5,(uint8_t)c,0,(uint8_t)n,64}; (p?din_send_hook:usb_send_hook)((void*)1,&m); pend[p][c][n]=0; }
    for (int i = 0; i < 2000; i++) tick_hook(0, 1);
    int hang = 0; for (int p = 0; p < 2; p++) for (int c = 0; c < 16; c++) for (int n = 0; n < 128; n++) { if (bal[p][c][n] && WP<0) { printf("haengt: port %d ch %d note %d\n",p,c,n); } hang += bal[p][c][n]; }
    for (int i = (ntr>16?ntr-16:0); i < ntr; i++) { tr_t *x=&tr[i%64]; printf("  t=%ld %s %s\n", x->t, x->on==9?"CC123":x->on ? "ON " : "off", x->src ? "Sequencer" : "Effekt"); }
    printf("gesendet: %ld Nachrichten | haengende Noten: %d | Events uebrig: %u\n", sent, hang, host_pending());
    return hang || host_pending();
}
