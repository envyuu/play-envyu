/* Patch 015 – USB-Multitrack (Play → PC), Polyend Play Rev A, Firmware 1.6.0
 * ENTWURF – nur am PC getestet, am Geraet UNGETESTET. Baut auf Patch 012 (usbaudio.c, Stereo-Master) auf.
 *
 * Idee (Details: Vault-Notiz Firmware/USB_Multitrack.md):
 *  - Zweites Audio-Streaming-Interface IF5 (EP 0x87 isochron asynchron, frueher RawHID-EP7):
 *      Alt 0 = aus · Alt 1 = 10 Kanaele (max. 900 B/ms) · Alt 2 = 11 Kanaele (max. 990 B/ms), 16 Bit, 44,1 kHz nominal.
 *    IF4 (EP 0x86, Stereo-Master) bleibt unveraendert wie in 012.
 *  - Welche Quelle in welchem Kanal landet, bestimmt eine Routing-Tabelle im RAM (mt_cfg_t, 0x1FFFD480):
 *      Preset A: Spur 1..8, Master L/R (+ FX-Return mono im 11. Kanal)
 *      Preset B: Spur 1..8, FX-Return L/R (Delay+Reverb nass) (+ Master mono im 11. Kanal)
 *      Preset 0xFF: eigene Tabelle route10/route11. Wirkt ab dem naechsten Block, ohne Neuanmeldung.
 *  - Abgriffe im Mixer-Update 0x15884 (zwei BL-Umleitungen, Mixer arbeitet unveraendert weiter):
 *      0x158D6 mt_trk_hook: Spureingang i (r6 = int16[128], genau der Kanal, den der Mixer verwendet),
 *              vor Pan und Sends; optional mit Spur-Lautstaerke (Mixer-Verstaerkung, Rampe wie im Mixer).
 *      0x15CBC mt_fx_hook:  Ausgang Delay (r5 = 8) bzw. Reverb (r5 = 9), r6 = L, r4 = R, bevor er auf den Master geht.
 *    Master L/R kommt wie in 012 aus audio_tap (nach Limiter und Loop, = DMA-Puffer).
 *  - Erzeuger (Audio-ISR, Prio 0xD0): mt_pre reserviert vor audio_callback einen 128-Frame-Block im Ring,
 *    die Hooks schreiben direkt verschraenkt hinein, mt_post ergaenzt Master/Nullen und gibt den Block frei.
 *    Laeuft der Mixer ausserhalb der Audio-ISR (WAV-Export im Hauptprogramm), ist nichts reserviert → Hooks tun nichts.
 *  - Verbraucher (USB-ISR, Prio 0x70): Fuellstandsregler wie 012, Pakete 44/45 Frames (selten 43).
 *
 * RAM (SRAM_L, eigener Bereich 0x1FFFD400..0x1FFFFF00):
 *   0x1FFFD400 Zustand mt_t (0x80) · 0x1FFFD480 Konfiguration mt_cfg_t (0x40) · 0x1FFFD4C0 Konfig.-Deskriptor (357 B)
 *   0x1FFFD630 / 0x1FFFDA10 Sendepuffer (je 990 B) · 0x1FFFDE00 Ring 384 Frames x 22 B = 8448 B → Ende genau 0x1FFFFF00
 *
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt (mt_test.c).
 */
#include <stdint.h>

#define MT_IF        5u
#define MT_EP        7u
#define MT_EPADDR    0x87u
#define MT_BDT_EVEN  (MT_EP * 4u + 2u)   /* 30 */
#define MT_BDT_ODD   (MT_EP * 4u + 3u)   /* 31 */
#define ENDPT_ISO_TX 0x14u
#define BLOCK        128u

#define MT_CAP       384u                /* Ringkapazitaet in Frames = 3 Bloecke → Bloecke liegen nie ueber dem Ringende */
#define MT_MOD       (MT_CAP << 20)      /* Zaehler laufen modulo MT_MOD (Vielfaches von CAP und 128, < 2^31) */
#define MT_TARGET    192u                /* Soll-Fuellstand (~4,4 ms), Platz nach oben und unten je ~128 Frames */
#define MT_MAXF      45u                 /* = wMaxPacketSize / Framegroesse (44,1 kHz → hoechstens 45 Frames/ms) */
#define MT_MAXCH     11u
#define MT_NOM_Q16   2891294u            /* 44117,647 Hz / 1000 * 65536 */
#define MT_LPF_SHIFT 5
#define MT_RATE_CLAMP (3 << 14)         /* hoechstens +-0,75 Frames/ms (1,7 %) → n <= 45 */
#define MT_MAGIC     0x4D54A015u
#define MT_CFG_MAGIC 0x4D544346u
#define GAIN_ONE     0x10000

