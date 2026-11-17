/* 046 (14.11.2026) – envyū-Startbild + Menue "About". Polyend Play Rev A, Firmware 1.6.0.
 *
 * Original: Gfx_init 0x3FF8 springt am Ende mit b.w 0x3E8C (FUN_00003e8c: Fonts per SETFONT2, dann bei flag = 1
 * Polyend-Logo 100×100 L8 + Cmd_text(10, 220, Font 21, "v1.6.0"), swap). Hook: dieser b.w bei 0x405C → bs_boot
 * (ueber die Weiche des Sicherheitsnetzes: Merker BAD → Original-Startbild).
 * bs_boot ruft das Original mit flag = 0 (Fonts + Zustand des Widget-Managers wie bisher, ein leeres Bild) und
 * spielt danach die Animation "Scatter": envyū im 20×8-Pad-Raster, Pads tauchen zufaellig auf, Funken auf den
 * leeren Pads klingen ab, der ū-Strich leuchtet zuletzt. 26 Bilder à 30 ms (~0,8 s), das letzte bleibt stehen,
 * bis die Firmware ihre erste Seite zeichnet.
 * Fusszeile: "envyū <Version>" (Firmware-Font 8, Strich ueber dem u selbst gezeichnet), darunter gedimmt
 * "based on Play 1.6.0" (ROM-Font 26), rechts "#xxxx" = erste 4 Hex-Stellen der Code-CRC (SN_HDR, = Build-ID).
 * RAM: ab_t bei 0x1FFFB6C0 (frei hinter hd_t 0x1FFFB680–0x1FFFB6BF, bis 0x1FFFB6FF).
 */
#include <stdint.h>
#include "version.h"

typedef struct { uint32_t magic, len, crc, nven; } sn_hdr_t;
extern const volatile sn_hdr_t SN_HDR;                 /* guard.c */

typedef struct { int32_t zero[4]; char build[12]; } ab_t;
_Static_assert(sizeof(ab_t) <= 0x40, "ab_t 0x1FFFB6C0..0x1FFFB6FF");
#define AB          ((volatile ab_t *)0x1FFFB6C0u)

#define ORIG_BOOT   ((void (*)(void *, uint32_t, const char *))0x00003E8Du)
#define EVE_READY   ((int (*)(void))0x00000849u)
#define CMD_BEGIN   ((void (*)(void))0x00000809u)
#define CMD_END     ((void (*)(void))0x00000829u)
#define DL_START    ((void (*)(void))0x00000DD5u)
#define CLEAR_RGB   ((void (*)(uint32_t))0x00000921u)
#define CLEAR       ((void (*)(uint32_t, uint32_t, uint32_t))0x00000941u)
#define COLOR_RGB   ((void (*)(uint32_t))0x00000971u)
#define VERTEX2II   ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))0x000009BDu)
#define LINE_WIDTH  ((void (*)(uint32_t))0x00000B25u)
#define BEGIN       ((void (*)(uint32_t))0x00000B9Du)
#define END         ((void (*)(void))0x00000BBDu)
#define DISPLAY     ((void (*)(void))0x00000C11u)
#define SWAP        ((void (*)(void))0x00000CD5u)
#define TEXT        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, const char *))0x00000C71u)
#define DELAY       ((void (*)(uint32_t))0x000139EDu)
#define MILLIS()    (*(volatile uint32_t *)0x20000D7Cu)
#define FONT_W(c)   (((const uint8_t *)0x000639A4u)[(uint8_t)(c) & 0x7Fu])   /* Breitenvektor Font Stil 0 (Handle 8) */

#define RECTS       9u
#define OPT_RIGHTX  2048u
#define FONT_UI     8u            /* Firmware-Font 16×22 */
#define FONT_SMALL  26u           /* ROM-Font, geglaettet, klein */

#define NFRAMES     26u
#define FRAME_MS    30u
#define PAD         11u
#define PITCH       14u
#define GX          21u           /* (320 − 20·14 + 3) / 2 */
#define GY          46u
#define V_OFF       26u
#define FOOT_X      10u
#define FOOT_Y1     196u
#define FOOT_Y2     222u

/* envyū, 3 Pads breit je Buchstabe: Zeile 0 = ū-Strich, 2…6 x-Hoehe, 7 Unterlaenge des y (Bit c = Spalte c) */
static const uint32_t WORD_ROWS[8] = { 0x00000u, 0x00000u, 0x55537u, 0x55555u, 0x56557u, 0x54551u, 0x74257u, 0x03000u };
#define MACRON_ROW  0x70000u

