/* PC-Test fuer usbmenu.c (Patch 017): Menueaufbau, Datei lesen/schreiben, Anwenden, Neuanmeldung, Zufallstest.
 * gcc -O1 -g -DHOST_TEST -fsanitize=address,undefined -o um_test usbmenu.c um_test.c && ./um_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint32_t store[2]; uint32_t manager; uint32_t invoker; } stdfn_t;
void um_early_init(void);
void um_poll(void);
void um_build(void *menu, void *parent);
void *um_menu_hook(void *item, void *parent, const char *name, uint32_t fn, uint32_t adj);
int um_get(uint32_t k);
int um_set(uint32_t k, int32_t v);

uint8_t host_um_ram[0x100];
uint8_t host_names_ram[32 * 16];
uint8_t host_desc_ram[357];
uint8_t host_mtcfg[0x40];
uint8_t host_flash_list[0x6C];
volatile uint8_t host_usb0_control = 0x10, host_usb_configuration = 1;
uint32_t host_millis = 0;

/* Deskriptor "Stereo + Multi" (Platzhalter mit echtem Kopf; Inhalt egal, verglichen wird byteweise) */
uint8_t ua_cfg_desc[357];
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
    memcpy(host_desc_ram, ua_cfg_desc, 357);                     /* ua_init */
}
static int names_are(const char *const *t) {
    for (int i = 0; i < 32; i++) if (strncmp((char *)host_names_ram + 16 * i, t[i], 16) != 0 || host_names_ram[16 * i + 15] != 0) return 0;
    return 1;
}
static int list_len(void) { return host_um_ram[0x70 + 12 + 8] | host_um_ram[0x70 + 12 + 9] << 8; }

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    srand(argc > 1 ? (unsigned)atoi(argv[1]) : 7u);
    for (int i = 0; i < 357; i++) ua_cfg_desc[i] = (uint8_t)(i * 7 + 3);
    ua_cfg_desc[0] = 9; ua_cfg_desc[1] = 2; ua_cfg_desc[2] = 357 & 0xFF; ua_cfg_desc[3] = 357 >> 8; ua_cfg_desc[4] = 6;
    static char cn[32][20];
    for (int i = 0; i < 32; i++) { snprintf(cn[i], sizeof cn[i], "FX Name %02d", i); FX_NAMES[i] = cn[i]; CN[i] = cn[i]; ON[i] = host_orig_names[i]; }
    /* Flash-Liste wie im gepatchten Image: Eintrag 1 = {0x0200, 0, 0x1FFFD4C0, 357} */
    memset(host_flash_list, 0, sizeof host_flash_list);
    host_flash_list[12 + 1] = 0x02; uint32_t a = 0x1FFFD4C0u; memcpy(host_flash_list + 12 + 4, &a, 4);
    host_flash_list[12 + 8] = 357 & 0xFF; host_flash_list[12 + 9] = 357 >> 8;

    printf("== 1) Frueh-Init\n");
    boot();
    check("Magic, Standardwerte 2/0/1/1", U32(0) == 0x55AEC017u && V(0) == 2 && V(1) == 0 && V(2) == 1 && V(3) == 1);
    check("creative-Wert liegt bei +0x10 (fx.c/arp.c lesen 0x1FFFD310)", V(3) == 1 && host_um_ram[0x10] == 1);
    check("Deskriptorliste kopiert, Laenge 357", memcmp(host_um_ram + 0x70, host_flash_list, 0x6C) == 0 && list_len() == 357);
    check("Effektnamen = FX-Suite (Creative), nullterminiert", names_are(CN));

    printf("== 2) Menue\n");
    int parent;
    um_menu_hook((void *)0x1234, &parent, "Settings", 0x36141u, 0);
    check("Hauptmenue: erst \"USB Audio\", dann \"Settings\" unveraendert", nmenu == 2 && strcmp(menus[0].name, "USB Audio") == 0 && menus[0].adj == 0
          && menus[0].parent == &parent && strcmp(menus[1].name, "Settings") == 0 && menus[1].fn == 0x36141u && menus[1].parent == &parent);
    check("USB-Audio-Builder ist um_build", menus[0].fn == (uint32_t)(uintptr_t)um_build);
    int sub;
    um_build((void *)0x99, &sub);
    check("5 Eintraege (026: + Rec Mono/Stereo)", nenum == 5);
    const char *nm[4] = { "Audio Out", "Multitrack 9/10", "Track Level", "FX Mode" };
    uint32_t mx[4] = { 3, 1, 1, 1 };                       /* 020: Audio Out + "Stereo + In" */
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
    check("gespeichert: usb=2 mt=1 fader=0 creative=0", file_len > 0 && strstr(file_buf, "\nusb=2\nmt=1\nfader=0\ncreative=0\n"));
    int w0 = writes; run_ms(5000); check("kein weiteres Schreiben ohne Aenderung", writes == w0 && mallocs == 0 && opens == 0);

    printf("== 5) Neustart mit Datei, SD erst nach 3 s bereit\n");
    boot(); sd_ready_at = 3000;
    run_ms(2500); check("vor SD-Bereitschaft: Standardwerte, nicht gelesen", U32(0x44) == 0 && V(1) == 0 && V(3) == 1 && names_are(CN));
    run_ms(1500); check("nach SD-Bereitschaft gelesen: mt=1 fader=0 creative=0", U32(0x44) == 1 && V(1) == 1 && V(2) == 0 && V(3) == 0);
    check("angewendet: Original-Namen, MT preset 1", names_are(ON) && host_mtcfg[4] == 1 && arp_last == 0);
    w0 = writes; run_ms(4000); check("Lesen loest kein Schreiben aus", writes == w0);

    printf("== 6) Audio Out: Neuanmeldung\n");
    int r0 = ua_stops;
    *enums[0].val = 1; run_ms(1);
    check("Stereo: sofort getrennt (Pullup aus, unkonfiguriert, Streams gestoppt)", host_usb0_control == 0 && host_usb_configuration == 0 && ua_stops == r0 + 1 && mt_stops >= 1);
    run_ms(300); check("nach 300 ms noch getrennt", host_usb0_control == 0);
    run_ms(150);
    check("nach 400 ms: Pullup an, Laenge 240", host_usb0_control == 0x10 && list_len() == 240);
    check("Stereo-Deskriptor im RAM: Kopf 240 B / 5 Interfaces", host_desc_ram[0] == 9 && host_desc_ram[2] == 240 && host_desc_ram[3] == 0 && host_desc_ram[4] == 5);
    *enums[0].val = 0; run_ms(500);
    check("Off: Laenge 149, Kopf 149 B / 3 Interfaces, Rest = ua_cfg_desc", list_len() == 149 && host_desc_ram[2] == 149 && host_desc_ram[3] == 0 && host_desc_ram[4] == 3
          && memcmp(host_desc_ram + 5, ua_cfg_desc + 5, 144) == 0);
    *enums[0].val = 2; run_ms(500);
    check("Stereo + Multi: 357 B, identisch mit ua_cfg_desc", list_len() == 357 && memcmp(host_desc_ram, ua_cfg_desc, 357) == 0 && host_usb0_control == 0x10);
    check("Zaehler Neuanmeldungen = 3", U32(0x58) == 3);
    *enums[0].val = 1; run_ms(100); *enums[0].val = 2; run_ms(500);
    check("Wechsel waehrend getrennt: endet beim letzten Wert", list_len() == 357 && host_usb0_control == 0x10 && U32(0x58) == 4);
    *enums[0].val = 1; run_ms(2000);
    file_buf[file_len] = 0; check("gespeichert usb=1", strstr(file_buf, "usb=1\n") != 0);
    boot(); sd_ready_at = 0; run_ms(1500);
    check("Neustart mit usb=1: Neuanmeldung auf Stereo", list_len() == 240 && host_usb0_control == 0x10 && U32(0x58) == 1);

    printf("== 7) Kaputte / fremde Datei\n");
    const char *bad[] = { "usb=7\nmt=9\nfader=x\ncreative=1", "garbage\n\n=\nusb=\n=1\nusb=0001\n", "usb=0\r\nmt=1\r\n#creative=0\r\n",
                          "creative=0", "", "usbx=1\nmtt=1\nu=1\n" };
    int exp[][4] = { {2,0,1,1}, {2,0,1,1}, {0,1,1,1}, {2,0,1,0}, {2,0,1,1}, {2,0,1,1} };
    for (int t = 0; t < 6; t++) {
        file_len = (int)strlen(bad[t]); memcpy(file_buf, bad[t], (size_t)file_len);
        boot(); run_ms(1200);
        char what[80]; snprintf(what, sizeof what, "Datei %d → %d/%d/%d/%d", t, exp[t][0], exp[t][1], exp[t][2], exp[t][3]);
        check(what, V(0) == exp[t][0] && V(1) == exp[t][1] && V(2) == exp[t][2] && V(3) == exp[t][3]);
    }
    file_len = 300; memset(file_buf, 'a', 300); boot(); run_ms(1200); check("zu lange Datei: nur 255 B gelesen, Standard", V(0) == 2 && mallocs == 0);

    printf("== 8) Karte defekt / Schreibfehler\n");
    sd_broken = 1; file_len = -1; boot(); run_ms(25000);
    check("Karte antwortet nie: nach 20 s aufgegeben, Standardwerte", U32(0x44) == 1 && V(0) == 2 && mallocs == 0);
    sd_broken = 0; sd_fail_write = 1;
    *enums[1].val = 1; run_ms(20000);
    check("Schreibfehler: 5 Versuche, dann Ruhe", U32(0x68) == 5 && U32(0x64) == 5 && mallocs == 0 && opens == 0);
    sd_fail_write = 0; *enums[1].val = 0; run_ms(3000);
    check("naechste Aenderung wird wieder gespeichert", U32(0x4C) > 0 && file_len > 0 && strstr(file_buf, "mt=0\n"));

    printf("== 9) CFG-Schnittstelle (usbup.c)\n");
    check("um_set gueltig/ungueltig", um_set(0, 1) == 1 && um_set(0, 4) == 0 && um_set(4, 0) == 0 && um_set(3, -1) == 0);
    check("um_get", um_get(0) == 1 && um_get(9) == -1);

    printf("== 10) Zufallstest (Menue, Datei, Zeit)\n");
    file_len = -1; boot();
    for (int it = 0; it < 200000; it++) {
        int r = rand() % 100;
        if (r < 3) { int k = rand() % 4; *(int32_t *)(void *)(host_um_ram + 4 + 4 * k) = rand() % 3 - (rand() % 20 == 0 ? 5 : 0); }
        else if (r == 3 && rand() % 50 == 0) { boot(); }
        else if (r == 4 && rand() % 20 == 0) { sd_ready_at = (int)host_millis + rand() % 3000; }
        um_poll(); host_millis += (uint32_t)(rand() % 5);
        for (int k = 0; k < 4; k++) if (V(k) < 0 || V(k) > (int)mx[k]) {  /* ungueltig nur bis zum naechsten Poll */ um_poll(); }
    }
    run_ms(3000);
    int good = 1;
    for (int k = 0; k < 4; k++) good &= V(k) >= 0 && V(k) <= (int)mx[k];
    check("Werte gueltig, Namen passend zum Modus", good && names_are(V(3) ? CN : ON));
    check("Deskriptor passt zu Audio Out", list_len() == (V(0) == 2 ? 357 : V(0) == 1 ? 240 : 149) && host_usb0_control == 0x10);
    check("MT-Konfiguration passt", host_mtcfg[4] == V(1) && (host_mtcfg[5] & 1) == V(2));
    if (U32(0x44)) {
        file_buf[file_len > 0 ? file_len : 0] = 0;
        char want[80]; snprintf(want, sizeof want, "usb=%d\nmt=%d\nfader=%d\ncreative=%d\n", V(0), V(1), V(2), V(3));
        if (!((file_len > 0 && strstr(file_buf, want)) || (file_len < 0 && V(0) == 2 && V(1) == 0 && V(2) == 1 && V(3) == 1))) printf("  Stand %d/%d/%d/%d saved %d/%d/%d/%d fail %u err %u saves %u Datei [%s] t=%u tch=%u\n", V(0),V(1),V(2),V(3), (int)U32(0x14),(int)U32(0x18),(int)U32(0x1C),(int)U32(0x20), U32(0x68),U32(0x64),U32(0x5C), file_len>0?file_buf:"-", host_millis, U32(0x4C));
        check("Datei = aktueller Stand (oder unveraendert Standard ohne Datei)", (file_len > 0 && strstr(file_buf, want)) || (file_len < 0 && V(0) == 2 && V(1) == 0 && V(2) == 1 && V(3) == 1));
    }
    check("keine Lecks, keine offenen Dateien", mallocs == 0 && opens == 0);

    printf("GESAMT: %s (%d Fehler)\n", fails ? "FEHLER" : "alles OK", fails);
    return fails != 0;
}