/* Quellen in der Routing-Tabelle */
enum {
    S_T1 = 0,                    /* 0..7  Spur 1..8 (mono, vor Pan) */
    S_ML = 8, S_MR = 9,          /* Master L/R (nach Limiter/Loop, wie Stereo-USB) */
    S_FXL = 10, S_FXR = 11,      /* FX-Return L/R = Delay + Reverb nass */
    S_FXM = 12,                  /* FX-Return mono (L+R)/2 */
    S_MM = 13,                   /* Master mono (L+R)/2 */
    S_DL = 14, S_DR = 15,        /* Delay nass L/R */
    S_RL = 16, S_RR = 17,        /* Reverb nass L/R */
    S_LAST = 17,
    S_ZERO = 0xFF                /* Stille */
};

enum { ST_OFF = 0, ST_PREFILL = 1, ST_RUN = 2 };
#define CFG_POSTFADER 0x01u      /* flags Bit 0: Spuren mit Spur-Lautstaerke (Mixer-Verstaerkung, ohne Pan) */

typedef struct {               /* per PEEK 1FFFD400.. lesbar (mt_status.py) */
    uint32_t magic;            /* 00 */
    uint32_t head;             /* 04 Erzeuger, Frames modulo MT_MOD */
    uint32_t tail;             /* 08 Verbraucher */
    uint32_t alt;              /* 0C Alternate Setting IF5 */
    uint32_t state;            /* 10 ST_* */
    uint32_t nch;              /* 14 Kanaele des aktiven Alt (10/11) */
    uint32_t epoch;            /* 18 wird bei jedem Alt-Wechsel erhoeht (Verbraucher) */
    uint32_t blocks;           /* 1C freigegebene Bloecke */
    uint32_t drops;            /* 20 verworfene Bloecke (Ring voll) im Zustand LAEUFT */
    uint32_t packets;          /* 24 */
    uint32_t underruns;        /* 28 */
    uint32_t frames;           /* 2C gesendete Frames */
    int32_t  favg;             /* 30 gefilterter Fuellstand Q8 */
    int32_t  rate;             /* 34 Q16 Frames/ms */
    uint32_t acc;              /* 38 */
    uint32_t last_n;           /* 3C */
    uint32_t fill_min;         /* 40 */
    uint32_t fill_max;         /* 44 */
    uint32_t starts;           /* 48 */
    uint32_t setups;           /* 4C von uns beantwortete Setup-Requests fuer IF5/EP 0x87 */
    uint32_t wr_ok;            /* 50 Erzeuger: Block reserviert (nur zwischen mt_pre und mt_post) */
    uint32_t wr_epoch;         /* 54 */
    uint32_t wr_pos;           /* 58 Frameposition des reservierten Blocks (0/128/256) */
    uint32_t wr_nch;           /* 5C */
    uint32_t wmask;            /* 60 beschriebene Kanaele des Blocks */
    uint32_t trk_seen;         /* 64 Spuren, die im letzten Block geliefert haben (Bit i) */
    uint32_t fx_seen;          /* 68 Bit 0 Delay, Bit 1 Reverb */
    uint32_t hook_calls;       /* 6C Anzahl Hook-Aufrufe ausserhalb eines reservierten Blocks (Export, voll) */
    uint8_t  rt[12];           /* 70 aufgeloestes Routing des laufenden Blocks */
    uint32_t pad;              /* 7C */
} mt_t;

typedef struct {               /* Konfiguration (spaeter per Menue/USB setzbar) */
    uint32_t magic;            /* 00 MT_CFG_MAGIC */
    uint8_t  preset;           /* 04 0 = A, 1 = B, 0xFF = route10/route11 */
    uint8_t  flags;            /* 05 CFG_POSTFADER */
    uint8_t  res[2];           /* 06 */
    uint8_t  route10[10];      /* 08 Quelle je Kanal fuer Alt 1 */
    uint8_t  route11[11];      /* 12 Quelle je Kanal fuer Alt 2 */
    uint8_t  res2[0x40 - 0x1D];
} mt_cfg_t;