/* ------------------------------------------------------------------ kleine Festkomma-Hilfen (Promille) */
static int32_t clamp1k(int32_t x) { return x < 0 ? 0 : x > 1000 ? 1000 : x; }
static int32_t ease(int32_t x) { x = clamp1k(x); return x * x / 1000 * (3000 - 2 * x) / 1000; }   /* smoothstep */
static int32_t bump(int32_t x) { x = clamp1k(x); return 4 * x * (1000 - x) / 1000; }              /* ~ sin(pi x) */
static uint32_t gray(int32_t v) { if (v < 0) v = 0; if (v > 255) v = 255; return (uint32_t)v * 0x010101u; }

static uint32_t rnd(uint32_t *st, uint32_t lo, uint32_t hi) {   /* xorshift32, [lo, hi]; Zustand lokal (kein .bss) */
    uint32_t x = *st; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *st = x;
    return lo + x % (hi - lo + 1u);
}

/* Helligkeit eines Pads zum Zeitpunkt t (Promille der Animation); die Zufallswerte je Pad sind fest (Startwert 42) */
int32_t bs_pad_value(uint32_t r, uint32_t c, int32_t t) {
    uint32_t st = 42u + 1013u * (r * 20u + c);
    for (uint32_t k = 0; k < 3u; k++) (void)rnd(&st, 0, 1);   /* Startwerte entkoppeln */
    int32_t t_on = (int32_t)rnd(&st, 0, 550);
    int32_t t0 = (int32_t)rnd(&st, 0, 600), dur = (int32_t)rnd(&st, 40, 120), amp = (int32_t)rnd(&st, 250, 700);
    if (r == 0u && (MACRON_ROW >> c) & 1u)
        return V_OFF + 229 * ease((t - 780) * 1000 / 120) / 1000;
    if ((WORD_ROWS[r] >> c) & 1u) {
        int32_t k = (t - t_on) * 1000 / 120;
        if (k <= 0) return V_OFF;
        return V_OFF + 189 * ease(k) / 1000 + 40 * bump(k) / 1000;
    }
    int32_t fade = 1000 - t * 1000 / 750; if (fade < 0) fade = 0;
    if (t >= t0 && t < t0 + dur)
        return V_OFF + 150 * amp / 1000 * fade / 1000 * bump((t - t0) * 1000 / dur) / 1000;
    return V_OFF;
}

static void hex8(volatile char *d, uint32_t v) {
    for (int i = 7; i >= 0; i--) { uint32_t n = v & 15u; d[i] = (char)(n < 10u ? '0' + n : 'a' + n - 10u); v >>= 4; }
    d[8] = 0;
}

void ab_init(void) {
    volatile ab_t *a = AB;
    for (uint32_t i = 0; i < 4u; i++) a->zero[i] = 0;
    hex8(a->build, SN_HDR.crc);
}

static int wait_ready(void) {
    uint32_t t0 = MILLIS(), n = 0;
    while (!EVE_READY())                                    /* 100 ms; Zaehler falls millis wider Erwarten steht */
        if ((uint32_t)(MILLIS() - t0) > 100u || ++n > 200000u) return 0;
    return 1;
}

static void draw_body(int32_t t, const char *base, const char *hash);
static void draw_frame(int32_t t, const char *base, const char *hash) {
    CMD_BEGIN();
    DL_START();
    CLEAR_RGB(0);
    CLEAR(1, 1, 1);
    draw_body(t, base, hash);
    DISPLAY();
    SWAP();
    CMD_END();
}
static void draw_body(int32_t t, const char *base, const char *hash) {
    BEGIN(RECTS);
    LINE_WIDTH(32);                                         /* Ecken 2 px */
    COLOR_RGB(gray(V_OFF));
    for (uint32_t r = 0; r < 8u; r++)
        for (uint32_t c = 0; c < 20u; c++) {
            uint32_t x = GX + c * PITCH, y = GY + r * PITCH;
            VERTEX2II(x + 1u, y + 1u, 0, 0); VERTEX2II(x + PAD - 1u, y + PAD - 1u, 0, 0);
        }
    for (uint32_t r = 0; r < 8u; r++)
        for (uint32_t c = 0; c < 20u; c++) {
            int32_t v = bs_pad_value(r, c, t);
            if (v == V_OFF) continue;
            uint32_t x = GX + c * PITCH, y = GY + r * PITCH;
            COLOR_RGB(gray(v));
            VERTEX2II(x + 1u, y + 1u, 0, 0); VERTEX2II(x + PAD - 1u, y + PAD - 1u, 0, 0);
        }
    END();
    int32_t a = ease((t - 600) * 1000 / 400);               /* Fusszeile blendet ein */
    if (a > 0) {
        static const char LINE1[] = "envyu " ENVYU_VERSION;
        COLOR_RGB(gray(200 * a / 1000));
        TEXT(FOOT_X, FOOT_Y1, FONT_UI, 0, LINE1);
        uint32_t ux = FOOT_X; for (uint32_t i = 0; i < 4u; i++) ux += FONT_W(LINE1[i]);   /* x des u */
        uint32_t uw = FONT_W('u');
        BEGIN(RECTS); LINE_WIDTH(16);
        VERTEX2II(ux, FOOT_Y1 + 8u, 0, 0); VERTEX2II(ux + uw - 2u, FOOT_Y1 + 9u, 0, 0);   /* ū-Strich: u beginnt bei +11 (gemessen) */
        END();
        COLOR_RGB(gray(95 * a / 1000));
        TEXT(FOOT_X, FOOT_Y2, FONT_SMALL, 0, base);
        COLOR_RGB(gray(70 * a / 1000));
        TEXT(320u - FOOT_X, FOOT_Y2, FONT_SMALL, OPT_RIGHTX, hash);
    }
}

