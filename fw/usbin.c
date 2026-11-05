/* Patch 020 – USB-Audio-Eingang PC → Play (UAC 1.0, 44,1 kHz, 16 Bit Stereo), Polyend Play Rev A, Firmware 1.6.0
 * ENTWURF – nur am PC getestet.
 *
 * Idee (Details: Vault-Notiz Firmware/USB_Audio.md, Abschnitt Phase 2):
 *  - Menue "Audio Out" = "Stereo + In" (usbmenu.c) meldet das Play mit einem eigenen Deskriptor an (UI_DESC_IN, 323 B):
 *    Stereo-Ausgang IF4 (EP 0x86) wie 012 und neu IF5 = Wiedergabe: EP 0x06 OUT isochron asynchron (max. 49 Frames/ms),
 *    Rueckmelde-Endpunkt EP 0x87 IN (3 Byte, Format 10.14 Frames/ms, alle 32 ms abgefragt). Multitrack entfaellt in
 *    dieser Variante (USB-Bandbreite), deshalb sind IF5 und EP7 frei. Vorbild: Teensy usb_audio (gleicher USB-Baustein).
 *  - Taktabgleich: Der PC schickt so viele Frames, wie die Rueckmeldung verlangt. Die Rueckmeldung ist der Nenntakt des
 *    Play (44 117,6 Hz) plus ein P-Regler auf den gefilterten Fuellstand des Rings → kein Resampling im Geraet.
 *  - Erzeuger (USB-ISR, Prio 0x70): ui_rx_token schreibt die empfangenen Frames in einen Ring (512 Frames).
 *  - Verbraucher (Audio-ISR, Prio 0xD0): ui_pre holt vor audio_callback 128 Frames in IN_L/IN_R. Im Mixer-Update
 *    0x15884 wird das Signal an zwei Stellen eingespeist (nur waehrend der Audio-ISR, nie beim WAV-Export):
 *      0x15A3E ui_cb_hook:   Zusatzeingang des Mixers (Stream+0x150, AudioMixer_setInputCallback) → Master,
 *                            vor Limiter und Loop. Ein vorhandener Original-Rueckruf wird weiter aufgerufen und addiert.
 *      0x15CB6 ui_send_hook: Eingang von Delay (r5 = 8) bzw. Reverb (r5 = 9) + Send-Anteil des USB-Signals (mono).
 *    Der Mix geht wie immer ueber den Stereo-Ausgang zurueck zum PC (Loopback ohne Zusatzaufwand).
 *
 * RAM (SRAM_L 0x1FFF8800..0x1FFF97FF, vorher unreferenziert):
 *   0x1FFF8800 Ring 512 x u32 (L | R<<16) · 0x1FFF9000 Zustand ui_t (0x80) · 0x1FFF9080/9150 Empfangspuffer (je 196 B)
 *   0x1FFF9220/9224 Rueckmeldepuffer · 0x1FFF9240 IN_L · 0x1FFF9340 IN_R · 0x1FFF9440 Master L/R (2 x 256 B)
 *   0x1FFF9640 Send-Puffer (256 B) · Ende 0x1FFF9740
 *
 * Mit -DHOST_TEST wird gegen Attrappen uebersetzt (ui_test.c bindet die Datei direkt ein).
 */
#include <stdint.h>

void mt_ep_token(uint32_t stat);              /* multitrack.c */
void rec_capture(const int16_t *l, const int16_t *r) __attribute__((weak));   /* usbrec.c (Patch 024) */

#define UI_IF        5u
#define UI_EPADDR    0x06u
#define FB_EPADDR    0x87u
#define BDT_RX_EVEN  24u                       /* EP6 RX */
#define BDT_RX_ODD   25u
#define FB_BDT_EVEN  30u                       /* EP7 TX */
#define FB_BDT_ODD   31u
#define ENDPT_ISO_TX 0x14u                     /* EPCTLDIS | EPTXEN */
#define ENDPT_RXEN   0x08u
#define RX_MAX       196u                      /* = wMaxPacketSize: 49 Frames */
#define RING_FRAMES  512u
#define RING_MASK    (RING_FRAMES - 1u)
#define BLOCK        128u
#define TARGET       192u                      /* Soll-Fuellstand nach dem Entnehmen eines Blocks */
#define START_FILL   (TARGET + BLOCK)
#define NOM_FB       722824                    /* 44 117,647 Hz / 1000 * 2^14 (Format 10.14) */
#define FB_CLAMP     8192                      /* hoechstens +-0,5 Frames/ms Abweichung */
#define LPF_SHIFT    5
#define UI_MAGIC     0x55A0D120u
#define SEND_DEF     16384                     /* Send-Anteil Q16 (0,25) fuer Delay und Reverb */