_Static_assert(sizeof(mt_t) == 0x80, "mt_t");
_Static_assert(sizeof(mt_cfg_t) == 0x40, "mt_cfg_t");
_Static_assert(0x1FFFDE00u + MT_CAP * MT_MAXCH * 2u == 0x1FFFFF00u, "Ring endet bei 0x1FFFFF00");
_Static_assert(0x1FFFDA10u + MT_MAXF * MT_MAXCH * 2u <= 0x1FFFDE00u && 0x1FFFD630u + MT_MAXF * MT_MAXCH * 2u <= 0x1FFFDA10u, "Sendepuffer");

static const uint8_t preset_tab[2][2][MT_MAXCH] = {
    /* A */ { { 0, 1, 2, 3, 4, 5, 6, 7, S_ML, S_MR, S_ZERO }, { 0, 1, 2, 3, 4, 5, 6, 7, S_ML, S_MR, S_FXM } },
    /* B */ { { 0, 1, 2, 3, 4, 5, 6, 7, S_FXL, S_FXR, S_ZERO }, { 0, 1, 2, 3, 4, 5, 6, 7, S_FXL, S_FXR, S_MM } },
};

/* ------------------------------------------------------------------ Hardware-/Firmware-Zugriffe */
#ifdef HOST_TEST
extern uint8_t  host_ram[0x3F00];          /* bildet 0x1FFFC000..0x1FFFFF00 nach */
extern uint32_t host_bdt_desc[32];
extern const void *host_bdt_ptr[32];
extern uint8_t  host_endpt7;
#define MT_RAM(a)   ((void *)(host_ram + ((a) - 0x1FFFC000u)))
#define BDT_SET(i, p, d) do { host_bdt_ptr[i] = (p); host_bdt_desc[i] = (d); } while (0)
#define ENDPT7      host_endpt7
#define BARRIER()   __asm__ volatile("" ::: "memory")
#define IRQ_SAVE()  0u
#define IRQ_RESTORE(x) ((void)(x))
#else
#define MT_RAM(a)   ((void *)(a))
#define BDT         ((volatile uint32_t *)0x1FFF0000u)
#define BDT_SET(i, p, d) do { BDT[2u * (i) + 1u] = (uint32_t)(p); __asm__ volatile("dmb" ::: "memory"); BDT[2u * (i)] = (d); } while (0)
#define ENDPT7      (*(volatile uint8_t *)0x400720DCu)            /* USB0_ENDPT7 */
#define BARRIER()   __asm__ volatile("dmb" ::: "memory")
static inline uint32_t irq_save(void) { uint32_t p; __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(p) :: "memory"); return p; }
static inline void irq_restore(uint32_t p) { __asm__ volatile("msr primask, %0" :: "r"(p) : "memory"); }
#define IRQ_SAVE()  irq_save()
#define IRQ_RESTORE(x) irq_restore(x)
#endif

#define MT       ((volatile mt_t *)MT_RAM(0x1FFFD400u))
#define MTCFG    ((volatile mt_cfg_t *)MT_RAM(0x1FFFD480u))
#define MT_TX0   ((uint16_t *)MT_RAM(0x1FFFD630u))       /* 990 B */
#define MT_TX1   ((uint16_t *)MT_RAM(0x1FFFDA10u))       /* 990 B, Ende 0x1FFFDDEE */
#define MT_RING  ((int16_t *)MT_RAM(0x1FFFDE00u))      /* MT_CAP * MT_MAXCH int16 = 8448 B, Ende 0x1FFFFF00 */

#define BDT_OWN   0x80u

static inline uint32_t mod_add(uint32_t a, uint32_t n) { a += n; if (a >= MT_MOD) a -= MT_MOD; return a; }
static inline uint32_t mod_sub(uint32_t a, uint32_t b) { return a >= b ? a - b : a + MT_MOD - b; }   /* a - b modulo */

