/* Patch 015 (Basis 012) – USB-Audio Play → PC (UAC 1.0, 44,1 kHz, 16 Bit): Stereo-Master (IF4, wie 012) und
 * Multitrack (IF5, multitrack.c), Polyend Play Rev A, Firmware 1.6.0
 * ENTWURF – 015 nur am PC getestet. Der Stereo-Teil (012) ist am Geraet bestaetigt (03.11.2026).
 * Aenderungen gegenueber 012: TARGET 512 → 256, Deskriptor um IF5 erweitert (357 B, RAM 0x1FFFD4C0),
 * ep_hook auch fuer EP7, ua_setup reicht IF5/EP 0x87 an mt_setup weiter, audio_tap ruft mt_pre/mt_post.
 *
 * Idee (Details: Vault-Notiz Firmware/USB_Audio.md):
 *  - Das (im Geraet ungenutzte) RawHID-Interface 3 (EP6 IN / EP7 OUT) wird im Konfigurationsdeskriptor ersetzt durch
 *    Audio Control (Interface 3) + Audio Streaming (Interface 4, Alt 0 = aus, Alt 1 = EP 0x86 isochron, asynchron).
 *    CDC (0/1) und USB-MIDI (2) bleiben byte-identisch.
 *  - Der Deskriptor muss im RAM liegen (der USB-DMA darf laut FMC_PFAPR-Resetwert nicht aus dem Flash lesen; Teensy
 *    legt deshalb alle Deskriptoren in .data). usbinit_hook kopiert ihn vor usb_init() nach 0x1FFFD300, der Eintrag in
 *    usb_descriptor_list (0x9BACC) zeigt dorthin.
 *  - audio_tap ersetzt den Audio-Callback (Literal 0xBD58 in setup()): ruft audio_callback 0xBA18 und kopiert danach
 *    die 128 Frames L/R in einen Ringpuffer (1024 Frames, SRAM_L).
 *  - usbotg_isr 0x12B9C wird an zwei Stellen umgeleitet:
 *      0x12BAC (Token fuer Endpunkt != 0)  → ep_hook: EP6 (und in 015 EP7) behandeln wir selbst (isochron, eigene Puffer)
 *      0x12DEC (unbekannter Setup-Request → STALL) → setup_hook: SET/GET_INTERFACE, SET/GET_CUR Abtastrate
 *  - Taktabgleich: asynchroner IN-Endpunkt. Pro USB-Frame (1 ms) werden so viele Frames gesendet, wie der Fuellstand
 *    des Ringpuffers verlangt (P-Regler auf den tiefpassgefilterten Fuellstand, nominal 44,1176 Frames/ms).
 *    Der Host misst die Rate implizit – kein Feedback-Endpunkt, kein Resampling noetig.
 *
 * RAM (SRAM_L, fuer diesen Patch reserviert ab 0x1FFFC000):
 *   0x1FFFC000 Ringpuffer 1024 x u32 (L | R<<16) · 0x1FFFD000 Zustand ua_t · 0x1FFFD100/0x1FFFD1C0 Sendepuffer gerade/ungerade
 *   0x1FFFD280 EP0-Antwortpuffer · (0x1FFFD300..0x1FFFD3FF frei, Deskriptor liegt in 015 bei 0x1FFFD4C0)
 *
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt (ua_test.c).
 */
#include <stdint.h>

/* multitrack.c (Patch 015) */
void mt_init(void);
void mt_pre(void);
void mt_post(const int16_t *l, const int16_t *r);
void mt_ep_token(uint32_t stat);
void um_early_init(void);                  /* usbmenu.c (Patch 017) */
void rec_init(void) __attribute__((weak)); /* usbrec.c (Patch 024) */
int  mt_setup(uint32_t req, uint32_t wValue, uint32_t wIndex, uint8_t *out);
/* usbin.c (Patch 020); -DNO_UI fuer die alten Host-Tests 1/2 ohne USB-Eingang */
#ifdef NO_UI
static inline void ui_init(void) {}
static inline void ui_pre(void) {}
static inline void ui_post(void) {}
static inline void ui_rx_token(uint32_t i) { (void)i; }
static inline uint32_t ui_ep6_rx(void) { return 0; }
static inline int  ui_setup(uint32_t q, uint32_t v, uint32_t x, uint8_t *o) { (void)q; (void)v; (void)x; (void)o; return -1; }
#else
void ui_init(void);
void ui_pre(void);
void ui_post(void);
void ui_rx_token(uint32_t i);
uint32_t ui_ep6_rx(void);
int  ui_setup(uint32_t req, uint32_t wValue, uint32_t wIndex, uint8_t *out);
#endif

