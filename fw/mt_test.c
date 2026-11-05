/* PC-Test fuer multitrack.c (Patch 015) zusammen mit usbaudio.c (Setup-Weiterleitung, audio_tap).
 * gcc -O1 -g -DHOST_TEST -fsanitize=address,undefined -o mt_test usbaudio.c multitrack.c mt_test.c -lm && ./mt_test
 *
 * Teil A: Setup-Requests fuer IF5 / EP 0x87, BDT/ENDPT7, Zusammenspiel mit IF4 (Stereo).
 * Teil B: Erzeuger-Semantik: Routing-Presets A/B, eigene Tabelle, Stille fuer fehlende Spuren, FX-Summen,
 *         Spur-Lautstaerke mit Rampe, Export (Mixer ausserhalb der Audio-ISR), Alt-Wechsel mitten im Block, Ring voll.
 * Teil C: Simulation Erzeuger (Audio-ISR mit Mixer-Attrappe, die die Hooks wie der echte Mixer aufruft) gegen
 *         Verbraucher (USB-Frames, Hosttakt, Jitter, Aussetzer, Unterbrechung des Erzeugers mitten im Mixer).
 *         Geprueft: jeder Kanal jedes Frames gegen die erwartete Quelle (Zaehlermuster), Kontinuitaet,
 *         Unter-/Ueberlaeufe, Fuellstand, Paketgroessen, Ratenmittel; parallel laeuft IF4 (Stereo).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

uint8_t  host_ram[0x3F00];                       /* 0x1FFFC000..0x1FFFFF00 */
uint32_t host_bdt_desc[32];
const void *host_bdt_ptr[32];
uint32_t host_setup[2];
uint8_t  host_ep0_bank, host_ep0_toggle, host_endpt6, host_endpt7;
void host_usb_init(void) {}

void ua_ep_token(uint32_t stat);
int  ua_setup(void);
void audio_tap(int16_t *l, int16_t *r);
void usbinit_hook(void);
void mt_track(const int16_t *x, uint32_t i, const uint8_t *mixer);
void mt_fx(const int16_t *l, const int16_t *r, uint32_t which);
void mt_ep_token(uint32_t stat);
void mt_pre(void);
void mt_post(const int16_t *l, const int16_t *r);

typedef struct {
    uint32_t magic, head, tail, alt, state, nch, epoch, blocks, drops, packets, underruns, frames;
    int32_t favg, rate; uint32_t acc, last_n, fill_min, fill_max, starts, setups;
    uint32_t wr_ok, wr_epoch, wr_pos, wr_nch, wmask, trk_seen, fx_seen, hook_calls;
    uint8_t rt[12]; uint32_t pad;
} mt_view_t;
typedef struct { uint32_t magic; uint8_t preset, flags, res[2]; uint8_t route10[10], route11[11]; } cfg_view_t;
#define M   ((volatile mt_view_t *)(void *)(host_ram + 0x1400))
#define CFG ((volatile cfg_view_t *)(void *)(host_ram + 0x1480))
#define UAS (*(volatile uint32_t *)(void *)(host_ram + 0x1010))   /* ua_t.state */
#define TX0 (host_ram + 0x1630)
#define TX1 (host_ram + 0x1A10)
#define MOD (384u << 20)

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FEHLER %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- Signale (Frame f) ---------------- */
enum { S_ML = 8, S_MR = 9, S_FXL = 10, S_FXR = 11, S_FXM = 12, S_MM = 13, S_DL = 14, S_DR = 15, S_RL = 16, S_RR = 17 };
static uint32_t hsh(uint32_t a, uint32_t b) { uint32_t x = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u; x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; return x; }
static int16_t sig_track(uint32_t i, uint32_t f) {
    if (i == 0) return (int16_t)(f & 0x7FFF);                 /* Spur 1/2 tragen den Framezaehler */
    if (i == 1) return (int16_t)((f >> 15) & 0x7FFF);
    return (int16_t)(hsh(i, f) & 0xFFFF);
}
static int16_t sig_fx(uint32_t s, uint32_t f) { return (int16_t)((hsh(100 + s, f) & 0x3FFF) - 0x2000); }   /* klein: Summen ohne Saettigung */
static int16_t sig_m(uint32_t s, uint32_t f) { return s == S_ML ? (int16_t)(f & 0x7FFF) : (int16_t)((hsh(200, f) & 0xFFFF)); }
static int16_t expect_src(uint32_t s, uint32_t f, uint32_t present) {
    if (s < 8) return (present >> s) & 1u ? sig_track(s, f) : 0;
    switch (s) {
    case S_ML: case S_MR: return sig_m(s, f);
    case S_MM: return (int16_t)(((int32_t)sig_m(S_ML, f) + sig_m(S_MR, f)) >> 1);
    case S_FXL: return (int16_t)(sig_fx(S_DL, f) + sig_fx(S_RL, f));
    case S_FXR: return (int16_t)(sig_fx(S_DR, f) + sig_fx(S_RR, f));
    case S_FXM: return (int16_t)((((int32_t)sig_fx(S_DL, f) + sig_fx(S_DR, f)) >> 1) + (((int32_t)sig_fx(S_RL, f) + sig_fx(S_RR, f)) >> 1));
    case S_DL: case S_DR: case S_RL: case S_RR: return sig_fx(s, f);
    default: return 0;
    }
}
static const uint8_t PA10[10] = { 0, 1, 2, 3, 4, 5, 6, 7, S_ML, S_MR }, PA11[11] = { 0, 1, 2, 3, 4, 5, 6, 7, S_ML, S_MR, S_FXM };
static const uint8_t PB10[10] = { 0, 1, 2, 3, 4, 5, 6, 7, S_FXL, S_FXR }, PB11[11] = { 0, 1, 2, 3, 4, 5, 6, 7, S_FXL, S_FXR, S_MM };

