/* Patch 017 – Menueseite "USB Audio", Einstellungsdatei /usbaudio.cfg, Umschalter Classic/Creative
 * Polyend Play Rev A, Firmware 1.6.0
 *
 * Hauptmenue (MainMenu_build 0x335EC): vor "Settings" ein Eintrag "USB Audio" (Hook auf "bl MenuItem_ctor" bei 0x33676).
 * Seite (um_build), Eintraege wie in "Settings" (EnumItem_ctor 0x34F30: int32-Wert per Zeiger, Liste mit Namen):
 *   Audio Out        Off / Stereo / Stereo + Multi   → USB-Deskriptor (Neuanmeldung am Bus, ~0,4 s)
 *   Multitrack 9/10  Master / FX Return              → mt_cfg_t.preset (Patch 015, sofort)
 *   Track Level      Pre Fader / Post Fader          → mt_cfg_t.flags Bit 0 (sofort)
 *   FX Mode          Classic / Creative              → Perform-FX (fx.c) + Arp-Knopf (arp.c) + Effektnamen (sofort)
 * Die Werte liegen in um_t (SRAM_L 0x1FFFD300), werden aus der Hauptschleife (loop_hook → um_poll) angewendet und
 * 1,5 s nach der letzten Aenderung nach /usbaudio.cfg geschrieben (Textdatei key=value). Gelesen wird die Datei nach
 * dem Start, sobald die SD-Karte bereit ist.
 *
 * Neuanmeldung: D+-Pullup aus (USB0_CONTROL = 0), Streams stoppen, 400 ms warten, Deskriptor-Variante nach 0x1FFFD4C0
 * kopieren, Laenge in die RAM-Kopie der usb_descriptor_list (Literal 0x131AC zeigt auf um_t.list) eintragen,
 * Pullup an (USB0_CONTROL = 0x10, wie usb_init). Der Host setzt den Bus zurueck und zaehlt neu auf.
 *
 * Effektnamen: die Namenszeiger der MIDI-Delay-/Reverb-Eintraege (0xA86B4/0xA8E34 + 20k + 8) zeigen auf 32 Puffer
 * zu 16 Byte bei 0x1FFFB400 (Rest des Upload-Bereichs); um_apply_creative() kopiert Original- oder FX-Suite-Namen hinein.
 *
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt (um_test.c).
 */
#include <stdint.h>
#include "um_desc.h"
#include "cb.h"

extern const uint8_t ua_cfg_desc[];           /* usbaudio.c, 365 Byte (Stereo + Multi) */
void ua_force_stop(void);                      /* usbaudio.c */
void mt_force_stop(void);                      /* multitrack.c */
#ifdef NO_UI
static inline void ui_force_stop(void) {}
static inline void ui_set_mode(int on) { (void)on; }
#define UI_DESC_IN ua_cfg_desc
#define UI_DESC_LEN 331u
#else
void ui_force_stop(void);                      /* usbin.c (Patch 020) */
void ui_set_mode(int on);
extern const uint8_t UI_DESC_IN[];             /* usbin.c: "Stereo + In", 331 Byte */
#define UI_DESC_LEN 331u
#endif
void arp_set_creative(int on);                 /* arp.c: Platz 11 "Arp" bzw. Original */
extern const char *const FX_NAMES[32];         /* fx.c: 16 Delay- + 16 Reverb-Namen der FX-Suite */

#define UM_MAGIC     0x55AEC017u
#define MT_CFG_MAGIC 0x4D544346u
#define NNAMES       32u
#define NAMELEN      16u
#define DESC_FULL_LEN 365u
#define SAVE_DELAY_MS 1500u
#define REENUM_MS    400u
#define LOAD_FIRST_MS 1000u
#define LOAD_RETRY_MS 500u
#define LOAD_GIVEUP_MS 20000u
#define FA_READ      0x01u
#define FA_WRITE_NEW 0x0Au
#define FIL_SIZE     0x250u
#define FR_NO_FILE   4
#define FR_NO_PATH   5

enum { V_USB, V_PRESET, V_FADER, V_CREATIVE, NV };
static const int32_t VMAX[NV] = { 3, 1, 1, 1 };   /* 020: usb 3 = Stereo + In */
static const int32_t VDEF[NV] = { 2, 0, 1, 1 };
static const char *const VKEY[NV] = { "usb", "mt", "fader", "creative" };

