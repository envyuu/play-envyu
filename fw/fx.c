/* Patch 009–014 (008 + Loop-Slot-Fix; ab 014 mit Arp-Anbindung) – MIDI FX Suite: Echo (Delay-Pads), Loop (Loop-Pads), CC-Motion (Reverb-Pads), MIDI-Perform-Seite
 * Polyend Play Rev A, Firmware 1.6.0
 *
 * Perform-Seite X = *(0x1FFF69B8); Slot k = X + 0x64 + 4k (Zeiger auf aktiven Preset-Eintrag).
 *   Slot 13 = Delay (MIDI) → Eintraege 0xA86B4 + 20*k
 *   Slot 7  = Loop  (Audio+MIDI gemeinsam; Pad-Handler 0x22598 legt Kat. 15 auf Slot 7) → Eintraege 0xA8434 + 20*k
 *   Slot 14 = Reverb(MIDI) → Eintraege 0xA8E34 + 20*k  (CC-Motion)
 * Hooks: USB-Senden 0x9AD40 (0x5D34), DIN-Senden 0x9AA8C (0x42E0), Tick 3x BL → 0x4B21C.
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt.
 */
#include <stdint.h>

typedef struct { uint32_t type; uint8_t ch, pad, d1, d2; } midi_msg_t;
typedef void (*send_fn)(void *self, const midi_msg_t *m);
typedef void (*tick_fn)(void *queue, int ticks);

#ifdef HOST_TEST
extern void host_usb(void *self, const midi_msg_t *m);
extern void host_din(void *self, const midi_msg_t *m);
extern void host_tick(void *q, int t);
extern uint32_t host_perform_page;
extern uint8_t host_ram[];
#define ORIG_USB  host_usb
#define ORIG_DIN  host_din
#define ORIG_TICK host_tick
extern uint32_t host_read32(uint32_t a);
#define READ32(a) host_read32((uint32_t)(a))
#define PERFORM_PAGE_ADDR() (host_perform_page)
#define STATE_BASE ((uintptr_t)host_ram)
#define SNAP_BASE ((uintptr_t)host_ram + 8192u)
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t p) { (void)p; }
#define RAM_LO 0u
#define RAM_HI 0xFFFFFFFFu
__attribute__((weak)) int host_creative = 1;
#define CREATIVE() (host_creative != 0)
#else
#define ORIG_USB  ((send_fn)0x00005D35u)
#define ORIG_DIN  ((send_fn)0x000042E1u)
#define ORIG_TICK ((tick_fn)0x0004B21Du)
#define READ32(a) (*(volatile uint32_t *)(a))
#define PERFORM_PAGE_ADDR() READ32(0x1FFF69B8u)
#define STATE_BASE 0x1FFFA000u
#define SNAP_BASE  0x1FFFBC00u          /* Loop-Schnappschuss (Patch 014), 516 Byte */
#define RAM_LO 0x1FFF0000u
#define RAM_HI 0x2002FF00u
/* Patch 017: Menue "FX Mode" (usbmenu.c, um_t bei 0x1FFFD300: magic, v[3] = creative @ +0x10); ohne gueltiges um_t = Creative */
#define CREATIVE() (*(volatile uint32_t *)0x1FFFD300u != 0x55AEC017u || *(volatile int32_t *)0x1FFFD310u != 0)
static inline uint32_t irq_save(void) {
    uint32_t p; __asm volatile ("mrs %0, primask\n cpsid i" : "=r"(p) :: "memory"); return p;
}
static inline void irq_restore(uint32_t p) { __asm volatile ("msr primask, %0" :: "r"(p) : "memory"); }
#endif

#define SLOT_ADDR(x,k)   ((x) + 0x64u + 4u*(uint32_t)(k))
#define DELAY_MIDI_FIRST 0x000A86B4u
#define LOOP_FIRST       0x000A8434u
#define REVERB_MIDI_FIRST 0x000A8E34u
#define ENTRY_SIZE       20u

