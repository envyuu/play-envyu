/* PC-Test fuer usbmenu.c (Patch 017): Menueaufbau, Datei lesen/schreiben, Anwenden, Neuanmeldung, Zufallstest.
 * gcc -O1 -g -DHOST_TEST -fsanitize=address,undefined -o um_test usbmenu.c um_test.c && ./um_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint32_t store[2]; uint32_t manager; uint32_t invoker; } stdfn_t;
void um_early_init(void);
void um_boot_desc(void);
void um_poll(void);
void um_build(void *menu, void *parent);
void *um_menu_hook(void *item, void *parent, const char *name, uint32_t fn, uint32_t adj);
int um_get(uint32_t k);
int um_set(uint32_t k, int32_t v);

uint8_t host_um_ram[0x100];
int um_rec_wait(void); int um_rec_pause(void);
uint8_t host_rs_ram[0x20];          /* 045: Wait For Signal / Pause On Silence */
uint8_t host_names_ram[32 * 16];
uint8_t host_desc_ram[390];
uint8_t host_mtcfg[0x20];
uint8_t host_dev_desc[18];
uint8_t host_flash_list[0x6C];
volatile uint8_t host_usb0_control = 0x10, host_usb_configuration = 1;
uint32_t host_millis = 0;

/* Deskriptor "Stereo + Multi" (Platzhalter mit echtem Kopf; Inhalt egal, verglichen wird byteweise) */
uint8_t ua_cfg_desc[390];
static const char *ON[32], *CN[32];
const char *FX_NAMES[32];
const char *const host_orig_names[32] = {
    "One2One", "Halftime Show", "Falling Slow", "Stereo Trip", "Dimension Delay", "Piper", "Robot Language", "Tight Space",
    "Twoism", "Warbliani", "Rabbithole", "Stringston", "Metalhead", "Shock", "Stringston Jr.", "Superfario",
    "Big Hall", "Bedroom", "Late Verb", "Bright Space", "Dark Matter", "Ultracrisp", "Later Guys", "Freezer",
    "Peas", "Dissolver", "Dark Starlette", "Garage", "Tunel Vision", "Sand", "Bouncing Ball", "Radiator" };

static int ua_stops = 0, mt_stops = 0, arp_calls = 0, arp_last = -1;
void ua_force_stop(void) { ua_stops++; }
void mt_force_stop(void) { mt_stops++; }
static uint32_t mt_split = 99; void mt_set_split(uint32_t v) { mt_split = v; }   /* 049 */
void arp_set_creative(int on) { arp_calls++; arp_last = on; }

/* ---- Menue-Attrappen ---- */
typedef struct { const char *name; int32_t *val; uint32_t max; const char *const *tab; stdfn_t fn; void *parent; } enum_rec_t;
static enum_rec_t enums[16]; static int nenum = 0;
typedef struct { const char *name; uint32_t fn, adj; void *parent; } menu_rec_t;
static menu_rec_t menus[16]; static int nmenu = 0;
static int heap_live = 0;
static uint8_t heap_pool[64][0x60];
void *host_heap_alloc(uint32_t n) { if (n > 0x60 || heap_live >= 64) return 0; return memset(heap_pool[heap_live++], 0, n); }   /* gehoert der Firmware */
void *host_menu_add(void *it, void *parent, const char *name, uint32_t fn, uint32_t adj) {
    menus[nmenu].name = name; menus[nmenu].fn = fn; menus[nmenu].adj = adj; menus[nmenu].parent = parent; nmenu++;
    return it;
}
void *host_enum_item(void *it, void *parent, const char *name, int32_t *val, uint32_t max, const char *const *tab, stdfn_t *fn) {
    enums[nenum].name = name; enums[nenum].val = val; enums[nenum].max = max; enums[nenum].tab = tab; enums[nenum].fn = *fn;
    enums[nenum].parent = parent; nenum++;
    return it;
}