#define UA_IF_AC     3u
#define UA_IF_AS     4u
#define UA_EP        6u
#define UA_EPADDR    0x86u
#define BDT_TX_EVEN  (UA_EP * 4u + 2u)     /* 26 */
#define BDT_TX_ODD   (UA_EP * 4u + 3u)     /* 27 */
#define ENDPT_ISO_TX 0x14u                 /* EPCTLDIS | EPTXEN, ohne EPHSHK (isochron) */

#define RING_FRAMES  1024u
#define RING_MASK    (RING_FRAMES - 1u)
#define TARGET       256u                  /* Soll-Fuellstand in Frames (~5,8 ms; am Geraet: 256 reicht, Aussetzer beim SD-Schreiben kommen von der Firmware) */
#define MAXF         48u                   /* = wMaxPacketSize 192 / 4 */
#define NOM_Q16      2891294u              /* 44117,647 Hz / 1000 * 65536 (I2S: 180 MHz*16/255/256) */
#define LPF_SHIFT    5                     /* Fuellstand-Tiefpass, ~32 ms */
#define RATE_CLAMP   (2 << 16)             /* Regler: hoechstens +-2 Frames/ms Abweichung */
#define UA_MAGIC     0x55A0D10Au
#define BLOCK        128u

enum { ST_OFF = 0, ST_PREFILL = 1, ST_RUN = 2 };

typedef struct {               /* per PEEK 1FFFD000.. lesbar (Patch 011/usbup) */
    uint32_t magic;            /* 00 */
    uint32_t head;             /* 04 Erzeuger: geschriebene Frames (laufend) */
    uint32_t tail;             /* 08 Verbraucher: gelesene Frames (laufend) */
    uint32_t alt;              /* 0C Alternate Setting Interface 4 */
    uint32_t state;            /* 10 ST_* */
    uint32_t blocks;           /* 14 Audio-Bloecke gesehen */
    uint32_t drops;            /* 18 verworfene Bloecke waehrend ST_RUN (Puffer voll) */
    uint32_t packets;          /* 1C vorbereitete isochrone Pakete */
    uint32_t underruns;        /* 20 */
    uint32_t frames;           /* 24 gesendete Frames */
    int32_t  favg;             /* 28 gefilterter Fuellstand, Q8 */
    int32_t  rate;             /* 2C aktuelle Rate, Q16 Frames/ms */
    uint32_t acc;              /* 30 Phasenakkumulator Q16 */
    uint32_t last_n;           /* 34 Frames im letzten Paket */
    uint32_t fill_min;         /* 38 seit Start, nur ST_RUN */
    uint32_t fill_max;         /* 3C */
    uint32_t starts;           /* 40 SET_INTERFACE alt 1 */
    uint32_t setups;           /* 44 von uns beantwortete Setup-Requests */
} ua_t;