/* ---------- Echo-Varianten ---------- */
typedef struct { uint16_t delay; uint8_t repeats, vel_pct; int8_t pitch; uint8_t alt; } echo_cfg_t;
static const echo_cfg_t CFG[16] = {
    {  24, 3,  70,   0, 0 }, {  48, 3,  65,   0, 0 }, {  72, 3,  60,   0, 0 }, {  96, 3,  60,   0, 0 },
    {  32, 4,  65,   0, 0 }, {  16, 4,  70,   0, 0 }, {  12, 6,  85,   0, 0 }, { 144, 2,  55,   0, 0 },
    {  48, 3,  70,  12, 0 }, {  48, 3,  70, -12, 0 }, {  72, 4,  70,   7, 0 }, {  24, 4,  80,  12, 1 },
    {  24, 6,  85,   1, 0 }, {  24, 6,  85,  -1, 0 }, {  48, 4, 125,   0, 0 }, {  24, 8,  80,   5, 0 },
};
/* Loop-Laengen in Ticks fuer Pad 0..7 (LR-Varianten 8..15 = gleiche Laenge, rueckwaerts) */
static const uint8_t LOOP_LEN[8] = { 96, 72, 48, 24, 12, 8, 6, 3 };

/* CC-Motion (Reverb-Reihe): Ziel-CC, Form, Periode in Ticks, Min, Max */
enum { SH_TRI = 0, SH_SAWUP, SH_SAWDN, SH_SQUARE, SH_SH, SH_ONCE };
typedef struct { uint8_t cc, shape; uint16_t period; uint8_t lo, hi; } lfo_cfg_t;
static const lfo_cfg_t LFO[16] = {
    /* linke Spalte: Filter Cutoff (CC74) */
    { 74, SH_TRI,     24,  20, 120 },   /* 0  Wobble 1/16   */
    { 74, SH_TRI,     48,  20, 120 },   /* 1  Wobble 1/8    */
    { 74, SH_TRI,     96,  10, 127 },   /* 2  Wobble 1/4    */
    { 74, SH_TRI,    192,  10, 127 },   /* 3  Wobble 1/2    */
    { 74, SH_SAWUP,  384,   0, 127 },   /* 4  Sweep Up 1 Bar   */
    { 74, SH_SAWDN,  384,   0, 127 },   /* 5  Sweep Down 1 Bar */
    { 74, SH_SH,      24,  10, 127 },   /* 6  S&H 1/16      */
    { 74, SH_SH,      48,  10, 127 },   /* 7  S&H 1/8       */
    /* rechte Spalte: weitere Ziele */
    {  1, SH_TRI,     96,   0, 127 },   /* 8  ModWheel LFO 1/4 */
    {  1, SH_ONCE,   768,   0, 127 },   /* 9  ModWheel Swell 2 Bars */
    { 10, SH_SQUARE,  48,   0, 127 },   /* 10 Pan PingPong 1/8 */
    { 10, SH_TRI,    384,   0, 127 },   /* 11 Auto Pan 1 Bar   */
    { 71, SH_TRI,     96,  10, 110 },   /* 12 Reso Wobble 1/4  */
    { 74, SH_SQUARE,  12,  15, 127 },   /* 13 Filter Gate 1/16 */
    {  7, SH_TRI,     48,  40, 127 },   /* 14 Tremolo 1/8      */
    { 71, SH_SH,      48,  10, 110 },   /* 15 Reso S&H 1/8     */
};
/* beobachtete CCs (fuer Wiederherstellung) und neutrale Standardwerte */
static const uint8_t WATCH_CC[5]  = { 74, 1, 10, 71, 7 };
static const uint8_t WATCH_DEF[5] = { 127, 0, 64, 0, 100 };