typedef struct {
    uint32_t magic;            /* 00 */
    int32_t  v[NV];            /* 04 Menuewerte: usb, preset, fader, creative (fx.c/arp.c lesen creative @ +0x10) */
    int32_t  saved[NV];        /* 14 Stand der Datei */
    int32_t  applied[NV];      /* 24 zuletzt angewendet */
    int32_t  seen[NV];         /* 34 zuletzt gesehen (fuer die Verzoegerung beim Speichern) */
    uint32_t loaded;           /* 44 1 = Datei gelesen (oder aufgegeben) */
    uint32_t t_load;           /* 48 naechster Leseversuch */
    uint32_t t_change;         /* 4C letzte Aenderung */
    uint32_t re_state, re_t0;  /* 50 Neuanmeldung: 0 ruhig, 1 getrennt */
    uint32_t reenums, saves;   /* 58 Zaehler */
    uint32_t load_res;         /* 60 Ergebnis des Lesens: FatFs-Code | Versuche << 8 | 0x10000 = gelesen */
    uint32_t save_err;         /* 64 Fehlversuche insgesamt */
    uint32_t save_fail;        /* 68 Fehlversuche in Folge (ab 5: aufgeben bis zur naechsten Aenderung) */
    uint32_t _r;               /* 6C */
    uint8_t  list[0x6C];       /* 70 RAM-Kopie usb_descriptor_list (9 x 12 Byte) */
    int32_t  stereo, stereo_saved, stereo_seen;    /* DC 026: Aufnahme Mono (0) / Stereo (1), hinten angehaengt (Offsets davor bleiben) */
    int32_t  ins[2], ins_saved[2], ins_seen[2];    /* E8 045: Audio In – Send zu Delay/Reverb, Index in N_SEND (0 = Off) */
} um_t;
_Static_assert(sizeof(um_t) == 0x100, "um_t");
_Static_assert(0x1FFFD300u + sizeof(um_t) <= 0x1FFFD400u, "um_t vor mt_t");

/* 045 (11.11.): Aufnahme-Einstellungen "Wait For Signal" / "Pause On Silence" (um_t ist voll) – SRAM_L 0x1FFF9A40 */
typedef struct {
    uint32_t magic;            /* 00 */
    int32_t  v[3];             /* 04 0 = Wait For Signal, 1 = Pause On Silence, 2 = Keep Selection (12.11.) (0 Off / 1 On) */
    int8_t   saved[3];         /* 10 (12.11.: Bytes, damit 3 Werte in 0x20 passen) */
    int8_t   seen[3];          /* 13 */
} rs_t;
_Static_assert(sizeof(rs_t) <= 0x20, "rs_t 0x1FFF9A40..0x1FFF9A5F");
#define RS_MAGIC 0x52534554u
#define RS_N 3u
static const char *const RSKEY[RS_N] = { "rwait", "rpause", "keepsel" };

typedef struct { uint32_t store[2]; uint32_t manager; uint32_t invoker; } stdfn_t;   /* std::function (libstdc++) */