/* ------------------------------------------------------------------ Initialisierung (aus usbinit_hook, vor usb_init) */
void mt_init(void) {
    volatile uint32_t *z = (volatile uint32_t *)(void *)MT;
    for (uint32_t i = 0; i < sizeof(mt_t) / 4u; i++) z[i] = 0;
    volatile uint32_t *c = (volatile uint32_t *)(void *)MTCFG;
    for (uint32_t i = 0; i < sizeof(mt_cfg_t) / 4u; i++) c[i] = 0;
    volatile mt_cfg_t *g = MTCFG;
    g->preset = 0;
    g->flags = CFG_POSTFADER;
    for (uint32_t k = 0; k < 10u; k++) g->route10[k] = preset_tab[0][0][k];
    for (uint32_t k = 0; k < MT_MAXCH; k++) g->route11[k] = preset_tab[0][1][k];
    volatile int16_t *r = MT_RING;
    for (uint32_t i = 0; i < MT_CAP * MT_MAXCH; i++) r[i] = 0;
    MT->fill_min = 0xFFFFFFFFu;
    g->magic = MT_CFG_MAGIC;
    BARRIER();
    MT->magic = MT_MAGIC;
}

/* ------------------------------------------------------------------ Erzeuger (Audio-ISR) */
/* vor audio_callback: Block reservieren und Routing aufloesen */
void mt_pre(void) {
    volatile mt_t *u = MT;
    if (u->magic != MT_MAGIC) return;
    u->wr_ok = 0;
    if (u->state == ST_OFF) return;
    uint32_t ep = u->epoch;
    BARRIER();
    uint32_t nch = u->nch;
    if (nch != 10u && nch != 11u) return;
    uint32_t head = u->head, tail = u->tail;
    if (head >= MT_MOD || (head % BLOCK) != 0u) { head = 0; u->head = 0; }   /* Muell → neu anfangen (Verbraucher prueft avail) */
    uint32_t used = mod_sub(head, tail);
    if (used > MT_CAP || MT_CAP - used < BLOCK) { if (u->state == ST_RUN) u->drops++; return; }
    volatile mt_cfg_t *g = MTCFG;
    const uint8_t *src;
    uint32_t p = g->preset;
    if (g->magic == MT_CFG_MAGIC && p == 0xFFu) src = (const uint8_t *)(nch == 10u ? g->route10 : g->route11);
    else src = preset_tab[p == 1u ? 1 : 0][nch == 10u ? 0 : 1];
    for (uint32_t c = 0; c < MT_MAXCH; c++) {
        uint32_t s = c < nch ? src[c] : S_ZERO;
        u->rt[c] = (uint8_t)(s <= S_LAST ? s : S_ZERO);
    }
    u->wr_pos = head % MT_CAP;          /* 0, 128 oder 256 */
    u->wr_nch = nch;
    u->wr_epoch = ep;
    u->wmask = 0;
    u->trk_seen = 0;
    u->fx_seen = 0;
    BARRIER();
    u->wr_ok = 1;
}

/* Der reservierte Block gehoert bis mt_post allein dem Erzeuger (Verbraucher liest nur [tail, head)) → kein volatile;
 * die Reihenfolge "Daten vor head" sichert BARRIER() in mt_post. */
static inline int16_t *slot(volatile mt_t *u, uint32_t ch) {
    return MT_RING + u->wr_pos * u->wr_nch + ch;
}
#ifdef HOST_TEST
static inline int32_t ssat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
#else
static inline int32_t ssat16(int32_t v) { int32_t r; __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(v)); return r; }
#endif

/* Spur i liefert ihren Mixer-Eingang (vor Pan/Sends). mixer = Mixer-AudioStream (0x15884: sl) fuer die Verstaerkungen:
 *   +0x1C+4i / +0x3C+4i aktuelle Verstaerkung L/R (Q16, Lautstaerke x Pan), +0x9C+4i / +0xBC+4i vorige (Rampe). */