/* ---------------- Mixer-Attrappe: ruft die Hooks in derselben Reihenfolge wie 0x15884 ---------------- */
static uint8_t mixer[0x200];                    /* +0x1C/+0x3C/+0x9C/+0xBC Verstaerkungen */
static uint32_t gen_f;                          /* naechster Frame */
static uint32_t present = 0xFF;                 /* Spuren mit Eingangsblock */
static int export_mode;                         /* App_audioProcess liefert Stille, Mixer laeuft nicht */
static void (*preempt_cb)(void);                /* USB-ISR mitten im Mixer */
static int preempt_at = -1;
static void set_gain(int i, int32_t cl, int32_t cr, int32_t pl, int32_t pr) {
    int32_t *g = (int32_t *)(void *)mixer; g[7 + i] = cl; g[15 + i] = cr; g[39 + i] = pl; g[47 + i] = pr;
}
static void mixer_run(int16_t *l, int16_t *r) {
    static int16_t x[8][128], fx[4][128];
    for (int i = 0; i < 8; i++) {
        if (preempt_at == i && preempt_cb) preempt_cb();
        if (!((present >> i) & 1u)) continue;   /* kein Eingangsblock → Mixer erreicht 0x158D6 nicht */
        for (int k = 0; k < 128; k++) x[i][k] = sig_track(i, gen_f + k);
        mt_track(x[i], i, mixer);
    }
    if (preempt_at == 8 && preempt_cb) preempt_cb();
    for (int k = 0; k < 128; k++) {
        fx[0][k] = sig_fx(S_DL, gen_f + k); fx[1][k] = sig_fx(S_DR, gen_f + k);
        fx[2][k] = sig_fx(S_RL, gen_f + k); fx[3][k] = sig_fx(S_RR, gen_f + k);
    }
    mt_fx(fx[0], fx[1], 8);
    mt_fx(fx[2], fx[3], 9);
    for (int k = 0; k < 128; k++) { l[k] = sig_m(S_ML, gen_f + k); r[k] = sig_m(S_MR, gen_f + k); }
}
void host_audio_cb(int16_t *l, int16_t *r) {
    if (export_mode) { memset(l, 0, 256); memset(r, 0, 256); gen_f += 128; return; }
    mixer_run(l, r);
    gen_f += 128;
}
static int16_t L[128], R[128];
static void block(void) { audio_tap(L, R); }

/* ---------------- EP0/Setup ---------------- */
static void setup_req(uint8_t bmrt, uint8_t breq, uint16_t wv, uint16_t wi, uint16_t wl) {
    host_setup[0] = (uint32_t)bmrt | ((uint32_t)breq << 8) | ((uint32_t)wv << 16);
    host_setup[1] = (uint32_t)wi | ((uint32_t)wl << 16);
    host_ep0_toggle = 1;
    memset(host_bdt_desc + 2, 0, 2 * sizeof(uint32_t)); host_bdt_ptr[2] = host_bdt_ptr[3] = 0;
}
static uint32_t ep0_last_desc(void) { return host_bdt_desc[2 | (host_ep0_bank ^ 1)]; }
static const uint8_t *ep0_last_ptr(void) { return host_bdt_ptr[2 | (host_ep0_bank ^ 1)]; }
static int set_if(int itf, int alt) { setup_req(0x01, 0x0B, alt, itf, 0); return ua_setup(); }
static int get_if(int itf) { setup_req(0x81, 0x0A, 0, itf, 1); if (!ua_setup()) return -1; return ep0_last_ptr()[0]; }