static void foot_texts(char *base, char *hash) {
    static const char B[] = "based on " ENVYU_BASE;
    for (uint32_t i = 0; i < sizeof B; i++) base[i] = B[i];
    hash[0] = '#';
    for (uint32_t i = 0; i < 4u; i++) hash[1 + i] = AB->build[i];
    hash[5] = 0;
}

__attribute__((used, section(".text.entry_boot")))
void bs_boot(void *mgr, uint32_t flag, const char *text) {
    ORIG_BOOT(mgr, 0u, text);                                /* Fonts, Widget-Manager-Zustand, leeres Bild */
    ab_init();
    if (!flag) return;
    char base[24], hash[6];
    foot_texts(base, hash);
    for (uint32_t f = 0; f < NFRAMES; f++) {
        if (!wait_ready()) return;                           /* Display antwortet nicht: Start nicht aufhalten */
        draw_frame((int32_t)(f * 1000u / (NFRAMES - 1u)), base, hash);
        DELAY(FRAME_MS);
    }
    wait_ready();
}

/* 17.11.2026: Ladebildschirm der Firmware (Projekt/Samples laden: P-Logo + Balken) zeigt stattdessen das Endbild der Animation.
 * Klasse "Ladeansicht", vtable 0xA05F8: [1] 0x2ED78 legt zwei Widgets an – +0x2C Text-Widget (Klasse 0x9B510) mit Symbol 0
 * (= P-Logo 100×100 aus der Bildtabelle 0x9A9B0) bei x 160/y 60, +0x30 Balken (Typ 6) bei 83/185, 154×5 (Fortschritt FUN_0002ee54).
 * Hook: vtable-Wort 0xA05FC → ld_create (ueber die Weiche des Sicherheitsnetzes: Merker BAD → Original). ld_create ruft das
 * Original und gibt dem Logo-Widget eine eigene vtable: vt[0] zeichnet Raster + Fusszeile (wie das letzte Animationsbild),
 * alle anderen Methoden bleiben die der Textklasse (Setzen, Anhaengen, Zerstoeren). Balken und Fortschritt: Original. */
#define ORIG_LD_CREATE  ((void (*)(void *))0x0002ED79u)
static uint32_t ld_draw(void *self) {
    (void)self;
    char base[24], hash[6];
    foot_texts(base, hash);
    CMD_BEGIN();
    DL_START();
    draw_body(1000, base, hash);
    CMD_END();
    return 0;
}
typedef struct { uint32_t (*draw)(void *); uint32_t tail[9]; } ld_vt_t;
static const ld_vt_t LD_VT = { ld_draw, { 0xB979u, 0xB94Du, 0xB3E1u, 0xB429u, 0xB42Du, 0xB431u, 0xB435u, 0xB3DDu, 0xB439u } };

__attribute__((used, section(".text.entry_ldcreate")))
void ld_create(void *self) {
    ORIG_LD_CREATE(self);
    uint32_t mgr = *(const volatile uint32_t *)((uint32_t)(uintptr_t)self + 4u);
    uint32_t id = *(const volatile uint32_t *)((uint32_t)(uintptr_t)self + 0x2Cu);
    if (mgr < 0x1FFF0000u || mgr >= 0x20030000u || id >= 50u) return;
    uint32_t arr = *(const volatile uint32_t *)(mgr + 8u);
    if (arr < 0x1FFF0000u || arr >= 0x20030000u) return;
    uint32_t obj = ((const volatile uint32_t *)arr)[id];
    if (obj < 0x1FFF0000u || obj >= 0x20030000u || *(const volatile uint32_t *)obj != 0x0009B510u) return;   /* nur die erwartete Textklasse */
    *(volatile uint32_t *)obj = (uint32_t)(uintptr_t)&LD_VT;
}