#ifdef HOST_TEST
extern uint8_t  host_um_ram[0x100];
extern uint8_t  host_names_ram[NNAMES * NAMELEN];
extern uint8_t  host_desc_ram[DESC_FULL_LEN];
extern uint8_t  host_mtcfg[0x40];
extern uint8_t  host_flash_list[0x6C];
extern volatile uint8_t host_usb0_control, host_usb_configuration;
extern uint32_t host_millis;
extern const char *const host_orig_names[NNAMES];
extern void *host_heap_alloc(uint32_t n);
extern void *host_menu_add(void *it, void *parent, const char *name, uint32_t fn, uint32_t adj);
extern void *host_enum_item(void *it, void *parent, const char *name, int32_t *val, uint32_t max, const char *const *tab, stdfn_t *fn);
extern void *host_malloc(uint32_t n);
extern void host_free(void *p);
extern int host_f_open(void *fp, const char *path, uint8_t mode);
extern int host_f_read(void *fp, void *buf, uint32_t n, uint32_t *br);
extern int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw);
extern int host_f_close(void *fp);
#define UM          ((volatile um_t *)(void *)host_um_ram)
extern uint8_t host_cb_ram[0x10];
#define CBS         ((volatile cb_t *)(void *)host_cb_ram)
extern uint8_t host_rs_ram[0x20];
#define RS          ((volatile rs_t *)(void *)host_rs_ram)
#define NAMES_RAM   ((volatile char *)host_names_ram)
#define DESC_RAM    ((volatile uint8_t *)host_desc_ram)
#define MTCFG_RAM   ((volatile uint8_t *)host_mtcfg)
#define FLASH_LIST  ((const uint8_t *)host_flash_list)
#define USB0_CONTROL host_usb0_control
#define USB_CONFIGURATION host_usb_configuration
#define MILLIS()    (host_millis)
#define ORIG_NAME(i) (host_orig_names[(i)])
#define HEAP_ALLOC(n) host_heap_alloc(n)
#define MENU_ADD    host_menu_add
#define ENUM_ITEM   host_enum_item
#define FN_MANAGER  0x31A35u
#define MALLOC(n)   host_malloc(n)
#define FREE(p)     host_free(p)
#define F_OPEN      host_f_open
#define F_READ      host_f_read
#define F_WRITE     host_f_write
#define F_CLOSE     host_f_close
#define FNADDR(f)   ((uint32_t)(uintptr_t)(f))
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t p) { (void)p; }
#else
#define UM          ((volatile um_t *)0x1FFFD300u)
#define CBS         ((volatile cb_t *)0x1FFFB860u)   /* 045: Color Mode (cb.c) */
#define RS          ((volatile rs_t *)0x1FFF9A40u)   /* 045: Aufnahme-Einstellungen */
#define NAMES_RAM   ((volatile char *)0x1FFFB400u)
#define DESC_RAM    ((volatile uint8_t *)0x1FFFD4C0u)
#define MTCFG_RAM   ((volatile uint8_t *)0x1FFFD480u)
#define FLASH_LIST  ((const uint8_t *)0x0009BAC0u)        /* usb_descriptor_list im Flash (Eintrag 1 von 015 gepatcht) */
#define USB0_CONTROL (*(volatile uint8_t *)0x40072108u)
#define USB_CONFIGURATION (*(volatile uint8_t *)0x20000D67u)
#define MILLIS()    (*(volatile uint32_t *)0x20000D7Cu)
#define ORIG_NAME(i) ((i) < 16u ? ORIG_DELAY[(i)] : ORIG_REVERB[(i) - 16u])
#define HEAP_ALLOC(n) (((void *(*)(uint32_t))0x000030C1u)(n))
#define MENU_ADD    ((void *(*)(void *, void *, const char *, uint32_t, uint32_t))0x0003350Du)    /* MenuItem_ctor */
#define ENUM_ITEM   ((void *(*)(void *, void *, const char *, volatile int32_t *, uint32_t, const char *const *, stdfn_t *))0x00034F31u)
#define FN_MANAGER  0x00031A35u          /* Manager der Firmware fuer Funktoren mit einem Zeiger (Settings-Eintraege) */
#define MALLOC(n)   (((void *(*)(uint32_t))0x00056F5Du)(n))
#define FREE(p)     (((void (*)(void *))0x00056F6Du)(p))
#define F_OPEN      ((int (*)(void *, const char *, uint8_t))0x0000FB39u)
#define F_READ      ((int (*)(void *, void *, uint32_t, uint32_t *))0x0000FEB5u)
#define F_WRITE     ((int (*)(void *, const void *, uint32_t, uint32_t *))0x00010089u)
#define F_CLOSE     ((int (*)(void *))0x000104F5u)
#define FNADDR(f)   ((uint32_t)(uintptr_t)(f) | 1u)
static inline uint32_t irq_save(void) {
    uint32_t p; __asm volatile ("mrs %0, primask\n cpsid i" : "=r"(p) :: "memory"); return p;
}
static inline void irq_restore(uint32_t p) { __asm volatile ("msr primask, %0" :: "r"(p) : "memory"); }
/* Original-Namen (Firmware 1.6.0, von build.sh gegen firmware.bin geprueft) */
static const char *const ORIG_DELAY[16] = {
    (const char *)0xA6078u, (const char *)0xA6080u, (const char *)0xA6090u, (const char *)0xA60A0u,
    (const char *)0xA60ACu, (const char *)0xA60BCu, (const char *)0xA60C4u, (const char *)0xA60D4u,
    (const char *)0x9D3FCu, (const char *)0x9D404u, (const char *)0x9D410u, (const char *)0x9D41Cu,
    (const char *)0x9D428u, (const char *)0x9D434u, (const char *)0x9D43Cu, (const char *)0x9D44Cu };
static const char *const ORIG_REVERB[16] = {
    (const char *)0xA5FBCu, (const char *)0xA5FC8u, (const char *)0xA5FD0u, (const char *)0xA5FDCu,
    (const char *)0xA5FECu, (const char *)0xA5FF8u, (const char *)0xA6004u, (const char *)0xA6010u,
    (const char *)0xA6018u, (const char *)0xA6020u, (const char *)0xA602Cu, (const char *)0xA603Cu,
    (const char *)0xA6044u, (const char *)0xA6054u, (const char *)0xA605Cu, (const char *)0xA606Cu };
#endif

static const char CFG_PATH[] = "/usbaudio.cfg";

/* ------------------------------------------------------------------ Namen (Classic/Creative) */
static void set_names(int creative) {
    for (uint32_t i = 0; i < NNAMES; i++) {
        const char *s = creative ? FX_NAMES[i] : ORIG_NAME(i);
        volatile char *d = NAMES_RAM + i * NAMELEN;
        uint32_t k = 0;
        if (s) for (; k < NAMELEN - 1u && s[k]; k++) d[k] = s[k];
        for (; k < NAMELEN; k++) d[k] = 0;
    }
}

static void apply_creative(int on) {
    set_names(on);
    arp_set_creative(on);
}

/* ------------------------------------------------------------------ Initialisierung (usbinit_hook, vor usb_init) */
void um_early_init(void) {
    volatile um_t *u = UM;
    volatile uint32_t *z = (volatile uint32_t *)(void *)u;
    for (uint32_t i = 0; i < sizeof(um_t) / 4u; i++) z[i] = 0;
    for (uint32_t k = 0; k < NV; k++) { u->v[k] = VDEF[k]; u->saved[k] = VDEF[k]; u->applied[k] = VDEF[k]; u->seen[k] = VDEF[k]; }
    for (uint32_t i = 0; i < sizeof u->list; i++) u->list[i] = FLASH_LIST[i];   /* Eintrag 1 = {0x0200, 0, 0x1FFFD4C0, 365} */
    set_names(1);                         /* vor dem ersten Bildaufbau; arp_init laeuft spaeter (init_array) */
    CBS->mode = 0; CBS->saved = 0; CBS->seen = 0;      /* 045 */
    for (uint32_t q = 0; q < RS_N; q++) { RS->v[q] = 1; RS->saved[q] = 1; RS->seen[q] = 1; }   /* 045: Standard On */
    RS->magic = RS_MAGIC;
    u->t_load = LOAD_FIRST_MS;
    u->magic = UM_MAGIC;
}