static void reset_all(uint8_t junk) {
    memset(host_ram, junk, sizeof host_ram);
    memset(host_bdt_desc, 0, sizeof host_bdt_desc); memset(host_bdt_ptr, 0, sizeof host_bdt_ptr);
    host_endpt6 = 0x15; host_endpt7 = 0x19;      /* wie nach SET_CONFIGURATION (Original) */
    usbinit_hook();
    for (int i = 0; i < 8; i++) set_gain(i, 0x10000, 0x10000, 0x10000, 0x10000);
    present = 0xFF; export_mode = 0; preempt_cb = 0; preempt_at = -1; gen_f = 0;
}

static void test_setup(void) {
    memset(host_ram, 0xA5, sizeof host_ram);
    host_endpt7 = 0x19;
    CHECK(set_if(5, 1) == 0, "vor der Initialisierung STALL");
    reset_all(0xA5);
    CHECK(M->magic == 0x4D54A015u && M->state == 0 && M->head == 0 && M->tail == 0 && M->alt == 0, "Zustand nicht initialisiert");
    CHECK(CFG->magic == 0x4D544346u && CFG->preset == 0 && CFG->flags == 1, "Konfiguration nicht initialisiert");
    CHECK(memcmp((const void *)CFG->route10, PA10, 10) == 0 && memcmp((const void *)CFG->route11, PA11, 11) == 0, "Routing-Vorgaben");
    CHECK(get_if(5) == 0, "GET_INTERFACE IF5 = 0");
    CHECK(set_if(5, 1) == 1 && ep0_last_desc() == 0xC8u && ep0_last_ptr() == 0, "SET_INTERFACE IF5 alt1 → ZLP DATA1");
    CHECK(host_endpt7 == 0x14, "ENDPT7 = %02x", host_endpt7);
    CHECK(host_bdt_desc[30] == 0x80 && host_bdt_desc[31] == 0x80 && host_bdt_ptr[30] == TX0 && host_bdt_ptr[31] == TX1, "EP7-Baenke");
    CHECK(M->state == 1 && M->alt == 1 && M->nch == 10 && M->starts == 1, "Zustand nach alt1");
    CHECK(get_if(5) == 1, "GET_INTERFACE IF5 = 1");
    CHECK(set_if(5, 2) == 1 && M->nch == 11 && M->alt == 2 && get_if(5) == 2, "alt2 = 11 Kanaele");
    CHECK(set_if(5, 3) == 0 && M->alt == 2, "alt3 → STALL, Zustand bleibt");
    CHECK(UAS == 0 && get_if(4) == 0, "IF4 unberuehrt");
    CHECK(set_if(4, 1) == 1 && host_endpt6 == 0x14 && UAS == 1 && M->alt == 2, "IF4 alt1 unabhaengig von IF5");
    setup_req(0x22, 0x01, 0x0100, 0x87, 3); CHECK(ua_setup() == 1 && ep0_last_desc() == 0xC8u, "SET_CUR Rate EP 0x87");
    setup_req(0xA2, 0x81, 0x0100, 0x87, 3);
    CHECK(ua_setup() == 1 && (ep0_last_desc() >> 16) == 3 && ep0_last_ptr()[0] == 0x44 && ep0_last_ptr()[1] == 0xAC, "GET_CUR Rate EP 0x87");
    setup_req(0xA2, 0x81, 0x0200, 0x87, 3); CHECK(ua_setup() == 0, "GET_CUR fremdes Control → STALL");
    setup_req(0x81, 0x0A, 0, 5, 0); CHECK(ua_setup() == 1 && (ep0_last_desc() >> 16) == 0, "GET_INTERFACE wLength 0");
    setup_req(0x01, 0x0B, 0, 6, 0); CHECK(ua_setup() == 0, "IF6 gibt es nicht → STALL");
    setup_req(0x01, 0x0B, 0, 0, 0); CHECK(ua_setup() == 0, "IF0 weiterhin STALL");
    /* EP7-Token im Vorfuellen: Nullpaket */
    mt_ep_token(0x78);
    CHECK(host_bdt_desc[30] == 0x80u, "PREFILL → ZLP (%08x)", host_bdt_desc[30]);
    /* RX-Token auf EP7 (Index 28/29 gehoert Teensy): nicht anfassen */
    host_bdt_desc[28] = 0x12345678u; mt_ep_token(0x70);
    CHECK(host_bdt_desc[28] == 0x12345678u, "EP7-RX darf nicht veraendert werden");
    /* SET_CONFIGURATION: ENDPT7 = 0x19 → beim naechsten Setup alt 0 */
    host_endpt7 = 0x19;
    CHECK(get_if(5) == 0 && M->state == 0, "nach SET_CONFIGURATION alt 0");
    mt_ep_token(0x7C);
    CHECK(host_bdt_desc[31] == 0, "im Zustand AUS nicht neu armieren");
    set_if(5, 1); set_if(5, 0);
    CHECK(host_bdt_desc[30] == 0 && host_bdt_desc[31] == 0 && M->state == 0 && host_endpt7 == 0x14, "alt0 gibt Baenke frei");
}