enum { ST_OFF = 0, ST_PREFILL = 1, ST_RUN = 2 };

typedef struct {               /* per PEEK 1FFF9000.. lesbar */
    uint32_t magic;            /* 00 */
    uint32_t mode;             /* 04 1 = Deskriptor "Stereo + In" aktiv (usbmenu.c) */
    uint32_t alt;              /* 08 Alternate Setting IF5 */
    uint32_t state;            /* 0C ST_* */
    uint32_t head;             /* 10 Erzeuger (USB): Frames laufend */
    uint32_t tail;             /* 14 Verbraucher (Audio) */
    uint32_t rx_pkts;          /* 18 */
    uint32_t rx_frames;        /* 1C */
    uint32_t rx_odd;           /* 20 Pakete mit Laenge kein Vielfaches von 4 */
    uint32_t overruns;         /* 24 Frames verworfen (Ring voll) */
    uint32_t underruns;        /* 28 */
    uint32_t blocks;           /* 2C eingespeiste Bloecke */
    int32_t  favg;             /* 30 gefilterter Fuellstand Q8 */
    int32_t  fb;               /* 34 aktuelle Rueckmeldung 10.14 */
    uint32_t fb_pkts;          /* 38 */
    uint32_t starts;           /* 3C */
    uint32_t setups;           /* 40 */
    int32_t  send[2];          /* 44 Send-Anteil Q16: Delay, Reverb */
    uint32_t in_valid;         /* 4C 1 = IN_L/IN_R gilt fuer den laufenden Block (nur in der Audio-ISR) */
    uint32_t orig_cb;          /* 50 Zusatzeingang der Firmware (Stream+0x150), zuletzt gesehen */
    uint32_t orig_calls;       /* 54 wie oft der Original-Rueckruf mitlief */
    int32_t  peak;             /* 58 Spitzenpegel des Eingangs (abs, seit Start) */
    uint32_t _r[9];            /* 5C */
} ui_t;
_Static_assert(sizeof(ui_t) == 0x80, "ui_t");

/* ------------------------------------------------------------------ Hardware-/Firmware-Zugriffe */
#ifdef HOST_TEST
extern uint8_t  host_ui_ram[0xF40];           /* bildet 0x1FFF8800..0x1FFF973F nach */
extern uint32_t host_bdt_desc[32];
extern const void *host_bdt_ptr[32];
extern uint8_t  host_endpt6, host_endpt7;
#define UIRAM(o)    ((uintptr_t)(host_ui_ram + (o)))
#define BDT_SET(i, p, d) do { host_bdt_ptr[i] = (p); host_bdt_desc[i] = (d); } while (0)
#define BDT_DESC(i) (host_bdt_desc[i])
#define ENDPT6      host_endpt6
#define ENDPT7      host_endpt7
#define BARRIER()   __asm__ volatile("" ::: "memory")
#define FNADDR(f)   ((uint32_t)(uintptr_t)(f))
typedef void (*incb_t)(int32_t *out, uint32_t ctx);
#define CALL_CB(a, o, c) (((incb_t)(uintptr_t)(a))((o), (c)))
#else
#define UIRAM(o)    (0x1FFF8800u + (o))
#define BDT         ((volatile uint32_t *)0x1FFF0000u)
#define BDT_SET(i, p, d) do { BDT[2u * (i) + 1u] = (uint32_t)(p); __asm__ volatile("dmb" ::: "memory"); BDT[2u * (i)] = (d); } while (0)
#define BDT_DESC(i) (BDT[2u * (i)])
#define ENDPT6      (*(volatile uint8_t *)0x400720D8u)
#define ENDPT7      (*(volatile uint8_t *)0x400720DCu)
#define BARRIER()   __asm__ volatile("dmb" ::: "memory")
#define FNADDR(f)   ((uint32_t)(uintptr_t)(f) | 1u)
typedef void (*incb_t)(int32_t *out, uint32_t ctx);
#define CALL_CB(a, o, c) (((incb_t)(a))((o), (c)))
#endif