/* ------------------------------------------------------------------ Menue */
static const char PAGE_NAME[] = "USB Audio";
static const char *const N_USB[4]     = { "Off", "Stereo", "Stereo + Multi", "Stereo + In" };
static const char *const N_PRESET[2]  = { "Master", "FX Return" };
static const char *const N_FADER[2]   = { "Pre Fader", "Post Fader" };
static const char *const N_MODE[2]    = { "Classic", "Creative" };

/* Aufruf durch das Menue nach einer Auswahl (std::function-Invoker): nur Zeitpunkt merken, angewendet wird in um_poll. */

static const char *const N_REC[2] = { "Mono", "Stereo" };
static const char *const N_ONOFF[2] = { "Off", "On" };
/* 045: Seite "Audio In" – Sends des USB-Eingangs in die Effekte der Play (vorher fest 25 %) */
#define NSEND 101                                     /* 0 = Off, 1…100 % */
static const char *const N_SEND[NSEND] = { "Off", "1%", "2%", "3%", "4%", "5%", "6%", "7%", "8%", "9%", "10%", "11%", "12%", "13%", "14%", "15%", "16%", "17%", "18%", "19%", "20%", "21%", "22%", "23%", "24%", "25%", "26%", "27%", "28%", "29%", "30%", "31%", "32%", "33%", "34%", "35%", "36%", "37%", "38%", "39%", "40%", "41%", "42%", "43%", "44%", "45%", "46%", "47%", "48%", "49%", "50%", "51%", "52%", "53%", "54%", "55%", "56%", "57%", "58%", "59%", "60%", "61%", "62%", "63%", "64%", "65%", "66%", "67%", "68%", "69%", "70%", "71%", "72%", "73%", "74%", "75%", "76%", "77%", "78%", "79%", "80%", "81%", "82%", "83%", "84%", "85%", "86%", "87%", "88%", "89%", "90%", "91%", "92%", "93%", "94%", "95%", "96%", "97%", "98%", "99%", "100%" };
#define SEND_Q16(i) ((int32_t)(((uint32_t)(i) * 65536u + 50u) / 100u))
static const char IN_PAGE[] = "Audio In";
static const char *const N_COLOR[3] = { "Normal", "Red-Green", "Blue-Yellow" };   /* 045: Color Mode */
static const char *const INKEY[2] = { "indly", "inrev" };
void ui_set_send(uint32_t which, int32_t q16) __attribute__((weak));   /* usbin.c (045) */

static void um_changed(void *any) { (void)any; if (UM->magic == UM_MAGIC) UM->t_change = MILLIS(); }

static void add_enum(void *parent, const char *name, volatile int32_t *val, uint32_t max, const char *const *tab) {
    stdfn_t fn;
    fn.store[0] = (uint32_t)(uintptr_t)(void *)UM; fn.store[1] = 0;
    fn.manager = FN_MANAGER; fn.invoker = FNADDR(um_changed);
    void *it = HEAP_ALLOC(0x60u);
    if (it) ENUM_ITEM(it, parent, name, (int32_t *)val, max, tab, &fn);
}

void ui_in_build(void *menu, void *parent);
/* Untermenue-Builder (Member-Funktionszeiger, this = Menue-Objekt, wie FUN_00036140 fuer "Settings") */
void um_build(void *menu, void *parent) {
    (void)menu;
    volatile um_t *u = UM;
    if (u->magic != UM_MAGIC) return;
    add_enum(parent, "Audio Out",       &u->v[V_USB],      3u, N_USB);
    add_enum(parent, "Multitrack 9/10", &u->v[V_PRESET],   1u, N_PRESET);
    add_enum(parent, "Track Level",     &u->v[V_FADER],    1u, N_FADER);
    add_enum(parent, "FX Mode",         &u->v[V_CREATIVE], 1u, N_MODE);
    add_enum(parent, "Rec",             &u->stereo,        1u, N_REC);   /* 026: Aufnahme Mono/Stereo (Standard Mono) */
    if (RS->magic == RS_MAGIC) {                                          /* 045: Aufnahme startet/pausiert nach Pegel */
        add_enum(parent, "Wait For Signal",  &RS->v[0], 1u, N_ONOFF);
        add_enum(parent, "Pause On Silence", &RS->v[1], 1u, N_ONOFF);
    }
    void *in = HEAP_ALLOC(0x5Cu);                                          /* 046: "Audio In" jetzt Unterseite von USB Audio */
    if (in) MENU_ADD(in, parent, IN_PAGE, FNADDR(ui_in_build), 0u);
}