/* ---------------- Paket abholen und pruefen ---------------- */
typedef struct { int bank, have; uint32_t expect_f; long gaps, bad, pk, frames; int nch; const uint8_t *route; uint32_t present; } rx_t;
static uint32_t decode_f(const int16_t *fr) { return ((uint32_t)(uint16_t)fr[0] & 0x7FFF) | (((uint32_t)(uint16_t)fr[1] & 0x7FFF) << 15); }
static uint32_t do_token(rx_t *x) {
    int i = x->bank ? 31 : 30;
    mt_ep_token(x->bank ? 0x7Cu : 0x78u);
    uint32_t d = host_bdt_desc[i];
    CHECK((d & 0xFFFFu) == 0x80u, "BDT-Steuerbits %08x", d);
    uint32_t len = d >> 16, fs = 2u * x->nch, n = len / fs;
    CHECK(len % fs == 0 && n <= 45 && len <= 990, "Paketlaenge %u", len);
    const int16_t *b = host_bdt_ptr[i];
    CHECK((const uint8_t *)b == (x->bank ? TX1 : TX0), "falscher Sendepuffer");
    for (uint32_t k = 0; k < n; k++) {
        const int16_t *fr = b + k * x->nch;
        uint32_t f = decode_f(fr);
        if (x->have && f != x->expect_f) x->gaps++;
        x->expect_f = f + 1; x->have = 1;
        for (int c = 0; c < x->nch; c++)
            if (fr[c] != expect_src(x->route[c], f, x->present)) { x->bad++; break; }
    }
    x->frames += n;
    x->bank ^= 1; x->pk++;
    return n;
}

/* ---------------- Teil B ---------------- */
static void push_blocks(int n) { for (int k = 0; k < n; k++) block(); }
static void sw2(void) { set_if(5, 2); }
static void sw0(void) { set_if(5, 0); }
static int16_t ring_at(uint32_t frame, uint32_t nch, uint32_t ch) {
    const int16_t *ring = (const int16_t *)(void *)(host_ram + 0x1E00);
    return ring[(frame % 384u) * nch + ch];
}