#define RING   ((volatile uint32_t *)UIRAM(0x000))
#define UI     ((volatile ui_t *)UIRAM(0x800))
#define RXBUF0 ((uint32_t *)UIRAM(0x880))
#define RXBUF1 ((uint32_t *)UIRAM(0x950))
#define FBBUF0 ((uint8_t *)UIRAM(0xA20))
#define FBBUF1 ((uint8_t *)UIRAM(0xA24))
#define IN_L   ((int16_t *)UIRAM(0xA40))
#define IN_R   ((int16_t *)UIRAM(0xB40))
#define MST    ((int16_t *)UIRAM(0xC40))      /* L[128], R[128] direkt dahinter (+0x100, wie die Firmware erwartet) */
#define SENDB  ((int16_t *)UIRAM(0xE40))

#define BDT_OWN 0x80u

/* ------------------------------------------------------------------ Konfigurationsdeskriptor "Stereo + In" */
#define UI_DESC_LEN 323u
const uint8_t UI_DESC_IN[UI_DESC_LEN] = {
    0x09, 0x02, UI_DESC_LEN & 0xFF, UI_DESC_LEN >> 8, 0x06, 0x01, 0x00, 0xC0, 0x32,   /* 6 Interfaces */
    /* ---- bis "Ende Original" byte-identisch mit 0xAED71..0xAEDFC (wie ua_cfg_desc) ---- */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x04,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x01, 0x01,
    0x04, 0x24, 0x02, 0x06,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x40,
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,
    0x09, 0x04, 0x02, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x00, 0x01, 0x25, 0x00,
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
    0x09, 0x05, 0x05, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x25, 0x01, 0x01, 0x01,
    0x09, 0x05, 0x84, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x25, 0x01, 0x01, 0x03,
    /* ---- Ende Original ---- */
    /* IF3 Audio Control */
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0A, 0x24, 0x01, 0x00, 0x01, 0x34, 0x00, 0x02, 0x04, UI_IF,          /* AC-Header, wTotalLength 52, AS-IF {4, 5} */
    0x0C, 0x24, 0x02, 0x01, 0x13, 0x07, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, /* IT 1: Synthesizer (0x0713), L/R */
    0x09, 0x24, 0x03, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00,       /* OT 2: USB Streaming, Quelle IT 1 */
    0x0C, 0x24, 0x02, 0x03, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, /* IT 3: USB Streaming (0x0101), L/R */
    0x09, 0x24, 0x03, 0x04, 0x01, 0x03, 0x00, 0x03, 0x00,       /* OT 4: Lautsprecher (0x0301), Quelle IT 3 */
    /* IF4 Stereo-Ausgang (wie 012) */
    0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x04, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x02, 0x01, 0x01, 0x00,
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x44, 0xAC, 0x00,
    0x09, 0x05, 0x86, 0x05, 0xC0, 0x00, 0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* IF5 Wiedergabe (PC → Play), Alt 0 */
    0x09, 0x04, UI_IF, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* IF5 Alt 1: 2 Endpunkte (Daten + Rueckmeldung) */
    0x09, 0x04, UI_IF, 0x01, 0x02, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x03, 0x01, 0x01, 0x00,                   /* AS General: Terminal 3, bDelay 1, PCM */
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x44, 0xAC, 0x00, /* Typ I, 2 Kanaele, 16 Bit, 44100 Hz */
    0x09, 0x05, UI_EPADDR, 0x05, RX_MAX, 0x00, 0x01, 0x00, FB_EPADDR, /* EP6 OUT isochron asynchron, 196 B, Sync EP 0x87 */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x09, 0x05, FB_EPADDR, 0x11, 0x03, 0x00, 0x01, 0x05, 0x00,  /* EP7 IN isochron Rueckmeldung, 3 B, bRefresh 2^5 ms */
};
_Static_assert(sizeof(UI_DESC_IN) == UI_DESC_LEN, "Deskriptorlaenge");

