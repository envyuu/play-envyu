/* Host-Test fuer usbin.c (Patch 020): Setup IF5, Empfang, Ring, Rueckmeldung/Taktabgleich, Mixer-Einspeisung.
 * gcc -O1 -g -DHOST_TEST -fsanitize=address,undefined -o ui_test ui_test.c -lm && ./ui_test [seed]
 */
#define HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "usbin.c"

uint8_t  host_ui_ram[0xF40];
uint32_t host_bdt_desc[32];
const void *host_bdt_ptr[32];
uint8_t  host_endpt6, host_endpt7;
static int mt_calls;
void mt_ep_token(uint32_t stat) { (void)stat; mt_calls++; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FEHLER %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* Paket wie der Host: n Frames ab Zaehler *seq (L = seq, R = ~seq) in den aktuell freien RX-Puffer */
static int rx_bank;
static void host_send(uint32_t n, uint32_t *seq) {
    uint32_t i = rx_bank ? BDT_RX_ODD : BDT_RX_EVEN;
    rx_bank ^= 1;
    uint32_t *b = (uint32_t *)(uintptr_t)host_bdt_ptr[i];
    CHECK(b != 0 && (host_bdt_desc[i] & BDT_OWN), "RX-Bank %u nicht bereit", i);
    if (!b) return;
    for (uint32_t k = 0; k < n; k++) { uint16_t v = (uint16_t)(*seq)++; b[k] = v | ((uint32_t)(uint16_t)~v << 16); }
    host_bdt_desc[i] = (n * 4u) << 16;          /* Hardware: BC, OWN geloescht */
    ui_rx_token(i);
}

static void reset_all(void) {
    memset(host_ui_ram, 0xA5, sizeof host_ui_ram);
    memset(host_bdt_desc, 0, sizeof host_bdt_desc);
    memset(host_bdt_ptr, 0, sizeof host_bdt_ptr);
    host_endpt6 = 0x15; host_endpt7 = 0x19;     /* wie nach SET_CONFIGURATION */
    rx_bank = 0;
    ui_init();
}

static void test_desc(void) {
    const uint8_t *d = UI_DESC_IN;
    CHECK(d[2] == (UI_DESC_LEN & 0xFF) && d[3] == (UI_DESC_LEN >> 8), "wTotalLength");
    /* IF5 alt 1 suchen und Endpunkte pruefen */
    uint32_t p = 0, in_alt = 0; int neps = 0, data = 0, fbk = 0;
    while (p < UI_DESC_LEN) {
        uint8_t l = d[p], t = d[p + 1];
        if (t == 4) in_alt = (d[p + 2] == UI_IF && d[p + 3] == 1);
        if (t == 4 && in_alt) CHECK(d[p + 4] == 2, "IF5 alt1 bNumEndpoints = 2");
        if (t == 5 && in_alt) {
            neps++;
            CHECK(l == 9, "Audio-Endpunkt 9 Byte");
            if (d[p + 2] == UI_EPADDR) { data = 1; CHECK(d[p + 3] == 0x05 && d[p + 4] == RX_MAX && d[p + 8] == FB_EPADDR, "Daten-EP: iso async, 196, Sync 0x87"); }
            if (d[p + 2] == FB_EPADDR) { fbk = 1; CHECK(d[p + 3] == 0x11 && d[p + 4] == 3 && d[p + 6] == 1 && d[p + 7] == 5 && d[p + 8] == 0, "Rueckmelde-EP: 0x11, 3 B, bInterval 1, bRefresh 5, bSynchAddress 0"); }
        }
        p += l;
    }
    CHECK(p == UI_DESC_LEN && neps == 2 && data && fbk, "IF5 alt1: Daten- und Rueckmelde-Endpunkt (%d)", neps);
    CHECK(memcmp(UI_DESC_IN + 9, (const uint8_t[]){ 0x08, 0x0B, 0x00, 0x02 }, 4) == 0, "IAD am Anfang");
}

static void test_setup(void) {
    reset_all();
    uint8_t o[4];
    CHECK(ui_setup(0x0B01, 1, UI_IF, o) == -1, "ohne Modus nicht zustaendig");
    CHECK(ui_ep6_rx() == 0, "ohne Strom kein RXEN");
    ui_set_mode(1);
    CHECK(ui_setup(0x0B01, 1, 4, o) == -1, "IF4 gehoert usbaudio.c");
    CHECK(ui_setup(0x0B01, 2, UI_IF, o) == 0, "alt 2 → STALL");
    CHECK(ui_setup(0x0B01, 1, UI_IF, o) == 1, "SET_INTERFACE alt 1");
    CHECK(host_endpt6 == 0x1C && host_endpt7 == 0x14, "ENDPT6 0x1C, ENDPT7 0x14 (%02x %02x)", host_endpt6, host_endpt7);
    CHECK(host_bdt_desc[24] == ((RX_MAX << 16) | 0x80) && host_bdt_desc[25] == ((RX_MAX << 16) | 0x80), "RX-Baenke bereit");
    CHECK(host_bdt_desc[30] == ((3u << 16) | 0x80) && host_bdt_desc[31] == ((3u << 16) | 0x80), "Rueckmeldung bereit");
    const uint8_t *f = host_bdt_ptr[30];
    CHECK(f && (f[0] | f[1] << 8 | f[2] << 16) == NOM_FB, "Rueckmeldung = Nenntakt");
    CHECK(ui_ep6_rx() == 0x08, "RXEN gemeldet");
    CHECK(ui_setup(0x0A81, 0, UI_IF, o) == 2 && o[0] == 1, "GET_INTERFACE = 1");
    CHECK(ui_setup(0x0122, 0x0100, UI_EPADDR, o) == 1, "SET_CUR Rate EP 0x06");
    CHECK(ui_setup(0x0122, 0x0100, 0x86, o) == -1, "SET_CUR EP 0x86 → usbaudio.c");
    CHECK(ui_setup(0x81A2, 0x0100, UI_EPADDR, o) == 3 && o[0] == 0x44 && o[1] == 0xAC && o[2] == 0, "GET_CUR 44100");
    CHECK(ui_setup(0x81A2, 0x0200, UI_EPADDR, o) == 0, "GET_CUR anderes Control → STALL");
    CHECK(ui_setup(0x0681, 0x2200, 0, o) == -1, "fremder Request");
    /* ep7_token verteilt */
    mt_calls = 0;
    ep7_token(31u << 2);
    CHECK(mt_calls == 0 && host_bdt_desc[31] == ((3u << 16) | 0x80) && UI->fb_pkts == 1, "EP7 → Rueckmeldung");
    /* SET_CONFIGURATION erkannt */
    host_endpt6 = 0x15;
    CHECK(ui_setup(0x0A81, 0, UI_IF, o) == 2 && o[0] == 0 && UI->state == ST_OFF, "nach SET_CONFIGURATION alt 0");
    CHECK(ui_ep6_rx() == 0, "dann kein RXEN");
    /* Stopp */
    ui_setup(0x0B01, 1, UI_IF, o);
    CHECK(ui_setup(0x0B01, 0, UI_IF, o) == 1 && host_endpt6 == 0x14 && host_bdt_desc[24] == 0 && host_bdt_desc[31] == 0, "alt 0 stoppt");
    ui_setup(0x0B01, 1, UI_IF, o);
    ui_set_mode(0);
    CHECK(UI->state == ST_OFF && UI->mode == 0 && host_bdt_desc[24] == 0, "Modus aus stoppt");
    ep7_token(30u << 2);
    CHECK(mt_calls == 1, "ohne Modus EP7 → Multitrack");
    CHECK(ui_setup(0x0B01, 1, UI_IF, o) == -1, "ohne Modus IF5 → Multitrack");
}

static void test_stream_exact(void) {
    reset_all();
    uint8_t o[4];
    ui_set_mode(1);
    ui_setup(0x0B01, 1, UI_IF, o);
    uint32_t seq = 0, expect = 0;
    int started = 0, bad = 0;
    /* Host 44/45 Frames je ms, Geraet 128 Frames je 2,9 ms; erst nach dem Vorlauf kommen Daten */
    double t_dev = 0;
    for (int ms = 0; ms < 3000; ms++) {
        host_send((ms % 10) < 1 ? 45u : 44u, &seq);           /* ~44,1 Frames/ms */
        while (t_dev < ms + 1) {
            ui_pre();
            if (UI->in_valid) {
                if (!started) { started = 1; CHECK(UI->head - UI->tail <= START_FILL, "Start bei Vorlauf"); }
                for (int k = 0; k < 128; k++) {
                    uint16_t a = (uint16_t)IN_L[k], b = (uint16_t)IN_R[k];
                    if (a != (uint16_t)expect || b != (uint16_t)~expect) bad++;
                    expect++;
                }
            }
            ui_post();
            CHECK(UI->in_valid == 0, "in_valid nach ui_post 0");
            t_dev += 128.0 / 44.1;                              /* gleicher Takt → keine Drift */
        }
    }
    CHECK(started && bad == 0, "Datenfolge lueckenlos (%d Fehler)", bad);
    CHECK(UI->underruns == 0 && UI->overruns == 0, "keine Aussetzer (%u/%u)", UI->underruns, UI->overruns);
    CHECK(UI->peak > 30000, "Spitzenpegel gemessen");
}

/* Host wie Linux: Frames je ms aus der Rueckmeldung (Q16-Akkumulator), alle 32 ms neu gelesen; Geraetetakt 44 117,6 Hz,
 * Host-Uhr um ppm verschoben. */
static void test_clock(double ppm, int seconds) {
    reset_all();
    uint8_t o[4];
    ui_set_mode(1);
    ui_setup(0x0B01, 1, UI_IF, o);
    uint32_t seq = 0, expect = 0;
    double fbv = NOM_FB / 16384.0, acc = 0, t_dev = 0, dev_ms = 1000.0 * 128.0 / 44117.647;
    uint32_t minf = 9999, maxf = 0; int bad = 0, run_blocks = 0;
    double fb_sum = 0; int fb_n = 0;
    for (int ms = 0; ms < seconds * 1000; ms++) {
        double host_t = (ms + 1) * (1.0 + ppm * 1e-6);          /* Host-ms in Geraetezeit */
        if (ms % 32 == 0) {
            uint32_t i = (ms / 32) & 1 ? FB_BDT_ODD : FB_BDT_EVEN;
            const uint8_t *f = host_bdt_ptr[i];
            fbv = (f[0] | f[1] << 8 | f[2] << 16) / 16384.0;
            host_bdt_desc[i] &= ~0x80u;
            ep7_token(i << 2);
            if (ms > seconds * 500) { fb_sum += fbv; fb_n++; }
        }
        acc += fbv;
        uint32_t n = (uint32_t)acc; acc -= n;
        if (n > 49) n = 49;
        host_send(n, &seq);
        while (t_dev < host_t) {
            ui_pre();
            if (UI->in_valid) {
                for (int k = 0; k < 128; k++) { if ((uint16_t)IN_L[k] != (uint16_t)expect) bad++; expect++; }
                if (++run_blocks > 200) {
                    uint32_t f = UI->head - UI->tail;
                    if (f < minf) minf = f;
                    if (f > maxf) maxf = f;
                }
            }
            ui_post();
            t_dev += dev_ms;
        }
    }
    double want = 44.117647 * (1.0 + ppm * 1e-6);               /* Frames je Host-ms (Host-ms dauert 1+ppm Geraete-ms) */
    double got = fb_sum / fb_n;
    CHECK(bad == 0, "ppm %+.0f: lueckenlos (%d)", ppm, bad);
    CHECK(UI->underruns == 0 && UI->overruns == 0, "ppm %+.0f: keine Aussetzer (u %u, o %u)", ppm, UI->underruns, UI->overruns);
    CHECK(fabs(got - want) < 0.002, "ppm %+.0f: Rueckmeldung %.5f ~ %.5f Frames/ms", ppm, got, want);
    CHECK(minf >= 64 && maxf <= 448, "ppm %+.0f: Fuellstand %u..%u", ppm, minf, maxf);
    printf("   Takt %+5.0f ppm: Rueckmeldung %.5f (soll %.5f), Fuellstand %u..%u, %u Bloecke\n", ppm, got, want, minf, maxf, UI->blocks);
}

static void test_underrun_restart(void) {
    reset_all();
    uint8_t o[4];
    ui_set_mode(1);
    ui_setup(0x0B01, 1, UI_IF, o);
    uint32_t seq = 0;
    for (int i = 0; i < 20; i++) host_send(44, &seq);
    ui_pre(); CHECK(UI->in_valid == 1, "laeuft nach Vorlauf"); ui_post();
    for (int i = 0; i < 10; i++) { ui_pre(); ui_post(); }    /* Host schickt nichts mehr */
    CHECK(UI->underruns == 1 && UI->state == ST_PREFILL, "Unterlauf → Vorlauf (%u)", UI->underruns);
    ui_pre(); CHECK(UI->in_valid == 0, "im Vorlauf keine Einspeisung"); ui_post();
    /* Ueberlauf: Ring voll */
    for (int i = 0; i < 20; i++) host_send(49, &seq);
    CHECK(UI->overruns > 0 && UI->head - UI->tail == RING_FRAMES, "Ueberlauf begrenzt auf Ringgroesse");
    /* ungerade/zu lange Pakete */
    uint32_t i = rx_bank ? BDT_RX_ODD : BDT_RX_EVEN; rx_bank ^= 1;
    host_bdt_desc[i] = 1023u << 16; ui_rx_token(i);
    CHECK(UI->rx_odd == 1 && host_bdt_desc[i] == ((RX_MAX << 16) | 0x80), "1023 B: begrenzt, neu bereit");
    ui_force_stop();
    CHECK(UI->state == ST_OFF && host_bdt_desc[24] == 0, "force_stop");
    host_bdt_desc[24] = 0; ui_rx_token(24);
    CHECK(host_bdt_desc[24] == 0, "nach Stopp kein Neuarmieren");
}

static int16_t orig_buf[256];
static uint32_t orig_ctx_seen;
static int orig_mono;
static void orig_cb(int32_t *out, uint32_t ctx) { orig_ctx_seen = ctx; out[0] = orig_mono ? 1 : 2; out[1] = (int32_t)(uintptr_t)orig_buf; }

static void test_mixer(void) {
    reset_all();
    uint32_t mixer_obj[0x200 / 4]; memset(mixer_obj, 0, sizeof mixer_obj);
    uint32_t mixer = (uint32_t)(uintptr_t)mixer_obj;
    /* ohne gueltigen Block: Original (0) */
    CHECK(ui_select(mixer) == 0, "inaktiv → kein Rueckruf");
    UI->in_valid = 1;
    for (int k = 0; k < 128; k++) { IN_L[k] = (int16_t)(k * 100); IN_R[k] = (int16_t)(-k * 100); }
    uint32_t cb = ui_select(mixer);
    CHECK(cb == FNADDR(ui_mix_cb), "aktiv → eigener Rueckruf");
    int32_t out[2] = { 0, 0 };
    ui_mix_cb(out, 0x1234);
    const int16_t *m = (const int16_t *)(uintptr_t)(uint32_t)out[1];
    CHECK(out[0] == 2 && m == MST, "Stereo, eigener Puffer");
    int okv = 1; for (int k = 0; k < 128; k++) if (m[k] != k * 100 || m[k + 128] != -k * 100) okv = 0;
    CHECK(okv, "Master = USB-Eingang");
    /* mit Original-Rueckruf (stereo und mono), Saettigung */
    *(uint32_t *)((uint8_t *)mixer_obj + 0x150) = 1;          /* Platzhalter, wird unten ersetzt */
    mixer_obj[0x150 / 4] = (uint32_t)(uintptr_t)orig_cb;
    for (int k = 0; k < 256; k++) orig_buf[k] = (int16_t)(k < 128 ? 30000 : 7);
    CHECK(ui_select(mixer) == FNADDR(ui_mix_cb) && UI->orig_cb == (uint32_t)(uintptr_t)orig_cb, "Original gemerkt");
    orig_mono = 0; ui_mix_cb(out, 0x55);
    CHECK(orig_ctx_seen == 0x55 && UI->orig_calls == 1, "Original mit ctx aufgerufen");
    CHECK(MST[127] == 32767 && MST[0] == 30000 && MST[128] == 7 && MST[129] == 7 - 100, "Summe stereo + Saettigung (%d %d %d)", MST[127], MST[128], MST[129]);
    orig_mono = 1; ui_mix_cb(out, 0);
    CHECK(MST[128] == 30000 && MST[129] == 30000 - 100, "mono-Original auf beide Seiten");
    UI->in_valid = 0;
    CHECK(ui_select(mixer) == (uint32_t)(uintptr_t)orig_cb, "inaktiv → Original-Rueckruf");
    /* Sends */
    int16_t src[128]; for (int k = 0; k < 128; k++) src[k] = (int16_t)(k - 64);
    CHECK(ui_send(src, 8) == src, "inaktiv → Original-Send");
    UI->in_valid = 1;
    for (int k = 0; k < 128; k++) { IN_L[k] = 4000; IN_R[k] = 0; }
    const int16_t *s = ui_send(src, 8);
    CHECK(s == SENDB && s[0] == -64 + 500 && s[127] == 63 + 500, "Delay: Original + 0,25 * mono (%d)", s[0]);
    CHECK(ui_send(src, 7) == src && ui_send(src, 10) == src, "andere Effekte unberuehrt");
    UI->send[1] = 0; CHECK(ui_send(src, 9) == src, "Anteil 0 → Original");
    UI->send[1] = 65536 * 4; for (int k = 0; k < 128; k++) { IN_L[k] = 32767; IN_R[k] = 32767; src[k] = 32767; }
    s = ui_send(src, 9);
    CHECK(s[5] == 32767, "Send begrenzt auf 1,0 und saettigt");
}

static void test_random(void) {
    /* Zufall: Pakete 0..49 Frames, Start/Stopp, SET_CONFIGURATION, Modus an/aus – nie ausserhalb des Rings, Daten immer
     * fortlaufend (nur Luecken an Stopps/Unterlaeufen) */
    reset_all();
    uint8_t o[4];
    ui_set_mode(1);
    uint32_t seq = 0;
    int steps = 200000; long inv = 0;
    for (int i = 0; i < steps; i++) {
        uint32_t r = rnd() % 1000;
        if (r < 3) ui_setup(0x0B01, rnd() & 1, UI_IF, o);
        else if (r < 4) { host_endpt6 = 0x15; ui_setup(0x0A81, 0, UI_IF, o); }
        else if (r < 5) ui_set_mode((int)(rnd() & 1) | (rnd() & 1));
        else if (r < 600) {
            if (UI->state != ST_OFF) host_send(rnd() % 50, &seq);
        } else {
            ui_pre();
            if (UI->in_valid) {
                for (int k = 1; k < 128; k++) if ((uint16_t)(IN_L[k] - IN_L[k - 1]) != 1) inv++;
            }
            ui_post();
        }
        if (UI->head - UI->tail > RING_FRAMES) inv += 1000;
        if (UI->state == ST_OFF && (host_bdt_desc[24] & 0x80)) inv += 1000000;
    }
    CHECK(inv == 0, "Zufallstest: Invarianten (%ld)", inv);
}

int main(int argc, char **argv) {
    if (argc > 1) rng = (uint32_t)strtoul(argv[1], 0, 0);
    test_desc();
    test_setup();
    test_stream_exact();
    test_clock(0, 120);
    test_clock(+300, 120);
    test_clock(-300, 120);
    test_underrun_restart();
    test_mixer();
    test_random();
    printf("%s: %d Pruefungen, %d Fehler\n", fails ? "FEHLER" : "OK", checks, fails);
    return fails != 0;
}