#ifndef HOST_TEST
__attribute__((used)) const char NAME00[] = "Echo 1/16";
__attribute__((used)) const char NAME01[] = "Echo 1/8";
__attribute__((used)) const char NAME02[] = "Echo 3/16";
__attribute__((used)) const char NAME03[] = "Echo 1/4";
__attribute__((used)) const char NAME04[] = "Echo 1/8T";
__attribute__((used)) const char NAME05[] = "Echo 1/16T";
__attribute__((used)) const char NAME06[] = "Roll 1/32";
__attribute__((used)) const char NAME07[] = "Echo 3/8";
__attribute__((used)) const char NAME08[] = "Oct Up";
__attribute__((used)) const char NAME09[] = "Oct Down";
__attribute__((used)) const char NAME10[] = "Fifths Up";
__attribute__((used)) const char NAME11[] = "Oct PingPong";
__attribute__((used)) const char NAME12[] = "Chroma Up";
__attribute__((used)) const char NAME13[] = "Chroma Down";
__attribute__((used)) const char NAME14[] = "Swell 1/8";
__attribute__((used)) const char NAME15[] = "Fourths Arp";
__attribute__((used)) const char RNAME00[] = "Wobble 1/16";
__attribute__((used)) const char RNAME01[] = "Wobble 1/8";
__attribute__((used)) const char RNAME02[] = "Wobble 1/4";
__attribute__((used)) const char RNAME03[] = "Wobble 1/2";
__attribute__((used)) const char RNAME04[] = "Sweep Up";
__attribute__((used)) const char RNAME05[] = "Sweep Down";
__attribute__((used)) const char RNAME06[] = "S&H 1/16";
__attribute__((used)) const char RNAME07[] = "S&H 1/8";
__attribute__((used)) const char RNAME08[] = "ModWheel LFO";
__attribute__((used)) const char RNAME09[] = "ModWheel Swell";
__attribute__((used)) const char RNAME10[] = "Pan PingPong";
__attribute__((used)) const char RNAME11[] = "Auto Pan";
__attribute__((used)) const char RNAME12[] = "Reso Wobble";
__attribute__((used)) const char RNAME13[] = "Filter Gate";
__attribute__((used)) const char RNAME14[] = "Tremolo 1/8";
__attribute__((used)) const char RNAME15[] = "Reso S&H";
/* Patch 017: Namen fuer den Umschalter Classic/Creative (usbmenu.c kopiert sie in die RAM-Puffer der Namenseintraege) */
const char *const FX_NAMES[32] = {
    NAME00, NAME01, NAME02, NAME03, NAME04, NAME05, NAME06, NAME07, NAME08, NAME09, NAME10, NAME11, NAME12, NAME13, NAME14, NAME15,
    RNAME00, RNAME01, RNAME02, RNAME03, RNAME04, RNAME05, RNAME06, RNAME07, RNAME08, RNAME09, RNAME10, RNAME11, RNAME12, RNAME13, RNAME14, RNAME15 };
#endif

/* ---------- Zustand ---------- */
typedef struct { int32_t ticks; uint8_t type, ch, note, vel; uint8_t port, _r[3]; } ev_t;     /* geplante Sendungen */
typedef struct { uint32_t t;    uint8_t type, ch, note, vel; uint8_t port, _r[3]; } hist_t;   /* Historie */
typedef struct { uint16_t rel, dur; uint8_t ch, note, vel, port; } lnote_t;                     /* Loop-Inhalt */

#define MAXEV   128
#define MAXHIST 128
#define MAXLOOP 64
#define MAGIC   0xEC40C0D8u

typedef struct {
    uint32_t magic;
    void    *port[2];
    uint32_t now;                 /* globaler Tick-Zaehler */
    uint32_t n;                   /* Anzahl geplanter Events */
    uint32_t hpos, hcount;        /* Ringpuffer-Historie */
    int32_t  loop_pad;            /* -1 = aus */
    uint32_t loop_start, loop_len, loop_n, loop_pos;
    int32_t  lfo_pad;             /* -1 = aus */
    uint32_t lfo_start, lfo_rand, lfo_last_tick;
    int32_t  lfo_val;             /* zuletzt gesendeter Wert, -1 = noch keiner */
    uint16_t lfo_sent[2];         /* Kanaele, an die CC-Motion gesendet hat (Bitmaske je Port) */
    uint32_t lastnote[2][16];     /* Zeit der letzten Note-On je Port/Kanal (+1, 0 = nie) */
    uint8_t  ccval[2][16][5];     /* zuletzt vom Play gesendete Werte der beobachteten CCs (0xFF = unbekannt) */
    ev_t     ev[MAXEV];
    hist_t   hist[MAXHIST];
    lnote_t  loop[MAXLOOP];
} state_t;
#define ST ((volatile state_t *)STATE_BASE)