static int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* ------------------------------------------------------------------ Initialisierung / Steuerung */
void ui_init(void) {
    volatile uint32_t *z = (volatile uint32_t *)(void *)UI;
    for (uint32_t i = 0; i < sizeof(ui_t) / 4u; i++) z[i] = 0;
    for (uint32_t i = 0; i < RING_FRAMES; i++) RING[i] = 0;
    UI->fb = NOM_FB;
    UI->send[0] = SEND_DEF; UI->send[1] = SEND_DEF;
    BARRIER();
    UI->magic = UI_MAGIC;
}

static void ui_stream_stop(void) {
    volatile ui_t *u = UI;
    u->state = ST_OFF;
    u->alt = 0;
    ENDPT6 = (uint8_t)(ENDPT6 & ~ENDPT_RXEN);
    BDT_SET(BDT_RX_EVEN, RXBUF0, 0);
    BDT_SET(BDT_RX_ODD, RXBUF1, 0);
    BDT_SET(FB_BDT_EVEN, FBBUF0, 0);
    BDT_SET(FB_BDT_ODD, FBBUF1, 0);
}

static void fb_fill(uint8_t *b) {
    uint32_t v = (uint32_t)UI->fb;
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = 0;
}

static void ui_stream_start(void) {
    volatile ui_t *u = UI;
    u->tail = u->head;                         /* Ring leer */
    u->favg = (int32_t)(TARGET << 8);
    u->fb = NOM_FB;
    u->peak = 0;
    u->starts++;
    u->state = ST_PREFILL;
    u->alt = 1;
    ENDPT6 = (uint8_t)(ENDPT_ISO_TX | ENDPT_RXEN);    /* EP6: IN (Stereo, usbaudio.c) und OUT isochron */
    ENDPT7 = ENDPT_ISO_TX;
    BDT_SET(BDT_RX_EVEN, RXBUF0, (RX_MAX << 16) | BDT_OWN);
    BDT_SET(BDT_RX_ODD, RXBUF1, (RX_MAX << 16) | BDT_OWN);
    fb_fill(FBBUF0); fb_fill(FBBUF1);
    BDT_SET(FB_BDT_EVEN, FBBUF0, (3u << 16) | BDT_OWN);
    BDT_SET(FB_BDT_ODD, FBBUF1, (3u << 16) | BDT_OWN);
}

/* usbmenu.c: vor der Neuanmeldung (unter Interrupt-Sperre) bzw. danach mit der neuen Variante */
void ui_force_stop(void) { if (UI->magic == UI_MAGIC && UI->state != ST_OFF) ui_stream_stop(); }
void ui_set_mode(int on) { if (UI->magic == UI_MAGIC) { if (!on) ui_force_stop(); UI->mode = on ? 1u : 0u; } }

/* usbaudio.c: Bit EPRXEN, das stream_start/stop des Stereo-Ausgangs beim Schreiben von ENDPT6 erhalten muss */
uint32_t ui_ep6_rx(void) { return (UI->magic == UI_MAGIC && UI->state != ST_OFF) ? ENDPT_RXEN : 0u; }

/* ------------------------------------------------------------------ USB-ISR */
void ui_rx_token(uint32_t i) {                 /* EP6 RX fertig (BDT 24/25) */
    volatile ui_t *u = UI;
    uint32_t *buf = (i & 1u) ? RXBUF1 : RXBUF0;
    if (u->magic != UI_MAGIC || u->state == ST_OFF) { BDT_SET(i, buf, 0); return; }
    uint32_t len = (BDT_DESC(i) >> 16) & 0x3FFu;
    if (len & 3u) u->rx_odd++;
    if (len > RX_MAX) len = RX_MAX;
    uint32_t n = len >> 2;
    uint32_t head = u->head, used = head - u->tail;
    if (used > RING_FRAMES) used = RING_FRAMES;
    uint32_t room = RING_FRAMES - used;
    if (n > room) { u->overruns += n - room; n = room; }
    for (uint32_t k = 0; k < n; k++) RING[(head + k) & RING_MASK] = buf[k];
    BARRIER();
    u->head = head + n;
    u->rx_pkts++;
    u->rx_frames += n;
    BDT_SET(i, buf, (RX_MAX << 16) | BDT_OWN);
}