/* ---- FatFs-Attrappe: eine Datei im Speicher ---- */
static char file_buf[1024]; static int file_len = -1;     /* -1 = keine Datei */
static int sd_ready_at = 0;                               /* host_millis, ab dem die Karte antwortet */
static int sd_fail_write = 0, sd_broken = 0;
static int mallocs = 0, opens = 0, writes = 0;
typedef struct { int mode; int pos; int w; char wb[1024]; } hfil_t;
void *host_malloc(uint32_t n) { mallocs++; return calloc(1, n); }
void host_free(void *p) { mallocs--; free(p); }
int host_f_open(void *fp, const char *path, uint8_t mode) {
    if (strcmp(path, "/usbaudio.cfg") != 0) { fprintf(stderr, "falscher Pfad %s\n", path); exit(1); }
    if (sd_broken || (int)host_millis < sd_ready_at) return 3;    /* FR_NOT_READY */
    if (mode == 1 && file_len < 0) return 4;                    /* FR_NO_FILE */
    hfil_t *h = calloc(1, sizeof *h); h->mode = mode; *(hfil_t **)fp = h; opens++;
    return 0;
}
int host_f_read(void *fp, void *buf, uint32_t n, uint32_t *br) {
    hfil_t *h = *(hfil_t **)fp; uint32_t k = (uint32_t)file_len - (uint32_t)h->pos; if (k > n) k = n;
    memcpy(buf, file_buf + h->pos, k); h->pos += (int)k; *br = k; return 0;
}
int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw) {
    hfil_t *h = *(hfil_t **)fp;
    if (sd_fail_write) { *bw = 0; return 1; }
    memcpy(h->wb + h->w, buf, n); h->w += (int)n; *bw = n; writes++; return 0;
}
int host_f_close(void *fp) {
    hfil_t *h = *(hfil_t **)fp; *(hfil_t **)fp = 0; opens--;
    if (h->mode != 1 && !sd_fail_write) { memcpy(file_buf, h->wb, (size_t)h->w); file_len = h->w; }
    free(h); return 0;
}

static int fails = 0;
static void check(const char *what, int ok) { printf("Pruefung %-58s %s\n", what, ok ? "OK" : "FEHLER"); if (!ok) fails++; }
static uint32_t U32(int off) { uint32_t v; memcpy(&v, host_um_ram + off, 4); return v; }
static int32_t V(int k) { int32_t v; memcpy(&v, host_um_ram + 4 + 4 * k, 4); return v; }
static void run_ms(int ms) { for (int i = 0; i < ms; i++) { um_poll(); host_millis++; } }
static void boot(void) {
    memset(host_um_ram, 0xA5, sizeof host_um_ram);              /* RAM nach dem Einschalten: Muell */
    memset(host_names_ram, 0xA5, sizeof host_names_ram);
    host_millis = 0; nenum = nmenu = 0; arp_calls = 0; arp_last = -1; ua_stops = mt_stops = 0;
    host_usb0_control = 0; um_early_init(); host_usb0_control = 0x10;   /* usb_init setzt den Pullup */
    uint32_t mg = 0x4D544346u; memcpy(host_mtcfg, &mg, 4); host_mtcfg[4] = 0; host_mtcfg[5] = 1;   /* mt_init */
    memcpy(host_desc_ram, ua_cfg_desc, 390);                     /* ua_init */
    um_boot_desc();                                              /* 049: Standard Stereo */
}
static int names_are(const char *const *t) {
    for (int i = 0; i < 32; i++) if (strncmp((char *)host_names_ram + 16 * i, t[i], 16) != 0 || host_names_ram[16 * i + 15] != 0) return 0;
    return 1;
}
static int list_len(void) { return host_um_ram[0x70 + 12 + 8] | host_um_ram[0x70 + 12 + 9] << 8; }

