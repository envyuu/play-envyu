/* PC-Test fuer usbaudio.c (Stereo-Teil aus Patch 012, in 015 weiterverwendet).
 * gcc -O1 -g -DHOST_TEST -fsanitize=address,undefined -o ua_test usbaudio.c multitrack.c ua_test.c -lm && ./ua_test [desc.bin]
 *
 * Teil A: Setup-Requests / EP0 / BDT-Belegung gegen Attrappen.
 * Teil B: ereignisgesteuerte Simulation Erzeuger (Audio-ISR, 128-Frame-Bloecke, Geraetetakt) gegen
 *         Verbraucher (USB-Frames, Hosttakt, Jitter, Aussetzer); Unterbrechung des Erzeugers mitten im Block.
 *         Geprueft: Datenkontinuitaet (Zaehlermuster), Unter-/Ueberlaeufe, Fuellstand, Paketgroessen, Ratenmittel.
 * Optional: schreibt den Konfigurationsdeskriptor nach argv[1] (fuer check_desc.py).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

uint8_t  host_ram[0x3F00];          /* 0x1FFFC000..0x1FFFFF00 (012 + 015) */
uint32_t host_bdt_desc[32];
const void *host_bdt_ptr[32];
uint32_t host_setup[2];
uint8_t  host_ep0_bank, host_ep0_toggle, host_endpt6, host_endpt7 = 0x19;
static int usb_init_called;
void host_usb_init(void) { usb_init_called++; }

/* aus usbaudio.c */
#define CFG_LEN 357
extern const uint8_t ua_cfg_desc[CFG_LEN];
void ua_init(void);
void ua_push(const int16_t *l, const int16_t *r, uint32_t n);
void ua_ep_token(uint32_t stat);
int  ua_setup(void);
void audio_tap(int16_t *l, int16_t *r);
void usbinit_hook(void);

typedef struct {
    uint32_t magic, head, tail, alt, state, blocks, drops, packets, underruns, frames;
    int32_t favg, rate; uint32_t acc, last_n, fill_min, fill_max, starts, setups;
} ua_view_t;
#define U ((volatile ua_view_t *)(void *)(host_ram + 0x1000))

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FEHLER %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- Audio-Callback-Attrappe: Zaehlermuster, Frame s → L = s & 0xFFFF, R = s >> 16 ---------------- */
static uint32_t gen_s;
void host_audio_cb(int16_t *l, int16_t *r) {
    for (int k = 0; k < 128; k++, gen_s++) { l[k] = (int16_t)(gen_s & 0xFFFF); r[k] = (int16_t)(gen_s >> 16); }
}

static void setup_req(uint8_t bmrt, uint8_t breq, uint16_t wv, uint16_t wi, uint16_t wl) {
    host_setup[0] = (uint32_t)bmrt | ((uint32_t)breq << 8) | ((uint32_t)wv << 16);
    host_setup[1] = (uint32_t)wi | ((uint32_t)wl << 16);
    host_ep0_toggle = 1;                         /* wie usbotg_isr vor dem Dispatch */
    memset(host_bdt_desc + 2, 0, 2 * sizeof(uint32_t)); host_bdt_ptr[2] = host_bdt_ptr[3] = 0;
}
static uint32_t ep0_last_desc(void) { return host_bdt_desc[2 | (host_ep0_bank ^ 1)]; }
static const void *ep0_last_ptr(void) { return host_bdt_ptr[2 | (host_ep0_bank ^ 1)]; }