void mt_track(const int16_t *x, uint32_t i, const uint8_t *mixer) {
    volatile mt_t *u = MT;
    if (u->magic != MT_MAGIC || !u->wr_ok) { if (u->magic == MT_MAGIC) u->hook_calls++; return; }
    if (i >= 8u || !x) return;
    int32_t gc = GAIN_ONE, gp = GAIN_ONE;
    if (MTCFG->flags & CFG_POSTFADER) {
        const volatile int32_t *g = (const volatile int32_t *)(const void *)mixer;
        int32_t cl = g[7 + i], cr = g[15 + i], pl = g[39 + i], pr = g[47 + i];   /* 0x1C, 0x3C, 0x9C, 0xBC */
        gc = cl > cr ? cl : cr;                      /* lineares Pan-Gesetz: eine Seite = Lautstaerke */
        gp = pl > pr ? pl : pr;
        if (gc < 0) gc = 0;
        if (gc > GAIN_ONE) gc = GAIN_ONE;
        if (gp < 0) gp = 0;
        if (gp > GAIN_ONE) gp = GAIN_ONE;
    }
    uint32_t nch = u->wr_nch;
    for (uint32_t c = 0; c < nch; c++) {
        if (u->rt[c] != i) continue;
        int16_t *d = slot(u, c);
        if (gc == GAIN_ONE && gp == GAIN_ONE) {
            for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = x[k];
        } else {
            /* 0 <= g <= 1.0 (Q16) → |x*g| <= 2^31, Ergebnis immer im int16-Bereich: keine Saettigung noetig */
            int32_t g = gp, dg = (gc - gp) / (int32_t)BLOCK;
            for (uint32_t k = 0; k < BLOCK; k++, d += nch, g += dg) *d = (int16_t)((x[k] * g) >> 16);
        }
        u->wmask |= 1u << c;
    }
    u->trk_seen |= 1u << i;
}

/* Delay (which = 8) bzw. Reverb (which = 9) liefert seinen nassen Stereo-Ausgang. */
void mt_fx(const int16_t *l, const int16_t *r, uint32_t which) {
    volatile mt_t *u = MT;
    if (u->magic != MT_MAGIC || !u->wr_ok) { if (u->magic == MT_MAGIC) u->hook_calls++; return; }
    if ((which != 8u && which != 9u) || !l || !r) return;
    uint32_t nch = u->wr_nch;
    for (uint32_t c = 0; c < nch; c++) {
        uint32_t s = u->rt[c], m;          /* m: 1 = L, 2 = R, 3 = (L+R)/2 */
        if (s == S_FXL) m = 1; else if (s == S_FXR) m = 2; else if (s == S_FXM) m = 3;
        else if (which == 8u && s == S_DL) m = 1; else if (which == 8u && s == S_DR) m = 2;
        else if (which == 9u && s == S_RL) m = 1; else if (which == 9u && s == S_RR) m = 2;
        else continue;
        int16_t *d = slot(u, c);
        int add = (u->wmask >> c) & 1u;
        if (m == 3) {
            if (add) for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = (int16_t)ssat16(*d + ((l[k] + r[k]) >> 1));
            else     for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = (int16_t)((l[k] + r[k]) >> 1);
        } else {
            const int16_t *x = m == 1 ? l : r;
            if (add) for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = (int16_t)ssat16(*d + x[k]);
            else     for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = x[k];
        }
        u->wmask |= 1u << c;
    }
    u->fx_seen |= which == 8u ? 1u : 2u;
}

/* nach audio_callback: Master eintragen, Rest mit Stille, Block freigeben. */
void mt_post(const int16_t *l, const int16_t *r) {
    volatile mt_t *u = MT;
    if (u->magic != MT_MAGIC || !u->wr_ok) return;
    uint32_t nch = u->wr_nch;
    for (uint32_t c = 0; c < nch; c++) {
        uint32_t s = u->rt[c];
        int16_t *d = slot(u, c);
        if (s == S_ML || s == S_MR) {
            const int16_t *x = s == S_ML ? l : r;
            for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = x[k];
        } else if (s == S_MM) {
            for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = (int16_t)((l[k] + r[k]) >> 1);
        } else if (!((u->wmask >> c) & 1u)) {
            for (uint32_t k = 0; k < BLOCK; k++, d += nch) *d = 0;
        }
    }
    BARRIER();                                       /* erst Daten, dann head */
    uint32_t pm = IRQ_SAVE();                        /* Alt-Wechsel (USB-ISR) darf nicht zwischen Pruefung und Freigabe fallen */
    if (u->epoch == u->wr_epoch && u->state != ST_OFF) { u->head = mod_add(u->head, BLOCK); u->blocks++; }
    u->wr_ok = 0;
    IRQ_RESTORE(pm);
}

/* ------------------------------------------------------------------ Verbraucher (USB-ISR) */
/* 32-Bit-Kopie, Quelle/Ziel duerfen auf 2 Byte ausgerichtet sein: Cortex-M4 erlaubt unausgerichtete LDR/STR auf SRAM
 * (SCB_CCR.UNALIGN_TRP = 0, die Firmware schreibt SCB_CCR nirgends). Reihenfolge gegen head: Lesen erst nach dem
 * Lesen von head (volatile) in mt_make_packet. */