static void ensure_init(void) {
    if (ST->magic != MAGIC) {
        ST->port[0] = 0; ST->port[1] = 0; ST->now = 0; ST->n = 0;
        ST->hpos = 0; ST->hcount = 0; ST->loop_pad = -1; ST->loop_n = 0; ST->loop_len = 0; ST->loop_pos = 0;
        ST->lfo_pad = -1; ST->lfo_start = 0; ST->lfo_rand = 0x1234567u; ST->lfo_last_tick = 0; ST->lfo_val = -1;
        ST->lfo_sent[0] = ST->lfo_sent[1] = 0;
        for (int p = 0; p < 2; p++) for (int c = 0; c < 16; c++) {
            ST->lastnote[p][c] = 0;
            for (int w = 0; w < 5; w++) ST->ccval[p][c][w] = 0xFF;
        }
        ST->magic = MAGIC;
    }
}

static int active_entry(uint32_t slot, uint32_t first) {
    if (!CREATIVE()) return -1;                                 /* 017: Classic = Original-Perform-FX (keine MIDI-Effekte) */
    uint32_t x = PERFORM_PAGE_ADDR();
    if (x < RAM_LO || x >= RAM_HI || (x & 3u)) return -1;
    uint32_t e = READ32(SLOT_ADDR(x, slot));
    if (e < first) return -1;
    uint32_t off = e - first;
    if (off >= 16u * ENTRY_SIZE || (off % ENTRY_SIZE) != 0u) return -1;
    return (int)(off / ENTRY_SIZE);
}

static void push(int32_t ticks, uint8_t type, uint8_t ch, uint8_t note, uint8_t vel, uint8_t port) {
    if (ST->n >= MAXEV) return;
    volatile ev_t *e = &ST->ev[ST->n++];
    e->ticks = ticks; e->type = type; e->ch = ch; e->note = note; e->vel = vel; e->port = port;
}

/* Echos fuer eine Note-On planen (falls Delay-Pad aktiv). Aufruf unter Sperre. */
static void plan_echo(uint8_t port, uint8_t ch, uint8_t note0, uint8_t vel0) {
    int k = active_entry(13, DELAY_MIDI_FIRST);
    if (k < 0) return;
    const echo_cfg_t *c = &CFG[k];
    int32_t v = vel0, note = note0, step = c->pitch;
    uint16_t gate = c->delay / 2u; if (gate < 6u) gate = 6u;
    for (int r = 1; r <= c->repeats; r++) {
        v = v * c->vel_pct / 100; if (v < 1) v = 1; if (v > 127) v = 127;
        note += step; if (c->alt) step = -step;
        if (note < 0 || note > 127) break;
        if (ST->n + 2u > MAXEV) break;                          /* nur paarweise (On+Off) planen */
        push((int32_t)r * c->delay,        4, ch, (uint8_t)note, (uint8_t)v, port);
        push((int32_t)r * c->delay + gate, 5, ch, (uint8_t)note, 64,         port);
    }
}

static void hist_add(uint8_t type, uint8_t ch, uint8_t note, uint8_t vel, uint8_t port) {
    volatile hist_t *h = &ST->hist[ST->hpos];
    h->t = ST->now; h->type = type; h->ch = ch; h->note = note; h->vel = vel; h->port = port;
    ST->hpos = (ST->hpos + 1u) % MAXHIST;
    if (ST->hcount < MAXHIST) ST->hcount++;
}

/* Loop (ab Patch 014): beim ERSTEN Loop-Pad wird ein Schnappschuss der letzten 4 Steps (96 Ticks) gespeichert.
   Jeder Pad-Wechsel schneidet seinen Loop aus diesem Original (nicht mehr aus dem gerade Gehoerten),
   damit man von kurz wieder auf lang wechseln kann. */
#define SNAPLEN 96u
typedef struct { uint32_t n; lnote_t note[MAXLOOP]; } snap_t;
#define SNAP ((volatile snap_t *)SNAP_BASE)