static void test_setup(void) {
    memset(host_ram, 0xA5, sizeof host_ram);    /* Muell wie nach dem Einschalten */
    host_endpt6 = 0x15;                          /* wie nach SET_CONFIGURATION (Original) */
    setup_req(0x01, 0x0B, 1, 4, 0);
    CHECK(ua_setup() == 0, "vor ua_init darf nichts beantwortet werden");
    usbinit_hook();
    CHECK(usb_init_called == 1, "usb_init nicht aufgerufen");
    CHECK(memcmp(host_ram + 0x14C0, ua_cfg_desc, CFG_LEN) == 0, "Deskriptor nicht ins RAM kopiert");
    CHECK(U->magic == 0x55A0D10Au && U->state == 0 && U->head == 0 && U->tail == 0, "Zustand nicht initialisiert");

    /* GET_INTERFACE AS → 0 */
    setup_req(0x81, 0x0A, 0, 4, 1);
    CHECK(ua_setup() == 1, "GET_INTERFACE nicht beantwortet");
    CHECK(ep0_last_desc() == ((1u << 16) | 0xC8u), "GET_INTERFACE BDT %08x", ep0_last_desc());
    CHECK(((const uint8_t *)ep0_last_ptr())[0] == 0, "alt != 0");
    /* SET_INTERFACE AS alt 1 */
    setup_req(0x01, 0x0B, 1, 4, 0);
    CHECK(ua_setup() == 1, "SET_INTERFACE alt1");
    CHECK(ep0_last_desc() == 0xC8u && ep0_last_ptr() == 0, "Statusstufe muss ZLP DATA1 sein (%08x)", ep0_last_desc());
    CHECK(host_endpt6 == 0x14, "ENDPT6 = %02x", host_endpt6);
    CHECK(host_bdt_desc[26] == 0x80 && host_bdt_desc[27] == 0x80, "EP6-Baenke nicht mit ZLP vorbelegt");
    CHECK(host_bdt_ptr[26] == host_ram + 0x1100 && host_bdt_ptr[27] == host_ram + 0x11C0, "EP6-Puffer falsch");
    CHECK(U->state == 1 && U->alt == 1 && U->starts == 1, "Zustand nach alt1");
    setup_req(0x81, 0x0A, 0, 4, 1);
    CHECK(ua_setup() == 1 && ((const uint8_t *)ep0_last_ptr())[0] == 1, "GET_INTERFACE muss 1 liefern");
    setup_req(0x81, 0x0A, 0, 4, 0);
    CHECK(ua_setup() == 1 && (ep0_last_desc() >> 16) == 0, "wLength 0 muss Laenge 0 ergeben");
    /* AC-Interface: alt 0 ok, alt 1 STALL */
    setup_req(0x01, 0x0B, 0, 3, 0);
    CHECK(ua_setup() == 1, "SET_INTERFACE AC alt0");
    setup_req(0x01, 0x0B, 1, 3, 0);
    CHECK(ua_setup() == 0, "SET_INTERFACE AC alt1 muss STALL sein");
    setup_req(0x01, 0x0B, 2, 4, 0);
    CHECK(ua_setup() == 0, "SET_INTERFACE AS alt2 muss STALL sein");
    CHECK(U->state == 1, "ungueltiges alt darf Zustand nicht aendern");
    /* fremde Interfaces / Requests unveraendert STALL */
    setup_req(0x01, 0x0B, 0, 0, 0);  CHECK(ua_setup() == 0, "SET_INTERFACE IF0 muss wie bisher STALL sein");
    setup_req(0x81, 0x0A, 0, 2, 1);  CHECK(ua_setup() == 0, "GET_INTERFACE IF2 muss wie bisher STALL sein");
    setup_req(0x80, 0x06, 0x0200, 0, 255); CHECK(ua_setup() == 0, "GET_DESCRIPTOR darf nicht abgefangen werden");
    setup_req(0x21, 0x0A, 0, 3, 0);  CHECK(ua_setup() == 0, "HID SET_IDLE muss STALL sein");
    setup_req(0xA1, 0x81, 0x0200, 0x0203, 2); CHECK(ua_setup() == 0, "GET_CUR an Unit muss STALL sein (keine Feature Unit)");
    /* Abtastrate */
    setup_req(0x22, 0x01, 0x0100, 0x86, 3); CHECK(ua_setup() == 1 && ep0_last_desc() == 0xC8u, "SET_CUR Rate");
    setup_req(0xA2, 0x81, 0x0100, 0x86, 3);
    CHECK(ua_setup() == 1 && (ep0_last_desc() >> 16) == 3, "GET_CUR Rate Laenge");
    { const uint8_t *p = ep0_last_ptr(); CHECK(p && p[0] == 0x44 && p[1] == 0xAC && p[2] == 0, "GET_CUR Rate Wert"); }
    setup_req(0xA2, 0x81, 0x0100, 0x05, 3); CHECK(ua_setup() == 0, "GET_CUR anderer Endpunkt muss STALL sein");
    /* Banken/Toggle wechseln wie bei Teensy */
    { uint8_t b = host_ep0_bank; setup_req(0x01, 0x0B, 0, 3, 0); ua_setup(); CHECK(host_ep0_bank == (b ^ 1), "ep0 bank toggelt nicht"); }

    /* Token-Behandlung: im PREFILL Nullpakete */
    ua_ep_token(0x68);
    CHECK(host_bdt_desc[26] == 0x80u, "PREFILL muss ZLP liefern (%08x)", host_bdt_desc[26]);
    /* SET_CONFIGURATION (Original setzt ENDPT6 = 0x15) → unser Zustand faellt auf OFF */
    host_endpt6 = 0x15;
    setup_req(0x81, 0x0A, 0, 4, 1);
    CHECK(ua_setup() == 1 && ((const uint8_t *)ep0_last_ptr())[0] == 0 && U->state == 0, "nach SET_CONFIGURATION muss alt 0 gelten");
    ua_ep_token(0x6C);
    CHECK(host_bdt_desc[27] == 0, "im Zustand OFF nicht neu armieren");
    /* alt 0 → Baenke frei */
    setup_req(0x01, 0x0B, 1, 4, 0); ua_setup();
    setup_req(0x01, 0x0B, 0, 4, 0); ua_setup();
    CHECK(host_bdt_desc[26] == 0 && host_bdt_desc[27] == 0 && U->state == 0, "alt0 muss Baenke freigeben");
    /* unerwartetes RX-Token auf EP6 */
    ua_ep_token(0x60);
    CHECK(host_bdt_desc[24] == 0, "RX-Token");
}