/* ------------------------------------------------------------------ Konfigurationsdeskriptor */
#define CFG_LEN 357u
#define MT_IFNUM 5u
#define MT_EPA   0x87u
const uint8_t ua_cfg_desc[CFG_LEN] = {
    /* Konfiguration: wTotalLength 357, 6 Interfaces (Original: 181, 4; 012: 240, 5) */
    0x09, 0x02, CFG_LEN & 0xFF, CFG_LEN >> 8, 0x06, 0x01, 0x00, 0xC0, 0x32,
    /* ---- ab hier bis "Ende Original" byte-identisch mit 0xAED71..0xAEDFC ---- */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x04,             /* IAD CDC 0..1 */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,       /* IF0 CDC Comm */
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x01, 0x01,
    0x04, 0x24, 0x02, 0x06,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x40,                   /* EP1 IN Interrupt */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,       /* IF1 CDC Data */
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,                   /* EP2 OUT Bulk */
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,                   /* EP3 IN Bulk */
    0x09, 0x04, 0x02, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,       /* IF2 MIDI Streaming */
    0x07, 0x24, 0x01, 0x00, 0x01, 0x25, 0x00,
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
    0x09, 0x05, 0x05, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,       /* EP5 OUT Bulk (MIDI) */
    0x05, 0x25, 0x01, 0x01, 0x01,
    0x09, 0x05, 0x84, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,       /* EP4 IN Bulk (MIDI) */
    0x05, 0x25, 0x01, 0x01, 0x03,
    /* ---- Ende Original (HID-Interface 3 mit EP6/EP7 entfaellt) ---- */
    /* IF3 Audio Control */
    0x09, 0x04, UA_IF_AC, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0A, 0x24, 0x01, 0x00, 0x01, 0x34, 0x00, 0x02, UA_IF_AS, MT_IFNUM, /* AC-Header bcdADC 1.00, wTotalLength 52, AS-IF {4, 5} */
    0x0C, 0x24, 0x02, 0x01, 0x13, 0x07, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, /* IT 1: Synthesizer (0x0713), 2 Kanaele L/R */
    0x09, 0x24, 0x03, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00,       /* OT 2: USB Streaming, Quelle IT 1 */
    0x0C, 0x24, 0x02, 0x03, 0x12, 0x07, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x00, /* IT 3: Multi-track Recorder (0x0712), 11 Kan., ohne Raumlage */
    0x09, 0x24, 0x03, 0x04, 0x01, 0x01, 0x00, 0x03, 0x00,       /* OT 4: USB Streaming, Quelle IT 3 */
    /* IF4 Audio Streaming (Stereo-Master wie 012), Alt 0 */
    0x09, 0x04, UA_IF_AS, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* IF4 Alt 1 */
    0x09, 0x04, UA_IF_AS, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x02, 0x01, 0x01, 0x00,                   /* AS General: Terminal 2, bDelay 1, PCM */
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x44, 0xAC, 0x00, /* Typ I, 2 Kanaele, 16 Bit, 44100 Hz */
    0x09, 0x05, UA_EPADDR, 0x05, MAXF * 4u, 0x00, 0x01, 0x00, 0x00,   /* EP6 IN isochron asynchron, 192 B, 1 ms */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,                   /* CS-Endpunkt: keine Controls */
    /* IF5 Audio Streaming (Multitrack), Alt 0 */
    0x09, 0x04, MT_IFNUM, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* IF5 Alt 1: 10 Kanaele (Preset A: Spur 1-8 + Master L/R, Preset B: Spur 1-8 + FX L/R) */
    0x09, 0x04, MT_IFNUM, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x04, 0x01, 0x01, 0x00,                   /* AS General: Terminal 4 */
    0x0B, 0x24, 0x02, 0x01, 0x0A, 0x02, 0x10, 0x01, 0x44, 0xAC, 0x00, /* Typ I, 10 Kanaele, 16 Bit, 44100 Hz */
    0x09, 0x05, MT_EPA, 0x05, 0x84, 0x03, 0x01, 0x00, 0x00,     /* EP7 IN isochron asynchron, 900 B (45 Frames) */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* IF5 Alt 2: 11 Kanaele (Preset A: + FX mono, Preset B: + Master mono) */
    0x09, 0x04, MT_IFNUM, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x04, 0x01, 0x01, 0x00,
    0x0B, 0x24, 0x02, 0x01, 0x0B, 0x02, 0x10, 0x01, 0x44, 0xAC, 0x00, /* Typ I, 11 Kanaele */
    0x09, 0x05, MT_EPA, 0x05, 0xDE, 0x03, 0x01, 0x00, 0x00,     /* EP7 IN isochron asynchron, 990 B (45 Frames) */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof(ua_cfg_desc) == CFG_LEN, "Deskriptorlaenge");