static void loop_snapshot(void) {                                  /* Fenster [now-96, now). Aufruf unter Sperre. */
    uint32_t start = ST->now - SNAPLEN, n = 0;
    for (uint32_t i = 0; i < ST->hcount && n < MAXLOOP; i++) {
        uint32_t idx = (ST->hpos + MAXHIST - ST->hcount + i) % MAXHIST;  /* aelteste zuerst */
        volatile hist_t *h = &ST->hist[idx];
        if (h->type != 4u) continue;
        uint32_t age = ST->now - h->t;
        if (age == 0u || age > SNAPLEN) continue;
        uint32_t dur = 0;                                          /* Dauer: passendes Note-Off suchen (0 = unbekannt) */
        for (uint32_t j = i + 1; j < ST->hcount; j++) {
            uint32_t jdx = (ST->hpos + MAXHIST - ST->hcount + j) % MAXHIST;
            volatile hist_t *o = &ST->hist[jdx];
            if (o->type == 5u && o->ch == h->ch && o->note == h->note && o->port == h->port) { dur = o->t - h->t; break; }
        }
        volatile lnote_t *l = &SNAP->note[n++];
        l->rel = (uint16_t)(h->t - start); l->dur = (uint16_t)(dur > 0xFFFFu ? 0xFFFFu : dur);
        l->ch = h->ch; l->note = h->note; l->vel = h->vel; l->port = h->port;
    }
    SNAP->n = n;
}

static void loop_capture(int pad) {                                /* aus dem Schnappschuss. Aufruf unter Sperre. */
    uint32_t len = LOOP_LEN[pad & 7], rev = (pad >= 8), n = 0, skip = SNAPLEN - len;
    for (uint32_t k = 0; k < SNAP->n && k < MAXLOOP; k++) {
        volatile lnote_t *s = &SNAP->note[k];
        if (s->rel < skip) continue;                               /* nur das Ende des Originals (die letzten len Ticks) */
        uint32_t rel = s->rel - skip, dur = s->dur;
        if (dur == 0u || dur >= len) dur = (len > 2u) ? len / 2u : 1u;
        if (rev) rel = (len - 1u - rel);
        volatile lnote_t *l = &ST->loop[n++];
        l->rel = (uint16_t)rel; l->dur = (uint16_t)dur; l->ch = s->ch; l->note = s->note; l->vel = s->vel; l->port = s->port;
    }
    ST->loop_n = n; ST->loop_len = len; ST->loop_start = ST->now; ST->loop_pos = 0; ST->loop_pad = pad;
}

/* CC-Motion: Wert zum Zeitpunkt now berechnen */
static int lfo_value(const lfo_cfg_t *c, uint32_t elapsed) {
    uint32_t per = c->period, ph = elapsed % per, span = (uint32_t)(c->hi - c->lo);
    switch (c->shape) {
    case SH_TRI:    { uint32_t h = per / 2u; uint32_t x = ph < h ? ph : per - ph; return c->lo + (int)(span * x / (h ? h : 1u)); }
    case SH_SAWUP:  return c->lo + (int)(span * ph / per);
    case SH_SAWDN:  return c->hi - (int)(span * ph / per);
    case SH_SQUARE: return (ph < per / 2u) ? c->hi : c->lo;
    case SH_SH:     if (ph == 0u || ST->lfo_val < 0) { ST->lfo_rand = ST->lfo_rand * 1664525u + 1013904223u;
                        return c->lo + (int)((ST->lfo_rand >> 16) % (span + 1u)); }
                    return ST->lfo_val;
    case SH_ONCE:   return (elapsed >= per) ? c->hi : c->lo + (int)(span * elapsed / per);
    }
    return c->lo;
}
static int watch_index(uint8_t cc) { for (int w = 0; w < 5; w++) if (WATCH_CC[w] == cc) return w; return -1; }