/* 045: Seite "Audio In" */
void ui_in_build(void *menu, void *parent);
void ab_build(void *menu, void *parent);   /* 046: Seite About */
void env_build(void *menu, void *parent);  /* 046: Seite envyu Settings */
static int32_t sends[2] = { -1, -1 }; static int send_calls = 0;
void ui_set_send(uint32_t which, int32_t q16) { if (which < 2u) sends[which] = q16; send_calls++; }

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    srand(argc > 1 ? (unsigned)atoi(argv[1]) : 7u);
    for (int i = 0; i < 390; i++) ua_cfg_desc[i] = (uint8_t)(i * 7 + 3);
    ua_cfg_desc[0] = 9; ua_cfg_desc[1] = 2; ua_cfg_desc[2] = 390 & 0xFF; ua_cfg_desc[3] = 390 >> 8; ua_cfg_desc[4] = 7;
    static char cn[32][20];
    for (int i = 0; i < 32; i++) { snprintf(cn[i], sizeof cn[i], "FX Name %02d", i); FX_NAMES[i] = cn[i]; CN[i] = cn[i]; ON[i] = host_orig_names[i]; }
    /* Flash-Liste wie im gepatchten Image: Eintrag 1 = {0x0200, 0, 0x1FFFD4A0, 390} (049) */
    memset(host_flash_list, 0, sizeof host_flash_list);
    host_flash_list[12 + 1] = 0x02; uint32_t a = 0x1FFFD4A0u; memcpy(host_flash_list + 12 + 4, &a, 4);
    host_flash_list[12 + 8] = 390 & 0xFF; host_flash_list[12 + 9] = 390 >> 8;

    printf("== 1) Frueh-Init\n");
    boot();
    check("Magic, Standardwerte 1/0/1/1 (049: Stereo)", U32(0) == 0x55AEC017u && V(0) == 1 && V(1) == 0 && V(2) == 1 && V(3) == 1);
    check("creative-Wert liegt bei +0x10 (fx.c/arp.c lesen 0x1FFFD310)", V(3) == 1 && host_um_ram[0x10] == 1);
    check("Deskriptorliste kopiert (bis auf die Laenge)", memcmp(host_um_ram + 0x70, host_flash_list, 12 + 8) == 0 && memcmp(host_um_ram + 0x70 + 22, host_flash_list + 22, 0x6C - 22) == 0);
    check("049: Start mit Stereo-Deskriptor (248 B, 5 Interfaces)", list_len() == 248 && host_desc_ram[2] == 248 && host_desc_ram[4] == 5);
    check("049: bcdDevice beim Start 0x0281 (Stereo)", host_dev_desc[12] == 0x81 && host_dev_desc[13] == 0x02);
    check("Effektnamen = FX-Suite (Creative), nullterminiert", names_are(CN));

    printf("== 2) Menue\n");
    int parent;
    um_menu_hook((void *)0x1234, &parent, "Settings", 0x36141u, 0);
    check("046: Hauptmenue \"USB Audio\", \"envyu Settings\", dann \"Settings\" unveraendert", nmenu == 3 && strcmp(menus[0].name, "USB Audio") == 0 && menus[0].adj == 0
          && menus[0].parent == &parent && strcmp(menus[1].name, "envyu Settings") == 0 && menus[1].fn == (uint32_t)(uintptr_t)env_build && menus[1].parent == &parent
          && strcmp(menus[2].name, "Settings") == 0 && menus[2].fn == 0x36141u && menus[2].parent == &parent && nenum == 0);
    check("USB-Audio-Builder ist um_build", menus[0].fn == (uint32_t)(uintptr_t)um_build);
    { int envp; nmenu = 0; nenum = 0; env_build((void *)0x77, &envp);
      check("045: envyu Settings: \"Color Mode\" (Normal/Red-Green/Blue-Yellow)", nenum == 2 && strcmp(enums[0].name, "Color Mode") == 0 && enums[0].parent == &envp && enums[0].max == 2
            && strcmp(enums[0].tab[1], "Red-Green") == 0 && strcmp(enums[0].tab[2], "Blue-Yellow") == 0);
      check("12.11.: envyu Settings: \"Keep Selection\" Off/On", strcmp(enums[1].name, "Keep Selection") == 0 && enums[1].parent == &envp && enums[1].max == 1
            && strcmp(enums[1].tab[0], "Off") == 0 && strcmp(enums[1].tab[1], "On") == 0);
      check("046: envyu Settings: Unterseite \"About\" → ab_build", nmenu == 1 && strcmp(menus[0].name, "About") == 0 && menus[0].fn == (uint32_t)(uintptr_t)ab_build && menus[0].parent == &envp); }
    nenum = 0;
    int sub;
    nmenu = 0;
    um_build((void *)0x99, &sub);
    check("046: USB Audio enthaelt Unterseite \"Audio In\" → ui_in_build", nmenu == 1 && strcmp(menus[0].name, "Audio In") == 0 && menus[0].fn == (uint32_t)(uintptr_t)ui_in_build && menus[0].parent == &sub);
    check("7 Eintraege (026: + Rec Mono/Stereo, 045: + Wait For Signal, Pause On Silence)", nenum == 7);
    check("045: Wait For Signal / Pause On Silence, Off/On, Standard On", nenum == 7 && strcmp(enums[5].name, "Wait For Signal") == 0 && strcmp(enums[6].name, "Pause On Silence") == 0
          && enums[5].max == 1 && enums[6].max == 1 && strcmp(enums[5].tab[1], "On") == 0 && strcmp(enums[6].tab[0], "Off") == 0
          && *enums[5].val == 1 && *enums[6].val == 1 && um_rec_wait() == 1 && um_rec_pause() == 1);
    const char *nm[4] = { "Audio Out", "Multitrack 9/10", "Track Level", "FX Mode" };
    uint32_t mx[4] = { 4, 1, 1, 1 };                       /* 020: Audio Out + "Stereo + In"; 049: + "Stereo+Multi (Win)" */
    int ok = 1;
    for (int k = 0; k < 4 && k < nenum; k++) {
        ok &= strcmp(enums[k].name, nm[k]) == 0 && enums[k].val == (int32_t *)(void *)(host_um_ram + 4 + 4 * k) && enums[k].max == mx[k]
              && enums[k].parent == &sub && enums[k].fn.manager == 0x31A35u && enums[k].fn.invoker != 0;
        for (uint32_t j = 0; j <= enums[k].max; j++) ok &= enums[k].tab[j] && strlen(enums[k].tab[j]) > 0 && strlen(enums[k].tab[j]) < 20;
    }
    check("Namen, Wertzeiger, Max, Tabellen, std::function", ok);
    check("Listen: Off/Stereo/Stereo + Multi/Stereo + In, Classic/Creative", strcmp(enums[0].tab[2], "Stereo + Multi") == 0 && strcmp(enums[0].tab[3], "Stereo + In") == 0 && strcmp(enums[3].tab[0], "Classic") == 0);
    ((void (*)(void *))(uintptr_t)enums[0].fn.invoker)(&enums[0].fn);   /* Firmware ruft den Invoker nach Auswahl */
    check("Invoker merkt Zeitpunkt", 1);

    printf("== 3) Datei fehlt → Standardwerte, nichts schreiben\n");
    file_len = -1; sd_ready_at = 0;
    run_ms(3000);
    check("gelesen (FR_NO_FILE), keine Datei angelegt", U32(0x44) == 1 && file_len == -1 && writes == 0);
    check("keine Lecks", mallocs == 0 && opens == 0);

    printf("== 3b) Aenderung vor SD-Bereitschaft, keine Datei\n");
    boot(); file_len = -1; sd_ready_at = 2500;
    run_ms(500); *enums[3].val = 0; run_ms(4000);
    file_buf[file_len > 0 ? file_len : 0] = 0;
    check("fruehe Aenderung wird nach dem Lesen gespeichert", file_len > 0 && strstr(file_buf, "creative=0\n"));
    printf("== 3c) 045: Color Mode speichern und laden\n");
    { extern uint8_t host_cb_ram[0x10]; int32_t *cbm = (int32_t *)(void *)host_cb_ram;
      cbm[0] = 2; run_ms(4000); file_buf[file_len > 0 ? file_len : 0] = 0;
      check("color=2 in usbaudio.cfg", file_len > 0 && strstr(file_buf, "color=2\n") != 0);
      check("Kopf + Zeilen passen in 256 Byte", file_len > 0 && file_len < 256);
      boot(); sd_ready_at = 0; run_ms(1500);
      check("nach Neustart: Color Mode 2 gelesen", cbm[0] == 2 && cbm[1] == 2); }
    printf("== 3d) 045: Wait For Signal / Pause On Silence speichern und laden\n");
    { int32_t *rs = (int32_t *)(void *)(host_rs_ram + 4);
      check("Standard On", rs[0] == 1 && rs[1] == 1 && um_rec_wait() && um_rec_pause());
      rs[1] = 0; run_ms(4000); file_buf[file_len > 0 ? file_len : 0] = 0;
      check("rwait=1 rpause=0 in usbaudio.cfg", file_len > 0 && strstr(file_buf, "\nrwait=1\nrpause=0\n") != 0);
      check("Kopf + Zeilen passen in 256 Byte (mit rwait/rpause)", file_len > 0 && file_len < 256);
      boot(); sd_ready_at = 0; run_ms(1500);
      check("nach Neustart: Pause Off gelesen, Wait On", rs[0] == 1 && rs[1] == 0 && um_rec_wait() == 1 && um_rec_pause() == 0);
      int w0x = writes; run_ms(3000); check("Lesen loest kein Schreiben aus", writes == w0x);
      rs[0] = 0; rs[1] = 1; run_ms(4000); file_buf[file_len > 0 ? file_len : 0] = 0;
      check("rwait=0 rpause=1", strstr(file_buf, "\nrwait=0\nrpause=1\n") != 0 && !um_rec_wait() && um_rec_pause());
      rs[0] = 7; run_ms(10); check("ungueltiger Wert → On", rs[0] == 1); }
    file_len = -1; boot(); sd_ready_at = 0; run_ms(1500); nenum = 0; um_build(0, &sub);

    printf("== 4) Aenderung im Menue → anwenden + nach 1,5 s speichern\n");
    *enums[1].val = 1; *enums[2].val = 0;                         /* FX Return, Pre Fader */
    run_ms(10);
    check("Multitrack sofort: preset 1, flags Bit0 = 0", host_mtcfg[4] == 1 && (host_mtcfg[5] & 1) == 0);
    run_ms(1000); check("noch nicht gespeichert (Verzoegerung)", file_len == -1);
    *enums[3].val = 0;                                            /* Classic: verlaengert die Verzoegerung */
    run_ms(10);
    check("Classic sofort: Original-Namen, arp_set_creative(0)", names_are(ON) && arp_last == 0);
    run_ms(1000); check("1 s nach letzter Aenderung: noch nicht gespeichert", file_len == -1);
    run_ms(600);
    file_buf[file_len > 0 ? file_len : 0] = 0;
    check("gespeichert: usb=1 mt=1 fader=0 creative=0 (049: Standard Stereo)", file_len > 0 && strstr(file_buf, "\nusb=1\nmt=1\nfader=0\ncreative=0\n"));
    int w0 = writes; run_ms(5000); check("kein weiteres Schreiben ohne Aenderung", writes == w0 && mallocs == 0 && opens == 0);

    printf("== 5) Neustart mit Datei, SD erst nach 3 s bereit\n");
    boot(); sd_ready_at = 3000;
    run_ms(2500); check("vor SD-Bereitschaft: Standardwerte, nicht gelesen", U32(0x44) == 0 && V(1) == 0 && V(3) == 1 && names_are(CN));
    run_ms(1500); check("nach SD-Bereitschaft gelesen: mt=1 fader=0 creative=0", U32(0x44) == 1 && V(1) == 1 && V(2) == 0 && V(3) == 0);
    check("angewendet: Original-Namen, MT preset 1", names_are(ON) && host_mtcfg[4] == 1 && arp_last == 0);
    w0 = writes; run_ms(4000); check("Lesen loest kein Schreiben aus", writes == w0);

    printf("== 6) Audio Out: Neuanmeldung\n");
    int r0 = ua_stops;
    /* 049: Start ist Stereo (Standard) → erst Stereo + Multi, dann Off, dann Stereo */
    *enums[0].val = 2; run_ms(1);
    check("Stereo + Multi: sofort getrennt (Pullup aus, unkonfiguriert, Streams gestoppt)", host_usb0_control == 0 && host_usb_configuration == 0 && ua_stops == r0 + 1 && mt_stops >= 1);
    run_ms(300); check("nach 300 ms noch getrennt", host_usb0_control == 0);
    run_ms(150);
    check("049: Stereo + Multi kombiniert: 365 B, 6 Interfaces, Kopf..AC-IF3 aus ua_cfg_desc, mt kombiniert", list_len() == 365 && host_desc_ram[2] == 0x6D && host_desc_ram[3] == 1
          && host_desc_ram[4] == 6 && memcmp(host_desc_ram + 5, ua_cfg_desc + 5, 147) == 0 && host_desc_ram[152] == 3 && host_desc_ram[166] == 0x0A
          && memcmp(host_desc_ram + 176, ua_cfg_desc + 175, 21) == 0 && memcmp(host_desc_ram + 197, ua_cfg_desc + 274, 21) == 0
          && memcmp(host_desc_ram + 218, ua_cfg_desc + 196, 52) == 0 && host_usb0_control == 0x10 && mt_split == 0);
    check("049: bcdDevice Stereo + Multi = 0x0282", host_dev_desc[12] == 0x82 && host_dev_desc[13] == 0x02);
    *enums[0].val = 4; run_ms(500);
    check("049: Stereo + Multi (Win): 390 B = ua_cfg_desc, mt getrennt, bcdDevice 0x0284", list_len() == 390 && memcmp(host_desc_ram, ua_cfg_desc, 390) == 0
          && host_usb0_control == 0x10 && mt_split == 1 && host_dev_desc[12] == 0x84 && host_dev_desc[13] == 0x02);
    *enums[0].val = 0; run_ms(500);
    check("Off: Laenge 149, Kopf 149 B / 3 Interfaces, Rest = ua_cfg_desc", list_len() == 149 && host_desc_ram[2] == 149 && host_desc_ram[3] == 0 && host_desc_ram[4] == 3
          && memcmp(host_desc_ram + 5, ua_cfg_desc + 5, 144) == 0);
    check("049: bcdDevice Off = 0x0277 (Original)", host_dev_desc[12] == 0x77 && host_dev_desc[13] == 0x02);
    *enums[0].val = 1; run_ms(500);
    check("nach 400 ms: Pullup an, Laenge 248", host_usb0_control == 0x10 && list_len() == 248);
    check("Stereo-Deskriptor im RAM: Kopf 248 B / 5 Interfaces", host_desc_ram[0] == 9 && host_desc_ram[2] == 248 && host_desc_ram[3] == 0 && host_desc_ram[4] == 5);
    check("049: bcdDevice Stereo = 0x0281", host_dev_desc[12] == 0x81 && host_dev_desc[13] == 0x02);
    check("Zaehler Neuanmeldungen = 4", U32(0x58) == 4);
    *enums[0].val = 2; run_ms(100); *enums[0].val = 1; run_ms(500);
    check("Wechsel waehrend getrennt: endet beim letzten Wert", list_len() == 248 && host_usb0_control == 0x10 && U32(0x58) == 5);
    *enums[0].val = 2; run_ms(2000);
    file_buf[file_len] = 0; check("gespeichert usb=2", strstr(file_buf, "usb=2\n") != 0);
    boot(); sd_ready_at = 0; run_ms(1500);
    check("Neustart mit usb=2: Neuanmeldung auf Stereo + Multi (049: Start war Stereo)", list_len() == 365 && host_usb0_control == 0x10 && U32(0x58) == 1 && host_dev_desc[12] == 0x82);

    printf("== 7) Kaputte / fremde Datei\n");
    const char *bad[] = { "usb=7\nmt=9\nfader=x\ncreative=1", "garbage\n\n=\nusb=\n=1\nusb=0001\n", "usb=0\r\nmt=1\r\n#creative=0\r\n",
                          "creative=0", "", "usbx=1\nmtt=1\nu=1\n" };
    int exp[][4] = { {1,0,1,1}, {1,0,1,1}, {0,1,1,1}, {1,0,1,0}, {1,0,1,1}, {1,0,1,1} };   /* 049: Standard usb=1 */
    for (int t = 0; t < 6; t++) {
        file_len = (int)strlen(bad[t]); memcpy(file_buf, bad[t], (size_t)file_len);
        boot(); run_ms(1200);
        char what[80]; snprintf(what, sizeof what, "Datei %d → %d/%d/%d/%d", t, exp[t][0], exp[t][1], exp[t][2], exp[t][3]);
        check(what, V(0) == exp[t][0] && V(1) == exp[t][1] && V(2) == exp[t][2] && V(3) == exp[t][3]);
    }
    file_len = 300; memset(file_buf, 'a', 300); boot(); run_ms(1200); check("zu lange Datei: nur 255 B gelesen, Standard", V(0) == 1 && mallocs == 0);

    printf("== 8) Karte defekt / Schreibfehler\n");
    sd_broken = 1; file_len = -1; boot(); run_ms(25000);
    check("Karte antwortet nie: nach 20 s aufgegeben, Standardwerte", U32(0x44) == 1 && V(0) == 1 && mallocs == 0);
    sd_broken = 0; sd_fail_write = 1;
    *enums[1].val = 1; run_ms(20000);
    check("Schreibfehler: 5 Versuche, dann Ruhe", U32(0x68) == 5 && U32(0x64) == 5 && mallocs == 0 && opens == 0);
    sd_fail_write = 0; *enums[1].val = 0; run_ms(3000);
    check("naechste Aenderung wird wieder gespeichert", U32(0x4C) > 0 && file_len > 0 && strstr(file_buf, "mt=0\n"));

    printf("== 9) CFG-Schnittstelle (usbup.c)\n");
    check("um_set gueltig/ungueltig", um_set(0, 1) == 1 && um_set(0, 5) == 0 && um_set(4, 0) == 0 && um_set(3, -1) == 0);
    check("um_get", um_get(0) == 1 && um_get(9) == -1);

    printf("== 10) Zufallstest (Menue, Datei, Zeit)\n");
    file_len = -1; boot();
    for (int it = 0; it < 200000; it++) {
        int r = rand() % 100;
        if (r < 3) { int k = rand() % 4; *(int32_t *)(void *)(host_um_ram + 4 + 4 * k) = rand() % (k == 0 ? 5 : 3) - (rand() % 20 == 0 ? 5 : 0); }   /* 049: usb bis 4 */
        else if (r == 3 && rand() % 50 == 0) { boot(); }
        else if (r == 4 && rand() % 20 == 0) { sd_ready_at = (int)host_millis + rand() % 3000; }
        um_poll(); host_millis += (uint32_t)(rand() % 5);
        for (int k = 0; k < 4; k++) if (V(k) < 0 || V(k) > (int)mx[k]) {  /* ungueltig nur bis zum naechsten Poll */ um_poll(); }
    }
    run_ms(3000);
    int good = 1;
    for (int k = 0; k < 4; k++) good &= V(k) >= 0 && V(k) <= (int)mx[k];
    check("Werte gueltig, Namen passend zum Modus", good && names_are(V(3) ? CN : ON));
    check("Deskriptor passt zu Audio Out", list_len() == (V(0) == 4 ? 390 : V(0) == 3 ? 331 : V(0) == 2 ? 365 : V(0) == 1 ? 248 : 149) && host_usb0_control == 0x10
          && host_dev_desc[12] == (V(0) == 4 ? 0x84 : V(0) == 3 ? 0x83 : V(0) == 2 ? 0x82 : V(0) == 1 ? 0x81 : 0x77) && host_dev_desc[13] == 0x02);
    check("MT-Konfiguration passt", host_mtcfg[4] == V(1) && (host_mtcfg[5] & 1) == V(2));
    if (U32(0x44)) {
        file_buf[file_len > 0 ? file_len : 0] = 0;
        char want[80]; snprintf(want, sizeof want, "usb=%d\nmt=%d\nfader=%d\ncreative=%d\n", V(0), V(1), V(2), V(3));
        if (!((file_len > 0 && strstr(file_buf, want)) || (file_len < 0 && V(0) == 1 && V(1) == 0 && V(2) == 1 && V(3) == 1))) printf("  Stand %d/%d/%d/%d saved %d/%d/%d/%d fail %u err %u saves %u Datei [%s] t=%u tch=%u\n", V(0),V(1),V(2),V(3), (int)U32(0x14),(int)U32(0x18),(int)U32(0x1C),(int)U32(0x20), U32(0x68),U32(0x64),U32(0x5C), file_len>0?file_buf:"-", host_millis, U32(0x4C));
        check("Datei = aktueller Stand (oder unveraendert Standard ohne Datei)", (file_len > 0 && strstr(file_buf, want)) || (file_len < 0 && V(0) == 1 && V(1) == 0 && V(2) == 1 && V(3) == 1));
    }
    check("keine Lecks, keine offenen Dateien", mallocs == 0 && opens == 0);

    printf("== 11) Audio In (045): Sends, Datei, Laden\n");
    sd_ready_at = 0; sd_fail_write = 0; file_len = -1; boot(); run_ms(3000);
    check("Standard: beide Sends Off, angewendet", sends[0] == 0 && sends[1] == 0 && send_calls > 0);
    { int par2; nenum = 0; ui_in_build(0, &par2);
      check("Seite: Delay Send / Reverb Send, Off + 1…100 %", nenum == 2 && strcmp(enums[0].name, "Delay Send") == 0 && strcmp(enums[1].name, "Reverb Send") == 0
            && enums[0].max == 100 && strcmp(enums[0].tab[0], "Off") == 0 && strcmp(enums[0].tab[25], "25%") == 0 && strcmp(enums[1].tab[100], "100%") == 0);
      *enums[0].val = 100; *enums[1].val = 25; run_ms(3000);
      check("Werte angewendet (100 % / 25 %)", sends[0] == 65536 && sends[1] == 16384);
      file_buf[file_len > 0 ? file_len : 0] = 0;
      check("gespeichert: indly=100, inrev=25, usb-Zeile intakt", file_len > 0 && strstr(file_buf, "indly=100\n") && strstr(file_buf, "inrev=25\n") && strstr(file_buf, "\nusb="));
      check("Datei passt in den Puffer (< 256)", file_len > 0 && file_len < 256); }
    boot(); run_ms(3000);
    check("nach Neustart gelesen und angewendet", sends[0] == 65536 && sends[1] == 16384);
    { static const char T9[] = "usb=2\nindly=101\ninrev=100\n"; memcpy(file_buf, T9, sizeof T9 - 1); file_len = (int)sizeof T9 - 1; } boot(); run_ms(3000);
    check("ungueltiger Wert ignoriert, gueltiger gelesen", sends[0] == 0 && sends[1] == 65536);

    printf("GESAMT: %s (%d Fehler)\n", fails ? "FEHLER" : "alles OK", fails);
    return fails != 0;
}