typedef struct __attribute__((packed, may_alias)) { uint32_t v; } u32u_t;
static void copy_frames(uint16_t *dst, uint32_t pos, uint32_t n, uint32_t nch) {
    const uint16_t *s = (const uint16_t *)(const void *)MT_RING + pos * nch;
    uint32_t m = n * nch, w = m >> 1;
    u32u_t *d4 = (u32u_t *)(void *)dst;
    const u32u_t *s4 = (const u32u_t *)(const void *)s;
    for (uint32_t k = 0; k < w; k++) d4[k].v = s4[k].v;
    if (m & 1u) dst[m - 1u] = s[m - 1u];
}

static uint32_t mt_make_packet(uint16_t *buf) {
    volatile mt_t *u = MT;
    uint32_t nch = u->nch;
    uint32_t head = u->head, tail = u->tail;
    if (head >= MT_MOD) head = 0;
    uint32_t avail = mod_sub(head, tail);
    if (tail >= MT_MOD || avail > MT_CAP) { tail = head; avail = 0; }
    if (u->state == ST_PREFILL) {
        if (avail < MT_TARGET) { u->tail = tail; u->last_n = 0; return 0; }
        if (avail > MT_TARGET + BLOCK) { tail = mod_sub(head, MT_TARGET); avail = MT_TARGET; }
        u->state = ST_RUN;
        u->favg = (int32_t)(avail << 8);
        u->acc = 0;
    } else if (u->state != ST_RUN) {
        return 0;
    }
    if (avail < u->fill_min) u->fill_min = avail;
    if (avail > u->fill_max) u->fill_max = avail;
    int32_t favg = u->favg;
    favg += (((int32_t)avail << 8) - favg) >> MT_LPF_SHIFT;
    u->favg = favg;
    int32_t adj = (favg - (int32_t)(MT_TARGET << 8)) >> 1;
    if (adj > MT_RATE_CLAMP) adj = MT_RATE_CLAMP;
    if (adj < -MT_RATE_CLAMP) adj = -MT_RATE_CLAMP;
    int32_t rate = (int32_t)MT_NOM_Q16 + adj;
    u->rate = rate;
    uint32_t acc = u->acc + (uint32_t)rate;
    uint32_t n = acc >> 16;
    u->acc = acc & 0xFFFFu;
    if (n > MT_MAXF) n = MT_MAXF;
    if (n > avail) { u->underruns++; n = avail; u->state = ST_PREFILL; }
    uint32_t pos = tail % MT_CAP, n1 = MT_CAP - pos;
    if (n1 > n) n1 = n;
    copy_frames(buf, pos, n1, nch);
    if (n > n1) copy_frames(buf + n1 * nch, 0, n - n1, nch);
    u->tail = mod_add(tail, n);
    u->frames += n;
    u->last_n = n;
    return n * nch * 2u;
}

static void mt_stream_start(uint32_t alt) {
    volatile mt_t *u = MT;
    u->state = ST_OFF;                       /* waehrend des Umbaus keine Pakete */
    u->nch = alt == 1u ? 10u : 11u;
    BARRIER();
    u->epoch++;                              /* ein gerade reservierter Block (altes Format) wird nicht freigegeben */
    uint32_t h = u->head;
    u->tail = h < MT_MOD ? h : 0;
    u->acc = 0; u->favg = 0; u->rate = (int32_t)MT_NOM_Q16; u->last_n = 0;
    u->fill_min = 0xFFFFFFFFu; u->fill_max = 0;
    u->underruns = 0; u->drops = 0;
    u->starts++;
    u->alt = alt;
    ENDPT7 = ENDPT_ISO_TX;
    BDT_SET(MT_BDT_EVEN, MT_TX0, BDT_OWN);   /* beide Baenke mit Nullpaket vorbelegen */
    BDT_SET(MT_BDT_ODD, MT_TX1, BDT_OWN);
    BARRIER();
    u->state = ST_PREFILL;
}

static void mt_stream_stop(void) {
    volatile mt_t *u = MT;
    u->state = ST_OFF;
    u->epoch++;
    u->alt = 0;
    ENDPT7 = ENDPT_ISO_TX;
    BDT_SET(MT_BDT_EVEN, MT_TX0, 0);
    BDT_SET(MT_BDT_ODD, MT_TX1, 0);
}