/* gemeinsamer Send-Pfad: liefert 1, wenn die Nachricht weitergeleitet werden soll */
#ifndef NO_ARP
void arp_on_send(uint8_t port, void *self, const midi_msg_t *m);   /* arp.c (Patch 014) */
void arp_tick(int ticks);
#endif
static int on_send(uint8_t port, void *self, const midi_msg_t *m)
{
    int forward = 1;
#ifndef NO_ARP
    arp_on_send(port, self, m);
#endif
    uint32_t p = irq_save();
    ensure_init();
    ST->port[port] = self;
    if (m->type == 6u && m->d1 == 123u) {                       /* All Notes Off (Stopp) */
        /* nur ausstehende Note-Ons verwerfen; Note-Offs bereits klingender Echos bleiben */
        uint32_t i = 0;
        while (i < ST->n) {
            if (ST->ev[i].port == port && ST->ev[i].type == 4u) ST->ev[i] = ST->ev[--ST->n]; else i++;
        }
        if (ST->loop_pad >= 0) ST->loop_pad = -2;               /* Loop bis zum naechsten Pad-Wechsel ruhen lassen */
    } else if (m->type == 4u && m->d2 > 0u) {
        if (ST->loop_pad >= 0) {
            forward = 0;                                        /* Loop aktiv: neue Noten stumm */
        } else {
            hist_add(4, m->ch, m->d1, m->d2, port);
            plan_echo(port, m->ch, m->d1, m->d2);
        }
    } else if (m->type == 5u || (m->type == 4u && m->d2 == 0u)) {
        hist_add(5, m->ch, m->d1, 0, port);
    } else if (m->type == 6u && m->ch < 16u) {
        for (int w = 0; w < 5; w++) if (WATCH_CC[w] == m->d1) ST->ccval[port][m->ch][w] = m->d2;
    }
    if (m->type == 4u && m->d2 > 0u && m->ch < 16u) ST->lastnote[port][m->ch] = ST->now + 1u;
    irq_restore(p);
    return forward;
}

__attribute__((used, section(".text.entry_usb")))
void usb_send_hook(void *self, const midi_msg_t *m) { if (on_send(0, self, m)) ORIG_USB(self, m); }

__attribute__((used, section(".text.entry_din")))
void din_send_hook(void *self, const midi_msg_t *m) { if (on_send(1, self, m)) ORIG_DIN(self, m); }