/* ---------------- Simulation ---------------- */
typedef struct {
    const char *name;
    double fs;              /* Geraete-Abtastrate (Hz) */
    double host_ppm;        /* Abweichung des USB-Frametakts */
    double jitter_ms;       /* Verzoegerung der USB-ISR (gleichverteilt 0..jitter) */
    double secs;
    int    gap_at, gap_len; /* Host laesst gap_len Frames aus ab Frame gap_at (0 = nie) */
    int    restart_at;      /* alt 0 → alt 1 bei diesem Frame (0 = nie) */
    double preempt_p;       /* Wahrscheinlichkeit, dass ein USB-Token den Erzeuger mitten im Block unterbricht */
} scen_t;

static int16_t blkL[128], blkR[128];

typedef struct { int bank, have; uint32_t expect; long gaps, pk; } rx_t;

static uint32_t do_token(rx_t *x) {          /* ein USB-Frame: Token fertig → Paket pruefen */
    int i = x->bank ? 27 : 26;
    ua_ep_token(x->bank ? 0x6Cu : 0x68u);
    uint32_t d = host_bdt_desc[i];
    CHECK((d & 0xFFFFu) == 0x80u, "BDT-Steuerbits %08x", d);
    uint32_t len = d >> 16, n = len / 4;
    CHECK(len % 4 == 0 && n <= 48, "Paketlaenge %u", len);
    const uint32_t *b = host_bdt_ptr[i];
    CHECK(b == (const void *)(host_ram + (x->bank ? 0x11C0 : 0x1100)), "falscher Sendepuffer");
    for (uint32_t k = 0; k < n; k++) {
        if (x->have && b[k] != x->expect) x->gaps++;
        x->expect = b[k] + 1; x->have = 1;
    }
    x->bank ^= 1; x->pk++;
    return n;
}