static void test_semantics(void) {
    /* Preset A, 10 Kanaele, Spur 4 fehlt */
    reset_all(0x3C); set_if(5, 1);
    present = 0xFF & ~(1u << 3);
    push_blocks(1);
    CHECK(M->head == 128 && M->blocks == 1, "Block freigegeben (head %u)", M->head);
    CHECK(M->trk_seen == present && M->fx_seen == 3, "gesehene Quellen %02x/%x", M->trk_seen, M->fx_seen);
    int ok = 1;
    for (uint32_t f = 0; f < 128; f++) for (int c = 0; c < 10; c++) ok &= ring_at(f, 10, c) == expect_src(PA10[c], f, present);
    CHECK(ok, "Preset A: Inhalt (fehlende Spur = Stille)");
    /* Preset B, ab dem naechsten Block */
    CFG->preset = 1; push_blocks(1);
    ok = 1;
    for (uint32_t f = 128; f < 256; f++) for (int c = 0; c < 10; c++) ok &= ring_at(f, 10, c) == expect_src(PB10[c], f, present);
    CHECK(ok, "Preset B: Inhalt");
    /* eigene Tabelle: rueckwaerts, ungueltige Quelle → Stille */
    static const uint8_t cust[10] = { S_RR, S_RL, S_DR, S_DL, S_MM, 7, 6, 99, S_FXM, 0 };
    for (int c = 0; c < 10; c++) CFG->route10[c] = cust[c];
    CFG->preset = 0xFF; present = 0xFF; push_blocks(1);
    ok = 1;
    for (uint32_t f = 256; f < 384; f++) for (int c = 0; c < 10; c++) ok &= ring_at(f, 10, c) == (cust[c] == 99 ? 0 : expect_src(cust[c], f, 0xFF));
    CHECK(ok, "eigene Tabelle (inkl. ungueltiger Quelle)");
    CHECK(M->head == 384, "3 Bloecke");
    /* Ring voll (Verbraucher steht): kein vierter Block, nichts ueberschrieben */
    int16_t before = ring_at(0, 10, 0);
    push_blocks(3);
    CHECK(M->head == 384 && ring_at(0, 10, 0) == before && M->wr_ok == 0, "voller Ring: Block verworfen");
    /* Export: Mixer im Hauptprogramm → Hooks ohne Reservierung tun nichts */
    reset_all(0); set_if(5, 1);
    int16_t xx[128]; memset(xx, 0x11, sizeof xx);
    uint32_t hc = M->hook_calls;
    mt_track(xx, 0, mixer); mt_fx(xx, xx, 8);
    CHECK(M->hook_calls == hc + 2 && M->head == 0, "Hooks ausserhalb der Audio-ISR ohne Wirkung");
    export_mode = 1; push_blocks(2);       /* Echtzeitpfad liefert Stille, Mixer laeuft nicht */
    ok = 1;
    for (uint32_t f = 0; f < 256; f++) for (int c = 0; c < 8; c++) ok &= ring_at(f, 10, c) == 0;
    CHECK(ok && M->head == 256, "Export: Spuren still, Bloecke laufen weiter");
    /* Spur-Lautstaerke: Rampe von vorigem (max L/R) zu aktuellem Wert */
    reset_all(0); set_if(5, 1);
    set_gain(2, 0x8000, 0x4000, 0x2000, 0x10000);   /* aktuell max 0x8000, vorher max 0x10000 */
    set_gain(5, -5, 0x20000, 0, 0);                  /* Begrenzung auf [0, 1.0] */
    push_blocks(1);
    ok = 1;
    for (int k = 0; k < 128; k++) {
        int32_t g = 0x10000 + ((0x8000 - 0x10000) / 128) * k;
        int32_t v = (int32_t)(((int64_t)sig_track(2, k) * g) >> 16);
        ok &= ring_at(k, 10, 2) == v;
        int32_t g5 = 0 + ((0x10000 - 0) / 128) * k;
        ok &= ring_at(k, 10, 5) == (int32_t)(((int64_t)sig_track(5, k) * g5) >> 16);
    }
    CHECK(ok, "Spur-Lautstaerke mit Rampe");
    CFG->flags = 0; push_blocks(1);
    ok = 1;
    for (int k = 128; k < 256; k++) ok &= ring_at(k, 10, 2) == sig_track(2, k);
    CHECK(ok, "vor Fader (flags = 0): unveraendert");
    /* Alt-Wechsel mitten im Block (USB-ISR unterbricht den Mixer) → Block nicht freigeben, danach 11 Kanaele */
    reset_all(0); set_if(5, 1); push_blocks(1);
    preempt_at = 4; preempt_cb = sw2;
    uint32_t h0 = M->head;
    push_blocks(1);
    CHECK(M->head == h0 && M->nch == 11 && M->alt == 2, "Alt-Wechsel im Block: nicht freigegeben (head %u)", M->head);
    preempt_cb = 0; preempt_at = -1;
    push_blocks(1);
    ok = M->head == h0 + 128;
    for (uint32_t k = 0; k < 128; k++) for (int c = 0; c < 11; c++) ok &= ring_at(h0 + k, 11, c) == expect_src(PA11[c], gen_f - 128 + k, 0xFF);
    CHECK(ok, "danach 11-Kanal-Block korrekt");
    /* alt 0 mitten im Block */
    preempt_at = 2; preempt_cb = sw0;
    h0 = M->head; push_blocks(1);
    CHECK(M->head == h0 && M->state == 0, "alt0 im Block: nicht freigegeben");
    preempt_cb = 0; preempt_at = -1;
    push_blocks(2);
    CHECK(M->head == h0 && M->wr_ok == 0, "im Zustand AUS keine Bloecke");
    /* Muell in head/tail (z. B. Speicherfehler) → keine Zugriffe ausserhalb des Rings (ASan) */
    reset_all(0); set_if(5, 2);
    M->head = 0xFFFFFF00u; M->tail = 0x12345u; push_blocks(2);
    M->tail = 0xFFFFFFFFu; mt_ep_token(0x78); mt_ep_token(0x7C);
    CHECK(M->head < MOD && M->tail < MOD, "Muellzustand bereinigt (head %x tail %x)", M->head, M->tail);
}