/* 045: Seite "Audio In" (USB-Eingang bei Audio Out = Stereo + In) */
void ui_in_build(void *menu, void *parent) {
    (void)menu;
    volatile um_t *u = UM;
    if (u->magic != UM_MAGIC) return;
    add_enum(parent, "Delay Send",  &u->ins[0], NSEND - 1u, N_SEND);
    add_enum(parent, "Reverb Send", &u->ins[1], NSEND - 1u, N_SEND);
}

/* 046 (14.11.): Seite "About" – nur lesbare Zeilen (Auswahl mit genau einem Eintrag, Wert immer 0 in ab_t, boot.c).
 * Build = Code-CRC aus SN_HDR als 8 Hex-Stellen (ab_init schreibt sie nach 0x1FFFB6D0); auf dem Startbild die ersten 4. */
#include "version.h"
void ab_init(void) __attribute__((weak));
#define AB_ZERO(i) ((volatile int32_t *)(0x1FFFB6C0u + 4u * (i)))
static const char *const N_AB_VER[1]   = { ENVYU_VERSION };
static const char *const N_AB_BUILD[1] = { (const char *)0x1FFFB6D0u };
static const char *const N_AB_BASE[1]  = { ENVYU_BASE };
static const char *const N_AB_HW[1]    = { "Rev A" };
static void ab_noop(void *any) { (void)any; }
static void add_info(void *parent, const char *name, uint32_t slot, const char *const *tab) {
    stdfn_t fn;
    fn.store[0] = 0; fn.store[1] = 0;
    fn.manager = FN_MANAGER; fn.invoker = FNADDR(ab_noop);
    *AB_ZERO(slot) = 0;
    void *it = HEAP_ALLOC(0x60u);
    if (it) ENUM_ITEM(it, parent, name, (int32_t *)AB_ZERO(slot), 0u, tab, &fn);
}
void ab_build(void *menu, void *parent) {
    (void)menu;
    if (ab_init) ab_init();                               /* Build-Text frisch aus SN_HDR */
    add_info(parent, "Version",  0u, N_AB_VER);
    add_info(parent, "Build",    1u, N_AB_BUILD);
    add_info(parent, "Based On", 2u, N_AB_BASE);
    add_info(parent, "Hardware", 3u, N_AB_HW);
}
static const char AB_PAGE[] = "About";

/* 046 (14.11.): Seite "envyu Settings" – Color Mode, Keep Selection, About. "Engines" steht seit 17.11. im Hauptmenue (Nutzer, wie 048).
 * Die Firmware-Schrift hat kein ū, daher "envyu". */
static const char ENV_PAGE[] = "envyu Settings";
void sl_menu_add(void *parent) __attribute__((weak));   /* Patch 021 (slicer.c): Seite "Engines" */
void env_build(void *menu, void *parent) {
    (void)menu;
    add_enum(parent, "Color Mode", &CBS->mode, 2u, N_COLOR);              /* 045 */
    if (RS->magic == RS_MAGIC) add_enum(parent, "Keep Selection", &RS->v[2], 1u, N_ONOFF);   /* 12.11.: Spurauswahl beim Wechsel Perform ↔ Pattern */
    void *it = HEAP_ALLOC(0x5Cu);
    if (it) MENU_ADD(it, parent, AB_PAGE, FNADDR(ab_build), 0u);          /* 046: Version / Build / Basis */
}

/* ersetzt bei 0x33676 den Aufruf MenuItem_ctor(item, parent, "Settings", builder, 0): zuerst "USB Audio" einfuegen */
__attribute__((used, section(".text.entry_menu")))
void *um_menu_hook(void *item, void *parent, const char *name, uint32_t fn, uint32_t adj) {
    if (UM->magic == UM_MAGIC) {
#ifndef HOST_TEST
        if (sl_menu_add) sl_menu_add(parent);                            /* 17.11.: "Engines" im Hauptmenue (Nutzer, wie 048) */
#endif
        void *it = HEAP_ALLOC(0x5Cu);
        if (it) MENU_ADD(it, parent, PAGE_NAME, FNADDR(um_build), 0u);    /* USB Audio bleibt oben (oft gebraucht) */
        void *it2 = HEAP_ALLOC(0x5Cu);
        if (it2) MENU_ADD(it2, parent, ENV_PAGE, FNADDR(env_build), 0u);  /* 046: alles Weitere von uns gebuendelt */
    } else if (sl_menu_add) sl_menu_add(parent);
    return MENU_ADD(item, parent, name, fn, adj);
}