static int run_scen(const scen_t *sc, unsigned seed) {
    srand(seed);
    int f0 = fails;
    memset(host_ram, 0x5A, sizeof host_ram);
    gen_s = 0x12340000u;
    usbinit_hook();
    host_endpt6 = 0x15;
    setup_req(0x01, 0x0B, 1, 4, 0); ua_setup();

    const double tblk = 128.0 / sc->fs;
    const double tfrm = 1e-3 * (1.0 + sc->host_ppm * 1e-6);
    double tp = 0.0;
    rx_t x = { 0, 0, 0, 0, 0 };
    long frames_total = (long)(sc->secs / tfrm);
    long measure_from = (long)(5.0 / tfrm);                           /* 5 s Einschwingen */
    if (sc->gap_at) measure_from = sc->gap_at + 5000;
    if (sc->restart_at) measure_from = sc->restart_at + 5000;
    uint32_t und0 = 0, drop0 = 0, fmin = 0xFFFFFFFFu, fmax = 0;
    long nmin = 1000, nmax = -1, gaps0 = 0, cnt_n = 0;
    double sum_n = 0;

    for (long frame = 0; frame < frames_total; frame++) {
        double tc = frame * tfrm + sc->jitter_ms * 1e-3 * (rand() / (double)RAND_MAX);
        int skip = sc->gap_at && frame >= sc->gap_at && frame < sc->gap_at + sc->gap_len;
        int done = 0;
        uint32_t n = 0;
        while (tp <= tc) {
            host_audio_cb(blkL, blkR);
            if (!skip && !done && tp + tblk > tc && sc->preempt_p > 0 && rand() < sc->preempt_p * RAND_MAX) {
                ua_push(blkL, blkR, 64);           /* Erzeuger mitten im Block von der USB-ISR unterbrochen */
                n = do_token(&x); done = 1;
                ua_push(blkL + 64, blkR + 64, 64);
            } else {
                ua_push(blkL, blkR, 128);
            }
            tp += tblk;
        }
        if (skip) continue;
        if (sc->restart_at && frame == sc->restart_at) {
            setup_req(0x01, 0x0B, 0, 4, 0); ua_setup();
            for (int k = 0; k < 50; k++) { host_audio_cb(blkL, blkR); ua_push(blkL, blkR, 128); }  /* laeuft voll */
            setup_req(0x01, 0x0B, 1, 4, 0); ua_setup();
            x.have = 0; x.bank = 0;
        }
        if (!done) n = do_token(&x);
        if (frame == measure_from) { und0 = U->underruns; drop0 = U->drops; gaps0 = x.gaps; }
        if (frame > measure_from && U->state == 2) {
            uint32_t av = U->head - U->tail;
            if (av < fmin) fmin = av;
            if (av > fmax) fmax = av;
            sum_n += n; cnt_n++;
            if ((long)n < nmin) nmin = n;
            if ((long)n > nmax) nmax = n;
        }
    }
    double mean = cnt_n ? sum_n / cnt_n : 0, want = sc->fs * tfrm;
    printf("  %-36s Pakete %6ld n=[%ld..%ld] Mittel %.4f (Soll %.4f) Fuellung [%u..%u] Unterl. %u Verw. %u Luecken %ld/%ld\n",
           sc->name, x.pk, nmin, nmax, mean, want, fmin, fmax, U->underruns - und0, U->drops - drop0, x.gaps - gaps0, x.gaps);
    CHECK(U->underruns == und0, "%s: Unterlaeufe im eingeschwungenen Zustand", sc->name);
    CHECK(U->drops == drop0, "%s: verworfene Bloecke im eingeschwungenen Zustand", sc->name);
    CHECK(x.gaps == gaps0, "%s: Datenluecken im eingeschwungenen Zustand", sc->name);
    CHECK(fabs(mean - want) < 0.002, "%s: Ratenmittel %.5f statt %.5f", sc->name, mean, want);
    CHECK(fmin > 48 && fmax < 1024 - 128, "%s: Fuellstand [%u..%u]", sc->name, fmin, fmax);
    CHECK(nmin >= 42 && nmax <= 47, "%s: Paketgroessen [%ld..%ld]", sc->name, nmin, nmax);
    if (!sc->gap_len && !sc->restart_at) CHECK(x.gaps == 0, "%s: %ld Datenluecken insgesamt", sc->name, x.gaps);
    if (sc->gap_len || sc->restart_at) CHECK(x.gaps <= 3, "%s: %ld Datenluecken nach Stoerung", sc->name, x.gaps);
    return fails == f0;
}