static void ui_fb_token(uint32_t stat) {       /* EP7 TX fertig: naechste Rueckmeldung */
    volatile ui_t *u = UI;
    uint32_t i = (stat >> 2) & 31u;
    if (i != FB_BDT_EVEN && i != FB_BDT_ODD) return;
    uint8_t *b = (i & 1u) ? FBBUF1 : FBBUF0;
    if (u->state == ST_OFF) { BDT_SET(i, b, 0); return; }
    fb_fill(b);
    u->fb_pkts++;
    BDT_SET(i, b, (3u << 16) | BDT_OWN);
}

/* ep_hook (usbaudio.c) fuer EP7: in der Variante "Stereo + In" Rueckmeldung, sonst Multitrack */
void ep7_token(uint32_t stat) {
    if (UI->magic == UI_MAGIC && UI->mode) ui_fb_token(stat);
    else mt_ep_token(stat);
}

/* Setup-Requests fuer IF5 / EP 0x06 (aus ua_setup, vor mt_setup). Nur in der Variante "Stereo + In" zustaendig.
 * Rueckgabe wie mt_setup: -1 nicht zustaendig, 0 STALL, 1 Statusstufe, 2 alt-Byte in out[0], 3 Rate in out[0..2]. */
int ui_setup(uint32_t req, uint32_t wValue, uint32_t wIndex, uint8_t *out) {
    volatile ui_t *u = UI;
    if (u->magic != UI_MAGIC || !u->mode) return -1;
    if (u->state != ST_OFF && !(ENDPT6 & ENDPT_RXEN)) ui_stream_stop();   /* SET_CONFIGURATION hat ENDPT6 neu gesetzt */
    switch (req) {
    case 0x0B01:                                            /* SET_INTERFACE */
        if (wIndex != UI_IF) return -1;
        if (wValue > 1u) return 0;
        if (wValue) ui_stream_start(); else ui_stream_stop();
        u->setups++;
        return 1;
    case 0x0A81:                                            /* GET_INTERFACE */
        if (wIndex != UI_IF) return -1;
        out[0] = (uint8_t)u->alt;
        u->setups++;
        return 2;
    case 0x0122:                                            /* SET_CUR Abtastrate an EP 0x06: annehmen, ignorieren */
        if ((wIndex & 0xFFu) != UI_EPADDR) return -1;
        if ((wValue >> 8) != 1u) return 0;
        u->setups++;
        return 1;
    case 0x81A2:
        if ((wIndex & 0xFFu) != UI_EPADDR) return -1;
        if ((wValue >> 8) != 1u) return 0;
        out[0] = 0x44; out[1] = 0xAC; out[2] = 0x00;
        u->setups++;
        return 3;
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ Audio-ISR (audio_tap) */
void ui_pre(void) {
    volatile ui_t *u = UI;
    if (u->magic != UI_MAGIC) return;
    u->in_valid = 0;
    if (u->state == ST_OFF) return;
    uint32_t tail = u->tail, avail = u->head - tail;
    if (avail > RING_FRAMES) { u->tail = u->head; return; }  /* inkonsistent → leeren */
    if (u->state == ST_PREFILL) {
        if (avail < START_FILL) return;
        u->state = ST_RUN;
    }
    if (avail < BLOCK) { u->underruns++; u->state = ST_PREFILL; return; }
    int16_t *l = IN_L, *r = IN_R;
    int32_t pk = u->peak;
    for (uint32_t k = 0; k < BLOCK; k++) {
        uint32_t f = RING[(tail + k) & RING_MASK];
        int16_t a = (int16_t)(f & 0xFFFFu), b = (int16_t)(f >> 16);
        l[k] = a; r[k] = b;
        int32_t m = a < 0 ? -a : a; if (m > pk) pk = m;
        m = b < 0 ? -b : b; if (m > pk) pk = m;
    }
    u->peak = pk;
    BARRIER();
    u->tail = tail + BLOCK;
    avail -= BLOCK;
    /* Rueckmeldung: Nenntakt + P-Regler auf den Fuellstand (1 Frame Abweichung → 1/512 Frame/ms) */
    int32_t favg = u->favg;
    favg += (((int32_t)avail << 8) - favg) >> LPF_SHIFT;
    u->favg = favg;
    int32_t adj = (favg - (int32_t)(TARGET << 8)) >> 3;
    if (adj > FB_CLAMP) adj = FB_CLAMP;
    if (adj < -FB_CLAMP) adj = -FB_CLAMP;
    u->fb = NOM_FB - adj;
    u->blocks++;
    if (rec_capture) rec_capture(l, r);                     /* 024: Aufnahme auf SD */
    u->in_valid = 1;
}

void ui_post(void) { if (UI->magic == UI_MAGIC) UI->in_valid = 0; }

/* ------------------------------------------------------------------ Mixer-Update (innerhalb von audio_callback) */
/* Zusatzeingang: out[0] = Kanalzahl, out[1] = int16 L[128] (R bei +0x100). */
static void ui_mix_cb(int32_t *out, uint32_t ctx) {
    volatile ui_t *u = UI;
    const int16_t *sl = 0, *sr = 0;
    if (u->orig_cb) {
        int32_t t[2] = { 0, 0 };
        CALL_CB(u->orig_cb, t, ctx);
        if (t[0] > 0 && t[1]) {
            sl = (const int16_t *)(uintptr_t)t[1];
            sr = t[0] > 1 ? sl + BLOCK : sl;
            u->orig_calls++;
        }
    }
    int16_t *ml = MST, *mr = MST + BLOCK;
    const int16_t *il = IN_L, *ir = IN_R;
    for (uint32_t k = 0; k < BLOCK; k++) {
        int32_t a = il[k], b = ir[k];
        if (sl) { a += sl[k]; b += sr[k]; }
        ml[k] = sat16(a); mr[k] = sat16(b);
    }
    out[0] = 2;
    out[1] = (int32_t)(uintptr_t)MST;
}

/* liefert den Rueckruf, den der Mixer aufrufen soll (0 = keiner, wie im Original) */
uint32_t ui_select(uint32_t mixer) {
    volatile ui_t *u = UI;
    uint32_t orig = *(volatile uint32_t *)(uintptr_t)(mixer + 0x150u);
    if (u->magic != UI_MAGIC) return orig;
    u->orig_cb = orig;
    if (!u->in_valid) return orig;
    return FNADDR(ui_mix_cb);
}

/* Eingang eines Send-Effekts (which 8 = Delay, 9 = Reverb): eigener Puffer = Original + Anteil USB mono */
const int16_t *ui_send(const int16_t *src, uint32_t which) {
    volatile ui_t *u = UI;
    if (u->magic != UI_MAGIC || !u->in_valid || (which != 8u && which != 9u)) return src;
    int32_t g = u->send[which - 8u];
    if (g <= 0) return src;
    if (g > 65536) g = 65536;
    int16_t *d = SENDB;
    const int16_t *il = IN_L, *ir = IN_R;
    for (uint32_t k = 0; k < BLOCK; k++) {
        int32_t m = ((int32_t)il[k] + ir[k]) >> 1;
        d[k] = sat16(src[k] + ((m * g) >> 16));
    }
    return d;
}

#ifndef HOST_TEST
/* 0x15A3E: "ldr.w r3,[sl,#0x150]" → bl ui_cb_hook. sl = Mixer-AudioStream. Ergebnis in r3, alles andere bleibt. */
__attribute__((naked, used, section(".text.entry_uicb")))
void ui_cb_hook(void) {
    __asm__ volatile(
        "push  {r0, r1, r2, r4, ip, lr}\n"
        "mov   r0, sl\n"
        "bl    ui_select\n"
        "mov   r3, r0\n"
        "pop   {r0, r1, r2, r4, ip, lr}\n"
        "bx    lr\n");
}

/* 0x15CB6: "mov r2,r6; mov r3,r4" (vor blx r7 = Send-Effekt) → bl ui_send_hook. r1 = Effekteingang, r5 = 8/9. */
__attribute__((naked, used, section(".text.entry_uisend")))
void ui_send_hook(void) {
    __asm__ volatile(
        "mov   r2, r6\n"
        "mov   r3, r4\n"
        "push  {r0, r2, r3, r4, ip, lr}\n"
        "mov   r0, r1\n"
        "mov   r1, r5\n"
        "bl    ui_send\n"
        "mov   r1, r0\n"
        "pop   {r0, r2, r3, r4, ip, lr}\n"
        "bx    lr\n");
}
#endif