/* ------------------------------------------------------------------ Datei */
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void parse_cfg(const char *b, uint32_t n) {
    volatile um_t *u = UM;
    uint32_t i = 0;
    while (i < n) {
        uint32_t s = i;
        while (i < n && b[i] != '\n') i++;
        uint32_t e = i++;                                   /* Zeile [s, e) */
        if (e > s && b[e - 1] == '\r') e--;
        if (e == s || b[s] == '#') continue;
        uint32_t eq = s;
        while (eq < e && b[eq] != '=') eq++;
        if (eq == e || eq + 1u == e) continue;
        int32_t val = 0; uint32_t nd = 0, ok = 1;
        for (uint32_t k = eq + 1u; k < e; k++) {
            if (b[k] < '0' || b[k] > '9' || nd >= 3u) { ok = 0; break; }
            val = val * 10 + (b[k] - '0'); nd++;
        }
        if (!ok) continue;
        for (uint32_t k = 0; k < NV; k++) {
            uint32_t kl = slen(VKEY[k]);
            if (kl != eq - s) continue;
            uint32_t m = 0; while (m < kl && b[s + m] == VKEY[k][m]) m++;
            if (m == kl && val <= VMAX[k]) u->v[k] = val;
        }
        if (eq - s == 6u && b[s] == 's' && b[s + 1] == 't' && b[s + 2] == 'e' && b[s + 3] == 'r' && b[s + 4] == 'e' && b[s + 5] == 'o' && val <= 1)
            u->stereo = val;                                /* 026 */
        for (uint32_t q = 0; q < 2u; q++)                   /* 045: indly= / inrev= */
            if (eq - s == 5u && val < NSEND) { uint32_t m = 0; while (m < 5u && b[s + m] == INKEY[q][m]) m++; if (m == 5u) u->ins[q] = val; }
        if (eq - s == 5u && b[s] == 'c' && b[s + 1] == 'o' && b[s + 2] == 'l' && b[s + 3] == 'o' && b[s + 4] == 'r' && val <= 2) CBS->mode = val;   /* 045 */
        for (uint32_t q = 0; q < RS_N; q++)                 /* 045: rwait= / rpause= (12.11.: keepsel=) */
            if (eq - s == slen(RSKEY[q]) && val <= 1) { uint32_t m = 0; while (m < eq - s && b[s + m] == RSKEY[q][m]) m++; if (m == eq - s) RS->v[q] = val; }
    }
}

static void try_load(void) {
    volatile um_t *u = UM;
    uint32_t now = MILLIS();
    if ((int32_t)(now - u->t_load) < 0) return;
    uint32_t tries = ((u->load_res >> 8) & 0xFFu) + 1u;
    u->load_res = (tries & 0xFFu) << 8;
    void *fil = MALLOC(FIL_SIZE);
    char *buf = (char *)MALLOC(320u);                   /* 12.11.: 256 → 320 (Kopf 160 + Zeilen bis 94) */
    if (!fil || !buf) { if (fil) FREE(fil); if (buf) FREE(buf); u->t_load = now + LOAD_RETRY_MS; return; }
    for (uint32_t i = 0; i < FIL_SIZE; i++) ((volatile uint8_t *)fil)[i] = 0;
    int r = F_OPEN(fil, CFG_PATH, FA_READ);
    if (r == 0) {
        uint32_t br = 0;
        if (F_READ(fil, buf, 319u, &br) == 0 && br <= 319u) parse_cfg(buf, br);   /* 12.11.: Puffer 320 */
        F_CLOSE(fil);
        u->load_res |= 0x10000u;
        u->loaded = 1;
    } else if (r == FR_NO_FILE || r == FR_NO_PATH || now >= LOAD_GIVEUP_MS) {
        u->loaded = 1;                                      /* keine Datei: Standardwerte (werden erst bei Aenderung gespeichert) */
    } else {
        u->t_load = now + LOAD_RETRY_MS;                    /* SD noch nicht bereit */
    }
    u->load_res |= (uint32_t)r & 0xFFu;
    FREE(buf); FREE(fil);
    /* Stand der Datei: gelesene Werte bzw. ohne Datei die Standardwerte – eine Aenderung im Menue vor dem Lesen
     * (SD noch nicht bereit) wird dadurch anschliessend gespeichert */
    if (u->loaded) for (uint32_t k = 0; k < NV; k++) { u->saved[k] = (u->load_res & 0x10000u) ? u->v[k] : VDEF[k]; u->seen[k] = u->v[k]; }
    if (u->loaded) { u->stereo_saved = (u->load_res & 0x10000u) ? u->stereo : 0; u->stereo_seen = u->stereo; }
    if (u->loaded) for (uint32_t q = 0; q < 2u; q++) { u->ins_saved[q] = (u->load_res & 0x10000u) ? u->ins[q] : 0; u->ins_seen[q] = u->ins[q]; }
    if (u->loaded) { CBS->saved = (u->load_res & 0x10000u) ? CBS->mode : 0; CBS->seen = CBS->mode; }   /* 045 */
    if (u->loaded) for (uint32_t q = 0; q < RS_N; q++) { RS->saved[q] = (int8_t)((u->load_res & 0x10000u) ? RS->v[q] : 1); RS->seen[q] = (int8_t)RS->v[q]; }
}