/* ------------------------------------------------------------------ Hardware-/Firmware-Zugriffe */
#ifdef HOST_TEST
extern uint8_t  host_ram[0x3F00];          /* bildet 0x1FFFC000..0x1FFFFF00 nach (012 + 015) */
extern uint32_t host_bdt_desc[32];
extern const void *host_bdt_ptr[32];
extern uint32_t host_setup[2];
extern uint8_t  host_ep0_bank, host_ep0_toggle, host_endpt6, host_endpt7;
extern void host_usb_init(void);
extern void host_audio_cb(int16_t *l, int16_t *r);
#define RING        ((volatile uint32_t *)(void *)(host_ram + 0x0000))
#define UA          ((volatile ua_t *)(void *)(host_ram + 0x1000))
#define TXBUF0      ((uint32_t *)(void *)(host_ram + 0x1100))
#define TXBUF1      ((uint32_t *)(void *)(host_ram + 0x11C0))
#define EP0BUF      ((uint8_t *)(host_ram + 0x1280))
#define DESC_RAM    ((uint8_t *)(host_ram + 0x14C0))
#define BDT_SET(i, p, d) do { host_bdt_ptr[i] = (p); host_bdt_desc[i] = (d); } while (0)
#define SETUP_W0    (host_setup[0])
#define SETUP_W1    (host_setup[1])
#define EP0_BANK    host_ep0_bank
#define EP0_TOGGLE  host_ep0_toggle
#define ENDPT6      host_endpt6
#define USB_INIT()  host_usb_init()
#define AUDIO_CB(l, r) host_audio_cb((l), (r))
#define BARRIER()   __asm__ volatile("" ::: "memory")
#else
#define RING        ((volatile uint32_t *)0x1FFFC000u)
#define UA          ((volatile ua_t *)0x1FFFD000u)
#define TXBUF0      ((uint32_t *)0x1FFFD100u)
#define TXBUF1      ((uint32_t *)0x1FFFD1C0u)
#define EP0BUF      ((uint8_t *)0x1FFFD280u)
#define DESC_RAM    ((uint8_t *)0x1FFFD4C0u)          /* 015: im Multitrack-Bereich */
#define BDT         ((volatile uint32_t *)0x1FFF0000u)          /* Teensy-BDT, 8 Endpunkte x 4 Eintraege */
#define BDT_SET(i, p, d) do { BDT[2u * (i) + 1u] = (uint32_t)(p); __asm__ volatile("dmb" ::: "memory"); BDT[2u * (i)] = (d); } while (0)
#define SETUP_W0    (*(volatile uint32_t *)0x20000D20u)          /* setup.word1: bmRequestType|bRequest|wValue */
#define SETUP_W1    (*(volatile uint32_t *)0x20000D24u)          /* setup.word2: wIndex|wLength */
#define EP0_BANK    (*(volatile uint8_t *)0x20000CD8u)           /* ep0_tx_bdt_bank */
#define EP0_TOGGLE  (*(volatile uint8_t *)0x20000CD9u)           /* ep0_tx_data_toggle */
#define ENDPT6      (*(volatile uint8_t *)0x400720D8u)           /* USB0_ENDPT6 */
#define USB_INIT()  (((void (*)(void))0x00013529u)())             /* usb_init 0x13528 */
#define AUDIO_CB(l, r) (((void (*)(int16_t *, int16_t *))0x0000BA19u)((l), (r)))  /* audio_callback 0xBA18 */
#define BARRIER()   __asm__ volatile("dmb" ::: "memory")
#endif

#define BDT_OWN   0x80u
#define BDT_DATA1 0x40u
#define BDT_DTS   0x08u

/* ------------------------------------------------------------------ Initialisierung (vor usb_init, aus ResetHandler-Kette) */
void ua_init(void) {
    volatile uint8_t *d = DESC_RAM;
    for (uint32_t i = 0; i < CFG_LEN; i++) d[i] = ua_cfg_desc[i];
    volatile uint32_t *z = (volatile uint32_t *)(void *)UA;
    for (uint32_t i = 0; i < sizeof(ua_t) / 4u; i++) z[i] = 0;
    for (uint32_t i = 0; i < RING_FRAMES; i++) RING[i] = 0;
    UA->fill_min = 0xFFFFFFFFu;
    BARRIER();
    UA->magic = UA_MAGIC;
}