/* 017: vor einer Neuanmeldung (usbmenu.c, unter Interrupt-Sperre) */
void mt_force_stop(void) {
    if (MT->magic != MT_MAGIC) return;
    mt_stream_stop();
}

/* Token fertig auf EP7 (aus ep_hook, STAT-Byte). */
void mt_ep_token(uint32_t stat) {
    volatile mt_t *u = MT;
    uint32_t i = (stat >> 2) & 31u;
    if (i != MT_BDT_EVEN && i != MT_BDT_ODD) return;     /* EP7-RX gehoert weiter Teensy (Host sendet nie dorthin) */
    uint16_t *buf = (i & 1u) ? MT_TX1 : MT_TX0;
    if (u->magic != MT_MAGIC || u->state == ST_OFF) { BDT_SET(i, buf, 0); return; }
    uint32_t len = mt_make_packet(buf);
    u->packets++;
    BDT_SET(i, buf, (len << 16) | BDT_OWN);
}

/* Setup-Requests fuer IF5 / EP 0x87 (aus ua_setup).
 * Rueckgabe: -1 nicht zustaendig, 0 STALL, 1 Statusstufe (ZLP) senden, 2 alt-Byte in *out senden, 3 Rate in out[0..2]. */
int mt_setup(uint32_t req, uint32_t wValue, uint32_t wIndex, uint8_t *out) {
    volatile mt_t *u = MT;
    if (u->magic != MT_MAGIC) return -1;
    if (ENDPT7 != ENDPT_ISO_TX) { u->alt = 0; u->state = ST_OFF; }   /* SET_CONFIGURATION hat EP7 neu gesetzt (0x19) */
    switch (req) {
    case 0x0B01:
        if (wIndex != MT_IF) return -1;
        if (wValue > 2u) return 0;
        if (wValue) mt_stream_start(wValue); else mt_stream_stop();
        u->setups++;
        return 1;
    case 0x0A81:
        if (wIndex != MT_IF) return -1;
        out[0] = (uint8_t)u->alt;
        u->setups++;
        return 2;
    case 0x0122:
        if ((wIndex & 0xFFu) != MT_EPADDR) return -1;
        if ((wValue >> 8) != 1u) return 0;
        u->setups++;
        return 1;
    case 0x81A2:
        if ((wIndex & 0xFFu) != MT_EPADDR) return -1;
        if ((wValue >> 8) != 1u) return 0;
        out[0] = 0x44; out[1] = 0xAC; out[2] = 0x00;
        u->setups++;
        return 3;
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ Einsprungpunkte im Mixer-Update 0x15884 */
#ifndef HOST_TEST
/* 0x158D6: "ldr r3,[sp,#8]; ldr r7,[sp,#32]" → bl mt_trk_hook.
 * Dort: r6 = int16[128] des Spureingangs (Mixer nimmt genau diesen Kanal fuer L, R und Sends), [sp+8] = Spur i,
 * sl (r10) = Mixer-AudioStream. Beide Befehle werden zuerst nachgebildet (sp ist beim BL unveraendert). */
__attribute__((naked, used, section(".text.entry_mttrk")))
void mt_trk_hook(void) {
    __asm__ volatile(
        "ldr   r3, [sp, #8]\n"
        "ldr   r7, [sp, #32]\n"
        "push  {r0, r1, r2, r3, ip, lr}\n"
        "mov   r0, r6\n"
        "mov   r1, r3\n"
        "mov   r2, sl\n"
        "bl    mt_track\n"
        "pop   {r0, r1, r2, r3, ip, lr}\n"
        "bx    lr\n");
}

/* 0x15CBC: "ldr.w r7,[sl,#0x154]" (direkt nach dem Aufruf von Delay/Reverb) → bl mt_fx_hook.
 * Dort: r6 = Ausgang L, r4 = Ausgang R (int16[128]), r5 = 8 (Delay) / 9 (Reverb). */
__attribute__((naked, used, section(".text.entry_mtfx")))
void mt_fx_hook(void) {
    __asm__ volatile(
        "push  {r0, r1, r2, r3, ip, lr}\n"
        "mov   r0, r6\n"
        "mov   r1, r4\n"
        "mov   r2, r5\n"
        "bl    mt_fx\n"
        "pop   {r0, r1, r2, r3, ip, lr}\n"
        "ldr.w r7, [sl, #0x154]\n"
        "bx    lr\n");
}
#endif