static void put_line(char *b, uint32_t *n, const char *key, int32_t v) {
    for (const char *s = key; *s; s++) b[(*n)++] = *s;
    b[(*n)++] = '=';
    if (v >= 100) b[(*n)++] = (char)('0' + v / 100 % 10);   /* 045: Sends bis 100 */
    if (v >= 10) b[(*n)++] = (char)('0' + v / 10 % 10);
    b[(*n)++] = (char)('0' + v % 10);
    b[(*n)++] = '\n';
}

static void save(void) {
    volatile um_t *u = UM;
    static const char HDR[] = "# Polyend Play USB Audio (Patch 017): usb 0=Off 1=Stereo 2=Stereo+Multi 3=Stereo+In, mt 0=Master 1=FX Return, fader 0=Pre 1=Post, creative 0=Classic 1=Creative\n";   /* nicht verlaengern: Puffer 256, Kopf max. 200 */
    void *fil = MALLOC(FIL_SIZE);
    char *buf = (char *)MALLOC(320u);                   /* 12.11.: 256 → 320 (Kopf 160 + Zeilen bis 94) */
    int32_t snap[NV]; int ok = 0;
    for (uint32_t k = 0; k < NV; k++) snap[k] = u->v[k];
    int32_t snap_st = u->stereo ? 1 : 0;
    int32_t snap_in[2] = { u->ins[0], u->ins[1] };
    int32_t snap_cb = CBS->mode;
    int32_t snap_rs[RS_N] = { RS->v[0], RS->v[1], RS->v[2] };
    if (fil && buf) {
        uint32_t n = 0;
        for (const char *s = HDR; *s && n < 200u; s++) buf[n++] = *s;
        for (uint32_t k = 0; k < NV; k++) put_line(buf, &n, VKEY[k], snap[k]);
        put_line(buf, &n, "stereo", snap_st);
        put_line(buf, &n, INKEY[0], snap_in[0]);            /* 045 */
        put_line(buf, &n, INKEY[1], snap_in[1]);
        put_line(buf, &n, "color", snap_cb);                /* 045 */
        put_line(buf, &n, RSKEY[0], snap_rs[0]);            /* 045 (11.11.) */
        put_line(buf, &n, RSKEY[1], snap_rs[1]);
        put_line(buf, &n, RSKEY[2], snap_rs[2]);            /* 12.11.: keepsel= */
        for (uint32_t i = 0; i < FIL_SIZE; i++) ((volatile uint8_t *)fil)[i] = 0;
        uint32_t bw = 0;
        int r = F_OPEN(fil, CFG_PATH, FA_WRITE_NEW);
        if (r == 0) {
            int w = F_WRITE(fil, buf, n, &bw);
            int c = F_CLOSE(fil);
            if (w == 0 && c == 0 && bw == n) ok = 1;
        }
    }
    if (fil) FREE(fil);
    if (buf) FREE(buf);
    if (ok) { u->saves++; u->save_fail = 0; }
    else { u->save_err++; u->save_fail++; }
    if (ok || u->save_fail >= 5u) { for (uint32_t k = 0; k < NV; k++) u->saved[k] = snap[k]; u->stereo_saved = snap_st; u->ins_saved[0] = snap_in[0]; u->ins_saved[1] = snap_in[1]; CBS->saved = snap_cb; RS->saved[0] = (int8_t)snap_rs[0]; RS->saved[1] = (int8_t)snap_rs[1]; RS->saved[2] = (int8_t)snap_rs[2]; }   /* Karte defekt: nicht endlos */
    u->t_change = MILLIS();                                 /* naechster Versuch fruehestens nach SAVE_DELAY_MS */
}

/* ------------------------------------------------------------------ USB-Neuanmeldung */
static void reenum_poll(void) {
    volatile um_t *u = UM;
    uint32_t now = MILLIS();
    if (u->re_state == 0u) {
        if (u->v[V_USB] == u->applied[V_USB]) return;
        uint32_t p = irq_save();
        USB0_CONTROL = 0;                                   /* D+-Pullup aus: Host sieht "Kabel ab" */
        USB_CONFIGURATION = 0;                              /* Teensy-Sender halten an (wie nach Bus-Reset) */
        ua_force_stop();
        mt_force_stop();
        ui_force_stop();                                    /* 020 */
        irq_restore(p);
        u->re_state = 1; u->re_t0 = now;
        return;
    }
    if (now - u->re_t0 < REENUM_MS) return;
    int32_t m = u->v[V_USB];
    uint32_t len = m == 3 ? UI_DESC_LEN : m == 2 ? DESC_FULL_LEN : m == 1 ? UM_DESC_ST_LEN : UM_DESC_OFF_LEN;
    const uint8_t *src = m == 3 ? UI_DESC_IN : m == 1 ? UM_DESC_ST : ua_cfg_desc;
    volatile uint8_t *d = DESC_RAM;
    for (uint32_t i = 0; i < len; i++) d[i] = src[i];
    if (m == 0) { d[2] = (uint8_t)UM_DESC_OFF_LEN; d[3] = 0; d[4] = 3; }   /* nur CDC (IF0/1) + MIDI (IF2) */
    u->list[12 + 8] = (uint8_t)len; u->list[12 + 9] = (uint8_t)(len >> 8);  /* Eintrag 1, Laenge */
    __asm__ volatile("" ::: "memory");
    ui_set_mode(m == 3);                                    /* 020: IF5/EP7 gehoeren dann usbin.c */
    u->applied[V_USB] = m;
    u->re_state = 0; u->reenums++;
    USB0_CONTROL = 0x10u;                                   /* USB_CONTROL_DPPULLUPNONOTG, wie usb_init */
}