/* ---------------- Teil C: Simulation ---------------- */
typedef struct {
    const char *name; double fs, host_ppm, jitter_ms, secs;
    int alt, preset, gap_at, gap_len, switch_at, preempt_pct, stereo_too;
} scen_t;

static rx_t *cur_rx;
static void preempt_token(void) { do_token(cur_rx); }

static int run_scen(const scen_t *sc, unsigned seed) {
    srand(seed);
    int f0 = fails;
    reset_all(0x5A);
    gen_f = 0x00100000u;
    CFG->preset = (uint8_t)sc->preset;
    set_if(5, sc->alt);
    if (sc->stereo_too) set_if(4, 1);
    const double tblk = 128.0 / sc->fs, tfrm = 1e-3 * (1.0 + sc->host_ppm * 1e-6);
    double tp = 0.0;
    int nch = sc->alt == 1 ? 10 : 11;
    const uint8_t *route = sc->preset == 1 ? (sc->alt == 1 ? PB10 : PB11) : (sc->alt == 1 ? PA10 : PA11);
    rx_t x = { 0, 0, 0, 0, 0, 0, 0, nch, route, 0xFF };
    cur_rx = &x;
    int ubank = 0;
    long frames_total = (long)(sc->secs / tfrm), measure_from = (long)(3.0 / tfrm);
    if (sc->gap_at) measure_from = sc->gap_at + 3000;
    if (sc->switch_at) measure_from = sc->switch_at + 3000;
    uint32_t und0 = 0, drop0 = 0, fmin = 0xFFFFFFFFu, fmax = 0;
    long nmin = 1000, nmax = -1, gaps0 = 0, cnt = 0; double sum = 0;
    for (long frame = 0; frame < frames_total; frame++) {
        double tc = frame * tfrm + sc->jitter_ms * 1e-3 * (rand() / (double)RAND_MAX);
        int skip = sc->gap_at && frame >= sc->gap_at && frame < sc->gap_at + sc->gap_len;
        int done = 0; uint32_t n = 0;
        while (tp <= tc) {
            if (!skip && !done && tp + tblk > tc && rand() % 100 < sc->preempt_pct) {
                preempt_at = rand() % 9; preempt_cb = preempt_token; done = 1;   /* USB-Token mitten im Mixer */
            }
            block();
            preempt_cb = 0; preempt_at = -1;
            tp += tblk;
        }
        if (skip) continue;
        if (sc->switch_at && frame == sc->switch_at) {          /* Host wechselt 10 ↔ 11 Kanaele */
            set_if(5, 0); push_blocks(10);
            int alt2 = sc->alt == 1 ? 2 : 1;
            set_if(5, alt2);
            x.nch = alt2 == 1 ? 10 : 11;
            x.route = sc->preset == 1 ? (alt2 == 1 ? PB10 : PB11) : (alt2 == 1 ? PA10 : PA11);
            x.have = 0; x.bank = 0;
        }
        if (!done) n = do_token(&x);
        else n = M->last_n;
        if (sc->stereo_too) { ua_ep_token(ubank ? 0x6Cu : 0x68u); ubank ^= 1; }
        if (frame == measure_from) { und0 = M->underruns; drop0 = M->drops; gaps0 = x.gaps; }
        if (frame > measure_from && M->state == 2) {
            uint32_t av = (M->head - M->tail + MOD) % MOD;
            if (av < fmin) fmin = av;
            if (av > fmax) fmax = av;
            sum += n; cnt++;
            if ((long)n < nmin) nmin = n;
            if ((long)n > nmax) nmax = n;
        }
    }
    double mean = cnt ? sum / cnt : 0, want = sc->fs * tfrm;
    printf("  %-40s %2d Kan. Pakete %6ld n=[%ld..%ld] Mittel %.4f (Soll %.4f) Fuellung [%u..%u] Unterl. %u Verw. %u Luecken %ld/%ld Fehlwerte %ld\n",
           sc->name, x.nch, x.pk, nmin, nmax, mean, want, fmin, fmax, M->underruns - und0, M->drops - drop0, x.gaps - gaps0, x.gaps, x.bad);
    CHECK(x.bad == 0, "%s: %ld Frames mit falschem Kanalinhalt", sc->name, x.bad);
    CHECK(M->underruns == und0, "%s: Unterlaeufe", sc->name);
    CHECK(M->drops == drop0, "%s: verworfene Bloecke", sc->name);
    CHECK(x.gaps == gaps0, "%s: Datenluecken im eingeschwungenen Zustand", sc->name);
    CHECK(fabs(mean - want) < 0.002, "%s: Ratenmittel %.5f statt %.5f", sc->name, mean, want);
    CHECK(fmin > 32 && fmax <= 384, "%s: Fuellstand [%u..%u]", sc->name, fmin, fmax);
    CHECK(nmin >= 43 && nmax <= 45, "%s: Paketgroessen [%ld..%ld]", sc->name, nmin, nmax);
    if (!sc->gap_len && !sc->switch_at) CHECK(x.gaps == 0, "%s: %ld Luecken insgesamt", sc->name, x.gaps);
    else CHECK(x.gaps <= 3, "%s: %ld Luecken nach Stoerung", sc->name, x.gaps);
    if (sc->stereo_too) CHECK(UAS == 2, "%s: Stereo-Strom (IF4) laeuft parallel", sc->name);
    return fails == f0;
}