/* ------------------------------------------------------------------ Erzeuger (Audio-DMA-ISR, Prioritaet 0xD0) */
void ua_push(const int16_t *l, const int16_t *r, uint32_t n) {
    volatile ua_t *u = UA;
    if (u->magic != UA_MAGIC) return;
    uint32_t head = u->head;
    uint32_t used = head - u->tail;
    u->blocks++;
    if (used > RING_FRAMES || RING_FRAMES - used < n) {      /* voll: Block verwerfen (tail gehoert dem Verbraucher) */
        if (u->state == ST_RUN) u->drops++;
        return;
    }
    for (uint32_t k = 0; k < n; k++)
        RING[(head + k) & RING_MASK] = (uint32_t)(uint16_t)l[k] | ((uint32_t)(uint16_t)r[k] << 16);
    BARRIER();                                               /* erst Daten, dann head */
    u->head = head + n;
}

/* ------------------------------------------------------------------ Verbraucher (usbotg_isr, Prioritaet 0x70) */
static uint32_t make_packet(uint32_t *buf) {
    volatile ua_t *u = UA;
    uint32_t head = u->head, tail = u->tail;
    uint32_t avail = head - tail;
    if (avail > RING_FRAMES) { tail = head; avail = 0; }   /* inkonsistent → leeren */
    if (u->state == ST_PREFILL) {
        if (avail < TARGET) { u->tail = tail; u->last_n = 0; return 0; }   /* Nullpaket bis genug da ist */
        if (avail > TARGET + BLOCK) { tail = head - TARGET; avail = TARGET; }
        u->state = ST_RUN;
        u->favg = (int32_t)(avail << 8);
        u->acc = 0;
    } else if (u->state != ST_RUN) {
        return 0;
    }
    if (avail < u->fill_min) u->fill_min = avail;
    if (avail > u->fill_max) u->fill_max = avail;
    int32_t favg = u->favg;
    favg += (((int32_t)avail << 8) - favg) >> LPF_SHIFT;
    u->favg = favg;
    int32_t err = favg - (int32_t)(TARGET << 8);             /* Q8 Frames */
    int32_t adj = err >> 1;                                  /* 1 Frame Abweichung → 1/512 Frame/ms */
    if (adj > RATE_CLAMP) adj = RATE_CLAMP;
    if (adj < -RATE_CLAMP) adj = -RATE_CLAMP;
    int32_t rate = (int32_t)NOM_Q16 + adj;
    u->rate = rate;
    uint32_t acc = u->acc + (uint32_t)rate;
    uint32_t n = acc >> 16;
    u->acc = acc & 0xFFFFu;
    if (n > MAXF) n = MAXF;
    if (n > avail) { u->underruns++; n = avail; u->state = ST_PREFILL; }
    for (uint32_t k = 0; k < n; k++) buf[k] = RING[(tail + k) & RING_MASK];
    u->tail = tail + n;
    u->frames += n;
    u->last_n = n;
    return n * 4u;
}

static void stream_start(void) {
    volatile ua_t *u = UA;
    u->tail = u->head;                       /* nur frische Daten; ein halb geschriebener Block wird spaeter gueltig */
    u->acc = 0; u->favg = 0; u->rate = (int32_t)NOM_Q16; u->last_n = 0;
    u->fill_min = 0xFFFFFFFFu; u->fill_max = 0;
    u->underruns = 0; u->drops = 0;
    u->starts++;
    u->state = ST_PREFILL;
    ENDPT6 = (uint8_t)(ENDPT_ISO_TX | ui_ep6_rx());          /* 020: EP6-OUT (usbin.c) nicht abschalten */
    BDT_SET(BDT_TX_EVEN, TXBUF0, BDT_OWN);   /* beide Baenke mit Nullpaket vorbelegen (Ping-Pong) */
    BDT_SET(BDT_TX_ODD, TXBUF1, BDT_OWN);
}

static void stream_stop(void) {
    UA->state = ST_OFF;
    ENDPT6 = (uint8_t)(ENDPT_ISO_TX | ui_ep6_rx());          /* 020: EP6-OUT (usbin.c) nicht abschalten */
    BDT_SET(BDT_TX_EVEN, TXBUF0, 0);
    BDT_SET(BDT_TX_ODD, TXBUF1, 0);
}

/* 017: vor einer Neuanmeldung (usbmenu.c, unter Interrupt-Sperre) – wie SET_INTERFACE alt 0 */
void ua_force_stop(void) {
    if (UA->magic != UA_MAGIC) return;
    stream_stop();
    UA->alt = 0;
}