/* ------------------------------------------------------------------ Hauptschleife */
void um_poll(void) {
    volatile um_t *u = UM;
    if (u->magic != UM_MAGIC) return;                       /* ohne frueh-Init (unmoeglich) nichts anfassen */
    for (uint32_t k = 0; k < NV; k++) if (u->v[k] < 0 || u->v[k] > VMAX[k]) u->v[k] = VDEF[k];
    if (!u->loaded) try_load();

    /* Multitrack-Konfiguration (Patch 015) */
    if (u->v[V_PRESET] != u->applied[V_PRESET] || u->v[V_FADER] != u->applied[V_FADER]) {
        volatile uint8_t *c = MTCFG_RAM;
        uint32_t mg = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
        if (mg == MT_CFG_MAGIC) {
            c[4] = (uint8_t)u->v[V_PRESET];                 /* mt_cfg_t.preset (0 = A, 1 = B) */
            c[5] = (uint8_t)((c[5] & ~1u) | (uint32_t)u->v[V_FADER]);   /* flags Bit 0 = Spur-Lautstaerke */
        }
        u->applied[V_PRESET] = u->v[V_PRESET]; u->applied[V_FADER] = u->v[V_FADER];
    }
    if (u->v[V_CREATIVE] != u->applied[V_CREATIVE]) {
        apply_creative(u->v[V_CREATIVE] != 0);
        u->applied[V_CREATIVE] = u->v[V_CREATIVE];
    }
    reenum_poll();
    for (uint32_t q = 0; q < 2u; q++) {                     /* 045: Sends des Eingangs, laufend angewendet (billig) */
        if (u->ins[q] < 0 || u->ins[q] >= NSEND) u->ins[q] = 0;
        if (ui_set_send) ui_set_send(q, SEND_Q16(u->ins[q]));
    }

    /* Speichern: erst nach dem Lesen, 1,5 s nach der letzten Aenderung */
    if (!u->loaded) return;
    uint32_t now = MILLIS(), diff = 0, moved = 0;
    for (uint32_t k = 0; k < NV; k++) {
        if (u->v[k] != u->seen[k]) { u->seen[k] = u->v[k]; moved = 1; }
        if (u->v[k] != u->saved[k]) diff = 1;
    }
    if (u->stereo < 0 || u->stereo > 1) u->stereo = 0;
    if (u->stereo != u->stereo_seen) { u->stereo_seen = u->stereo; moved = 1; }
    if (u->stereo != u->stereo_saved) diff = 1;
    for (uint32_t q = 0; q < 2u; q++) {                     /* 045 */
        if (u->ins[q] != u->ins_seen[q]) { u->ins_seen[q] = u->ins[q]; moved = 1; }
        if (u->ins[q] != u->ins_saved[q]) diff = 1;
    }
    if (CBS->mode < 0 || CBS->mode > 2) CBS->mode = 0;   /* 045 */
    if (CBS->mode != CBS->seen) { CBS->seen = CBS->mode; moved = 1; }
    if (CBS->mode != CBS->saved) diff = 1;
    if (RS->magic == RS_MAGIC) for (uint32_t q = 0; q < RS_N; q++) {   /* 045 (11.11.) */
        if (RS->v[q] < 0 || RS->v[q] > 1) RS->v[q] = 1;
        if (RS->v[q] != RS->seen[q]) { RS->seen[q] = (int8_t)RS->v[q]; moved = 1; }
        if (RS->v[q] != RS->saved[q]) diff = 1;
    }
    if (moved) u->t_change = now;
    if (diff && now - u->t_change >= SAVE_DELAY_MS) save();
}

/* fuer usbup.c (Befehl CFG) */
int um_rec_stereo(void) { return UM->magic == UM_MAGIC && UM->stereo == 1; }
/* 045 (11.11.): 1 = Aufnahme wartet auf Signal / pausiert bei Stille (ohne gueltige Einstellung: On) */
int um_rec_wait(void)  { return RS->magic != RS_MAGIC || RS->v[0] != 0; }
int um_rec_pause(void) { return RS->magic != RS_MAGIC || RS->v[1] != 0; }
int um_keep_sel(void)  { return RS->magic != RS_MAGIC || RS->v[2] != 0; }   /* 12.11.: pf.c */
int um_get(uint32_t k) { return (UM->magic == UM_MAGIC && k < NV) ? (int)UM->v[k] : -1; }
int um_set(uint32_t k, int32_t v) {
    if (UM->magic != UM_MAGIC || k >= NV || v < 0 || v > VMAX[k]) return 0;
    UM->v[k] = v;
    return 1;
}