int main(void) {
    printf("Teil A: Setup/EP0/BDT (IF5, EP 0x87)\n");
    test_setup();
    printf("Teil B: Routing, Rampen, Export, Alt-Wechsel, Ring voll\n");
    test_semantics();
    printf("Teil C: Simulation\n");
    const double FS = 180e6 * 16.0 / 255.0 / 256.0;
    const scen_t sc[] = {
        { "nominal, Preset A",                   FS,            0,   0.0, 40, 1, 0, 0, 0, 0, 0, 0 },
        { "nominal, Preset B",                   FS,            0,   0.0, 40, 1, 1, 0, 0, 0, 0, 0 },
        { "11 Kanaele, Preset A",                FS,            0,   0.0, 40, 2, 0, 0, 0, 0, 0, 0 },
        { "11 Kanaele, Preset B",                FS,            0,   0.0, 40, 2, 1, 0, 0, 0, 0, 0 },
        { "Geraet +100 ppm, Host -300 ppm",      FS * 1.0001, -300,  0.3, 60, 1, 0, 0, 0, 0, 0, 0 },
        { "Geraet -200 ppm, Host +500 ppm",      FS * 0.9998,  500,  0.3, 60, 2, 0, 0, 0, 0, 0, 0 },
        { "1000 ppm auseinander",                FS * 1.0005, -500,  0.2, 60, 2, 1, 0, 0, 0, 0, 0 },
        { "ISR-Jitter 0,9 ms",                   FS,            0,   0.9, 40, 2, 0, 0, 0, 0, 0, 0 },
        { "Erzeuger im Mixer unterbrochen 30 %", FS,           50,   0.5, 40, 2, 0, 0, 0, 0, 30, 0 },
        { "Host setzt 20 Frames aus",            FS,            0,   0.2, 40, 1, 0, 15000, 20, 0, 0, 0 },
        { "Wechsel 10 → 11 Kanaele",             FS,            0,   0.2, 40, 1, 0, 0, 0, 15000, 0, 0 },
        { "Wechsel 11 → 10 Kanaele, Preset B",   FS,          100,   0.2, 40, 2, 1, 0, 0, 15000, 10, 0 },
        { "parallel Stereo (IF4), 10 Kanaele",   FS,          200,   0.4, 40, 1, 0, 0, 0, 0, 20, 1 },
    };
    for (size_t i = 0; i < sizeof sc / sizeof sc[0]; i++)
        for (unsigned seed = 1; seed <= 2; seed++) run_scen(&sc[i], seed);
    printf("%d Pruefungen, %d Fehler\n", checks, fails);
    return fails ? 1 : 0;
}