static void test_tap(void) {
    /* audio_tap ruft den Original-Callback und schiebt danach genau dessen Block */
    memset(host_ram, 0, sizeof host_ram);
    usbinit_hook();
    gen_s = 1000;
    int16_t l[128], r[128];
    audio_tap(l, r);
    CHECK(U->head == 128 && U->blocks == 1, "audio_tap: head %u", U->head);
    const volatile uint32_t *ring = (const volatile uint32_t *)(void *)host_ram;
    CHECK(ring[0] == 1000u && ring[127] == 1127u, "audio_tap: Ringinhalt %08x %08x", ring[0], ring[127]);
    /* voller Puffer: weitere Bloecke verworfen, kein Ueberschreiben */
    for (int k = 0; k < 20; k++) audio_tap(l, r);
    CHECK(U->head == 1024, "Puffer muss bei 1024 Frames voll sein (head %u)", U->head);
    CHECK(ring[0] == 1000u, "voller Puffer darf nicht ueberschrieben werden");
    /* Muellzustand (head-tail > Ringgroesse) → Erzeuger schreibt nicht, Verbraucher leert */
    U->tail = U->head + 5000;
    audio_tap(l, r);
    CHECK(U->head == 1024, "Muellzustand: kein Schreiben");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (!f || fwrite(ua_cfg_desc, 1, CFG_LEN, f) != CFG_LEN) { perror(argv[1]); return 2; }
        fclose(f);
    }
    printf("Teil A: Setup/EP0/BDT\n");
    test_setup();
    test_tap();
    printf("Teil B: Simulation\n");
    const double FS = 180e6 * 16.0 / 255.0 / 256.0;
    const scen_t sc[] = {
        { "nominal",                         FS,              0, 0.0, 60, 0, 0, 0, 0.0 },
        { "Geraet +100 ppm, Host -300 ppm",  FS * 1.0001,  -300, 0.3, 60, 0, 0, 0, 0.0 },
        { "Geraet -200 ppm, Host +500 ppm",  FS * 0.9998,   500, 0.3, 60, 0, 0, 0, 0.0 },
        { "44100 exakt, Host 0",             44100.0,         0, 0.5, 60, 0, 0, 0, 0.0 },
        { "Extrem: 1000 ppm auseinander",    FS * 1.0005,   500, 0.8, 120, 0, 0, 0, 0.0 },
        { "Extrem: 3000 ppm auseinander",    FS * 0.9985, -1500, 0.5, 120, 0, 0, 0, 0.0 },
        { "ISR-Jitter 0,9 ms + Unterbrechung", FS,            0, 0.9, 60, 0, 0, 0, 0.05 },
        { "Host setzt 20 Frames aus",        FS,            100, 0.3, 60, 20000, 20, 0, 0.0 },
        { "Stop/Start (alt0→alt1)",          FS,           -100, 0.3, 60, 0, 0, 20000, 0.0 },
    };
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++)
        for (unsigned seed = 1; seed <= 3; seed++) run_scen(&sc[i], seed * 7919u + i);
    printf("%d Pruefungen, %d Fehler\n", checks, fails);
    return fails ? 1 : 0;
}