/* Token fertig auf EP6 (aus ep_hook, STAT-Byte). */
void ua_ep_token(uint32_t stat) {
    volatile ua_t *u = UA;
    uint32_t i = (stat >> 2) & 31u;
    if (i == BDT_TX_EVEN - 2u || i == BDT_TX_ODD - 2u) { ui_rx_token(i); return; }   /* 020: EP6 RX = USB-Eingang */
    if (i != BDT_TX_EVEN && i != BDT_TX_ODD) { BDT_SET(i, 0, 0); return; }
    uint32_t *buf = (i & 1u) ? TXBUF1 : TXBUF0;
    if (u->magic != UA_MAGIC || u->state == ST_OFF) { BDT_SET(i, buf, 0); return; }
    uint32_t len = make_packet(buf);
    u->packets++;
    BDT_SET(i, buf, (len << 16) | BDT_OWN);  /* isochron: DATA0, ohne DTS */
}

/* ------------------------------------------------------------------ EP0 (Setup-Requests, die Teensy nicht kennt) */
static void ep0_tx(const void *p, uint32_t len) {          /* wie Teensy endpoint0_transmit */
    uint32_t i = 2u | (EP0_BANK & 1u);
    BDT_SET(i, p, (len << 16) | (EP0_TOGGLE ? (BDT_OWN | BDT_DATA1 | BDT_DTS) : (BDT_OWN | BDT_DTS)));
    EP0_TOGGLE ^= 1u;
    EP0_BANK ^= 1u;
}