__attribute__((used, section(".text.entry_tick")))
void tick_hook(void *queue, int ticks)
{
    if (ticks <= 0) { ORIG_TICK(queue, ticks); return; }

    midi_msg_t due[40]; uint8_t dport[40]; void *ports[2];
    uint32_t nd = 0;

    /* 1) faellige geplante Events einsammeln (vor dem Sequencer-Tick, damit in diesem
          Tick neu geplante Events ihre volle Verzoegerung behalten) */
    uint32_t p = irq_save();
    ensure_init();
    uint32_t i = 0;
    while (i < ST->n) {
        volatile ev_t *e = &ST->ev[i];
        e->ticks -= ticks;
        if (e->ticks <= 0 && nd < 32u) {
            due[nd].type = e->type; due[nd].ch = e->ch; due[nd].pad = 0;
            due[nd].d1 = e->note; due[nd].d2 = e->vel; dport[nd] = e->port; nd++;
            hist_add(e->type, e->ch, e->note, e->vel, e->port);
            ST->ev[i] = ST->ev[--ST->n];
        } else {
            i++;
        }
    }
    irq_restore(p);

    /* 2) Sequencer-Tick (sendet Noten des aktuellen Ticks ueber die Hooks, Zeit = now) */
    ORIG_TICK(queue, ticks);
#ifndef NO_ARP
    arp_tick(ticks);                                            /* Arp-Toene (Patch 014) */
#endif

    /* 3) Loop-Zustand nachfuehren und Loop-Noten erzeugen; 4) Zeit weiterzaehlen */
    p = irq_save();
    ports[0] = ST->port[0]; ports[1] = ST->port[1];
#ifdef NO_LOOP
    int lp = -1;
#else
    int lp = active_entry(7, LOOP_FIRST);
#endif
    if (ST->loop_pad == -2) {                                   /* nach Stopp: erst nach Loslassen wieder scharf */
        if (lp < 0) ST->loop_pad = -1;
    } else if (lp != ST->loop_pad) {
        if (lp >= 0) { if (ST->loop_pad < 0) loop_snapshot(); loop_capture(lp); } else ST->loop_pad = -1;
    }
    for (int t = 0; t < ticks; t++) {
        if (ST->loop_pad >= 0 && ST->loop_len > 0u) {
            uint32_t pos = ST->loop_pos;
            for (uint32_t j = 0; j < ST->loop_n && nd < 32u; j++) {
                volatile lnote_t *l = &ST->loop[j];
                if (l->rel == pos && ST->n < MAXEV) {                /* nur mit Platz fuer das Note-Off */
                    due[nd].type = 4; due[nd].ch = l->ch; due[nd].pad = 0; due[nd].d1 = l->note; due[nd].d2 = l->vel;
                    dport[nd] = l->port; nd++;
                    hist_add(4, l->ch, l->note, l->vel, l->port);
                    push((int32_t)l->dur, 5, l->ch, l->note, 64, l->port);
                    plan_echo(l->port, l->ch, l->note, l->vel);
                }
            }
            ST->loop_pos = (pos + 1u) % ST->loop_len;
        }
        ST->now++;
    }

    /* CC-Motion (Slot 14) */
#ifdef NO_LFO
    int fp = -1;
#else
    int fp = active_entry(14, REVERB_MIDI_FIRST);
#endif
    if (fp != ST->lfo_pad) {
        /* altes Ziel wiederherstellen */
        if (ST->lfo_pad >= 0) {
            const lfo_cfg_t *oc = &LFO[ST->lfo_pad];
            int w = watch_index(oc->cc);
            for (int pt = 0; pt < 2; pt++) for (int ch = 0; ch < 16; ch++) {
                if (!(ST->lfo_sent[pt] & (1u << ch)) || nd >= 40u) continue;
                uint8_t v = (w >= 0 && ST->ccval[pt][ch][w] != 0xFF) ? ST->ccval[pt][ch][w] : (w >= 0 ? WATCH_DEF[w] : 64);
                due[nd].type = 6; due[nd].ch = (uint8_t)ch; due[nd].pad = 0; due[nd].d1 = oc->cc; due[nd].d2 = v; dport[nd] = (uint8_t)pt; nd++;
            }
        }
        ST->lfo_sent[0] = ST->lfo_sent[1] = 0;
        ST->lfo_pad = fp; ST->lfo_start = ST->now; ST->lfo_val = -1; ST->lfo_last_tick = ST->now - 2u;
    }
    if (ST->lfo_pad >= 0 && (ST->now - ST->lfo_last_tick) >= 2u) {
        const lfo_cfg_t *c = &LFO[ST->lfo_pad];
        int v = lfo_value(c, ST->now - ST->lfo_start);
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        if (v != ST->lfo_val) {
            ST->lfo_val = v; ST->lfo_last_tick = ST->now;
            for (int pt = 0; pt < 2; pt++) {
                int sentc = 0;
                for (int ch = 0; ch < 16 && sentc < 4; ch++) {
                    uint32_t ln = ST->lastnote[pt][ch];
                    if (ln == 0u || (ST->now + 1u - ln) > 384u || nd >= 40u) continue;
                    due[nd].type = 6; due[nd].ch = (uint8_t)ch; due[nd].pad = 0; due[nd].d1 = c->cc; due[nd].d2 = (uint8_t)v; dport[nd] = (uint8_t)pt; nd++;
                    ST->lfo_sent[pt] |= (uint16_t)(1u << ch); sentc++;
                }
            }
        }
    }
    irq_restore(p);

    /* 5) senden (ausserhalb der Interrupt-Sperre) */
    for (uint32_t k = 0; k < nd; k++) {
        void *s = ports[dport[k]];
        if (s == 0) continue;
        if (dport[k] == 0) ORIG_USB(s, &due[k]); else ORIG_DIN(s, &due[k]);
    }
}

#ifdef HOST_TEST
uint32_t host_pending(void) { return ST->n; }
int host_lfo_pad(void) { return ST->lfo_pad; }
#endif