/* 1 = beantwortet (danach weiter wie nach einem normalen Setup), 0 = STALL wie bisher. */
int ua_setup(void) {
    volatile ua_t *u = UA;
    if (u->magic != UA_MAGIC) return 0;
    uint32_t w0 = SETUP_W0, w1 = SETUP_W1;
    uint32_t req = w0 & 0xFFFFu, wValue = w0 >> 16, wIndex = w1 & 0xFFFFu, wLength = w1 >> 16;
    if ((ENDPT6 & ~0x08u) != ENDPT_ISO_TX) { u->alt = 0; u->state = ST_OFF; }   /* SET_CONFIGURATION hat EP6 neu (0x15) gesetzt; Bit 3 = EP6-OUT (020) */
    {                                                       /* 020: IF5 / EP 0x06 in der Variante "Stereo + In" */
        int m = ui_setup(req, wValue, wIndex, EP0BUF);
        if (m == 0) return 0;
        if (m == 1) { ep0_tx(0, 0); u->setups++; return 1; }
        if (m == 2) { ep0_tx(EP0BUF, wLength < 1u ? wLength : 1u); u->setups++; return 1; }
        if (m == 3) { ep0_tx(EP0BUF, wLength < 3u ? wLength : 3u); u->setups++; return 1; }
    }
    {                                                       /* 015: IF5 / EP 0x87 (Multitrack) */
        int m = mt_setup(req, wValue, wIndex, EP0BUF);
        if (m == 0) return 0;
        if (m == 1) { ep0_tx(0, 0); u->setups++; return 1; }
        if (m == 2) { ep0_tx(EP0BUF, wLength < 1u ? wLength : 1u); u->setups++; return 1; }
        if (m == 3) { ep0_tx(EP0BUF, wLength < 3u ? wLength : 3u); u->setups++; return 1; }
    }
    switch (req) {
    case 0x0B01:                                            /* SET_INTERFACE */
        if (wIndex == UA_IF_AS && wValue <= 1u) {
            if (wValue) stream_start(); else stream_stop();
            u->alt = wValue;
            break;
        }
        if (wIndex == UA_IF_AC && wValue == 0u) break;
        return 0;
    case 0x0A81:                                            /* GET_INTERFACE */
        if (wIndex != UA_IF_AS && wIndex != UA_IF_AC) return 0;
        EP0BUF[0] = (wIndex == UA_IF_AS) ? (uint8_t)u->alt : 0u;
        ep0_tx(EP0BUF, wLength < 1u ? wLength : 1u);
        u->setups++;
        return 1;
    case 0x0122:                                            /* SET_CUR an Endpunkt (Abtastrate): annehmen, ignorieren */
        if ((wIndex & 0xFFu) == UA_EPADDR && (wValue >> 8) == 1u) break;
        return 0;
    case 0x81A2:                                            /* GET_CUR an Endpunkt (Abtastrate) */
        if ((wIndex & 0xFFu) == UA_EPADDR && (wValue >> 8) == 1u) {
            EP0BUF[0] = 0x44; EP0BUF[1] = 0xAC; EP0BUF[2] = 0x00;
            ep0_tx(EP0BUF, wLength < 3u ? wLength : 3u);
            u->setups++;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
    ep0_tx(0, 0);                                           /* Statusstufe: Nullpaket DATA1 */
    u->setups++;
    return 1;
}

/* ------------------------------------------------------------------ Einsprungpunkte */
__attribute__((used, section(".text.entry_audio")))
void audio_tap(int16_t *l, int16_t *r) {                   /* ersetzt audio_callback (Literal 0xBD58) */
    ui_pre();                                               /* 020: USB-Eingang fuer diesen Block (Hooks im Mixer) */
    mt_pre();                                               /* 015: Multitrack-Block reservieren (Hooks im Mixer) */
    AUDIO_CB(l, r);
    mt_post(l, r);                                          /* 015: Master eintragen, Block freigeben */
    ui_post();
    ua_push(l, r, BLOCK);
}

__attribute__((used, section(".text.entry_usbinit")))
void usbinit_hook(void) {                                   /* ersetzt "bl usb_init" bei 0x13810 */
#ifndef NO_UM
    um_early_init();                                        /* 017: Menuewerte, Deskriptorliste im RAM, Effektnamen */
#endif
    ua_init();
    mt_init();
    ui_init();                                              /* 020 */
    if (rec_init) rec_init();                               /* 024 (nach mt_init: liegt im Multitrack-Bereich) */
    USB_INIT();
}

#ifndef HOST_TEST
/* 0x12BAC: "ldr r5,=0x1FFF0000; lsrs r6,r3,#2" → bl ep_hook.  r2 = Endpunkt (STAT>>4), r3 = STAT.
 * EP6 (Stereo, ua_ep_token) und EP7 (Multitrack, mt_ep_token): selbst behandeln, dann weiter bei 0x12C24
 * (ISTAT = TOKDNE, Schleife). Sonst die zwei Befehle nachbilden. */
__attribute__((naked, used, section(".text.entry_ep")))
void ep_hook(void) {
    __asm__ volatile(
        "cmp   r2, #6\n"
        "beq   1f\n"
        "cmp   r2, #7\n"
        "beq   2f\n"
        "ldr   r5, =0x1FFF0000\n"
        "lsrs  r6, r3, #2\n"
        "bx    lr\n"
        "1:\n"
        "push  {r4, lr}\n"
        "mov   r0, r3\n"
        "bl    ua_ep_token\n"
        "pop   {r4, lr}\n"
        "ldr   r0, =0x00012C25\n"
        "bx    r0\n"
        "2:\n"
        "push  {r4, lr}\n"
        "mov   r0, r3\n"
        "bl    ep7_token\n"                                    /* 020: usbin.c verteilt auf Multitrack / Rueckmeldung */
        "pop   {r4, lr}\n"
        "ldr   r0, =0x00012C25\n"
        "bx    r0\n"
        ".ltorg\n");
}

/* 0x12DEC: "ldr r3,=0x40072000; movs r2,#15" (STALL) → bl setup_hook.
 * Beantwortet: weiter bei 0x12CBE (CTL = USBENSOFEN, ISTAT = TOKDNE). Sonst die zwei Befehle nachbilden → STALL. */
__attribute__((naked, used, section(".text.entry_setup")))
void setup_hook(void) {
    __asm__ volatile(
        "push  {r4, lr}\n"
        "bl    ua_setup\n"
        "pop   {r4, lr}\n"
        "cmp   r0, #0\n"
        "bne   1f\n"
        "ldr   r3, =0x40072000\n"
        "movs  r2, #15\n"
        "bx    lr\n"
        "1:\n"
        "ldr   r0, =0x00012CBF\n"
        "bx    r0\n"
        ".ltorg\n");
}
#endif
