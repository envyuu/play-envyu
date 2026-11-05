/* Patch 011 – wie 010 + Diagnosebefehl DIAG fuer den FLASH-Hänger
 * Patch 010 – USB-Upload ueber CDC-Serial (/dev/ttyACM0), Polyend Play Rev A, Firmware 1.6.0
 *
 * Protokoll: Textzeilen mit '\n', Antworten beginnen mit "OK" oder "ERR".
 *   PING                         → OK USBUP 1
 *   LS <ordner>                  → je Eintrag "F <groesse> <name>" bzw. "D <name>", dann "OK <anzahl>"
 *   PUT <pfad> <groesse> <crc32> → "OK READY", danach genau <groesse> Rohbytes;
 *                                  Datei wird geschrieben, zurueckgelesen, CRC geprueft → "OK <crc>" (sonst "ERR …", Datei geloescht)
 *   CRC <pfad>                   → OK <groesse> <crc>
 *   DEL <pfad>                   → OK
 *   FLASH <name>                 → OK FLASHING, ~0,2 s spaeter Update wie im Menue (Board_rebootToBootloader)
 *   GET <pfad>                   → "OK GET", danach Bloecke [u32 Laenge LE][Daten], Block mit Laenge 0 = Ende,
 *                                  dann "OK <groesse> <crc>" (Patch 017; bei Fehler mittendrin Laenge 0xFFFFFFFF + "ERR …")
 *   CFG                          → OK <usb> <mt> <fader> <creative>   (Menue "USB Audio", Patch 017)
 *   CFG <usb|mt|fader|creative> <wert> → OK CFG (wie eine Auswahl im Menue, wird gespeichert)
 * Die Original-Firmware /Firmware/PolyendPlay_v1.6.0.ppf ist gegen PUT/DEL geschuetzt.
 *
 * Firmware-Funktionen: Teensy usb_rx 0x1296C (CDC-RX = EP 2), usb_tx 0x12B14 (CDC-TX = EP 3), usb_malloc 0x11F20,
 * usb_free 0x11F60, usb_tx_packet_count 0x129AC; FatFs f_open 0xFB38, f_read 0xFEB4, f_write 0x10088, f_close 0x104F4,
 * f_unlink 0x1099C, f_opendir 0x107DC, f_closedir 0x10890, f_readdir 0x108FC; Heap 0x56F5C/0x56F6C;
 * Bootloader-Objekt HAL+0xD5C = 0x1FFF449C, Methode 0x85D0 (schreibt /Firmware/_fwinfo, Neustart).
 * Hook: loop() 0xBD70 "bl Hal_update" → loop_hook (laeuft im Hauptschleifen-Kontext, wie die App selbst).
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt.
 */
#include <stdint.h>

typedef struct pkt { uint16_t len, index; struct pkt *next; uint8_t buf[64]; } pkt_t;   /* Teensy usb_packet_t */

#ifdef HOST_TEST
extern pkt_t *host_usb_rx(uint32_t ep);
extern void host_usb_tx(uint32_t ep, pkt_t *p);
extern pkt_t *host_usb_malloc(void);
extern void host_usb_free(pkt_t *p);
extern uint32_t host_usb_txcount(uint32_t ep);
extern uint8_t host_usb_configured;
extern uint32_t host_millis;
extern void host_hal_update(void *h);
extern void *host_malloc(uint32_t n);
extern void host_free(void *p);
extern int host_f_open(void *fp, const char *path, uint8_t mode);
extern int host_f_read(void *fp, void *buf, uint32_t n, uint32_t *br);
extern int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw);
extern int host_f_close(void *fp);
extern int host_f_unlink(const char *path);
extern int host_f_opendir(void *dp, const char *path);
extern int host_f_closedir(void *dp);
extern int host_f_readdir(void *dp, void *fno);
extern void host_reboot(void *obj, const char *name);
extern uint8_t host_up_ram[];
#define USB_RX(ep)        host_usb_rx(ep)
#define USB_TX(ep, p)     host_usb_tx((ep), (p))
#define USB_MALLOC()      host_usb_malloc()
#define USB_FREE(p)       host_usb_free(p)
#define USB_TXCOUNT(ep)   host_usb_txcount(ep)
#define USB_CONFIGURED()  (host_usb_configured)
#define MILLIS()          (host_millis)
#define HAL_UPDATE(h)     host_hal_update(h)
#define MALLOC(n)         host_malloc(n)
#define FREE(p)           host_free(p)
#define F_OPEN            host_f_open
#define F_READ            host_f_read
#define F_WRITE           host_f_write
#define F_CLOSE           host_f_close
#define F_UNLINK          host_f_unlink
#define F_OPENDIR         host_f_opendir
#define F_CLOSEDIR        host_f_closedir
#define F_READDIR         host_f_readdir
#define REBOOT(name)      host_reboot((void *)0, (name))
extern uint32_t host_boot_obj[3];
extern void host_fs_open(void *out, void *fs, const char *path, int mode, char buffered);
extern int host_fs_write(void *fs, const void *buf, uint32_t n, void *file);
extern void host_fs_close(void *fs, void *file);
extern void host_pinmode(uint32_t pin, uint32_t mode);
extern void host_digitalwrite(uint32_t pin, uint32_t val);
#define BOOT_OBJ          ((volatile uint32_t *)host_boot_obj)
#define FS_OPEN           host_fs_open
#define FS_WRITE          host_fs_write
#define FS_CLOSE          host_fs_close
#define PINMODE           host_pinmode
#define DIGITALWRITE      host_digitalwrite
#define UP_BASE           ((uintptr_t)host_up_ram)
#else
#define USB_RX(ep)        (((pkt_t *(*)(uint32_t))0x0001296Du)(ep))
#define USB_TX(ep, p)     (((void (*)(uint32_t, pkt_t *))0x00012B15u)((ep), (p)))
#define USB_MALLOC()      (((pkt_t *(*)(void))0x00011F21u)())
#define USB_FREE(p)       (((void (*)(pkt_t *))0x00011F61u)(p))
#define USB_TXCOUNT(ep)   (((uint32_t (*)(uint32_t))0x000129ADu)(ep))
#define USB_CONFIGURED()  (*(volatile uint8_t *)0x20000D67u)
#define MILLIS()          (*(volatile uint32_t *)0x20000D7Cu)
#define HAL_UPDATE(h)     (((void (*)(void *))0x0000371Du)(h))
#define MALLOC(n)         (((void *(*)(uint32_t))0x00056F5Du)(n))
#define FREE(p)           (((void (*)(void *))0x00056F6Du)(p))
#define F_OPEN            ((int (*)(void *, const char *, uint8_t))0x0000FB39u)
#define F_READ            ((int (*)(void *, void *, uint32_t, uint32_t *))0x0000FEB5u)
#define F_WRITE           ((int (*)(void *, const void *, uint32_t, uint32_t *))0x00010089u)
#define F_CLOSE           ((int (*)(void *))0x000104F5u)
#define F_UNLINK          ((int (*)(const char *))0x0001099Du)
#define F_OPENDIR         ((int (*)(void *, const char *))0x000107DDu)
#define F_CLOSEDIR        ((int (*)(void *))0x00010891u)
#define F_READDIR         ((int (*)(void *, void *))0x000108FDu)
#define REBOOT(name)      (((void (*)(void *, const char *))0x000085D1u)((void *)0x1FFF449Cu, (name)))
#define BOOT_OBJ          ((volatile uint32_t *)0x1FFF449Cu)   /* {vptr, fs*, pin (u8)} */
#define FS_OPEN           ((void (*)(void *, void *, const char *, int, char))0x000077E1u)   /* (out{file*,ok}, fs, pfad, schreiben, gepuffert) */
#define FS_WRITE          ((int (*)(void *, const void *, uint32_t, void *))0x00007EE5u)
#define FS_CLOSE          ((void (*)(void *, void *))0x00008115u)
#define PINMODE           ((void (*)(uint32_t, uint32_t))0x00013931u)
#define DIGITALWRITE      ((void (*)(uint32_t, uint32_t))0x000138C1u)
#define UP_BASE           0x1FFFB000u            /* SRAM_L, hinter dem FX-Zustand (bis 0x1FFFAF68) */
#endif

#define EP_RX       2u
#define EP_TX       3u
#define FA_READ     0x01u
#define FA_WRITE_NEW 0x0Au                       /* FA_WRITE | FA_CREATE_ALWAYS, wie die Firmware */
#define FIL_SIZE    0x250u                       /* wie die Firmware (0x2E2C) */
#define DIR_SIZE    0x50u                        /* wie die Firmware (0x2C10) */
#define FNO_SIZE    0x120u                       /* FILINFO: fsize 8, fdate, ftime, fattrib +0xC, altname, fname +0x1A[256] */
#define BUF_SIZE    4096u
#define TIMEOUT_MS  3000u
#define FLASH_DELAY_MS 200u
#define MAXLINE     160u
#define MAXPATH     100u
#define MAGIC       0x55B10AD1u

enum { S_IDLE, S_RECV, S_VERIFY, S_CRC, S_LS, S_FLASH, S_GET };

typedef struct {
    uint32_t magic, state;
    uint32_t linelen, overflow;
    uint32_t size, done, crc_expect, crc, buflen, count, bufpos;
    uint32_t last_ms;
    void    *fil, *dir, *fno;
    uint8_t *buf;
    char     line[MAXLINE];
    char     path[MAXPATH];
} up_t;
#define UP ((up_t *)UP_BASE)
_Static_assert(sizeof(up_t) <= 0x400, "up_t ragt in die Namenspuffer (0x1FFFB400, Patch 017)");

static const char ORIGINAL[] = "/Firmware/PolyendPlay_v1.6.0.ppf";

#ifndef NO_UM
int  um_get(uint32_t k);                   /* usbmenu.c (Patch 017) */
int  um_set(uint32_t k, int32_t v);
void um_poll(void);
void lr_poll(void);   /* lrec.c */
#endif
void rec_poll(void) __attribute__((weak));   /* Patch 024 (usbrec.c) */
int  rec_request(int on) __attribute__((weak));
void rec_status(uint32_t *st, uint32_t *ds, uint32_t *drops, uint32_t *num, int32_t *err) __attribute__((weak));

/* ---------- kleine Helfer (keine libc) ---------- */
#ifndef HOST_TEST
/* Falls GCC trotz -fno-builtin einen memset-Aufruf erzeugt (Struktur-Initialisierung) */
void *memset(void *d, int c, unsigned n) { volatile uint8_t *p = (volatile uint8_t *)d; while (n--) *p++ = (uint8_t)c; return d; }
#endif
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int ends(const char *s, const char *e) {
    uint32_t a = slen(s), b = slen(e);
    return a >= b && seq(s + a - b, e);
}

typedef struct { char s[MAXLINE]; uint32_t n; } out_t;
static void o_str(out_t *o, const char *s) { while (*s && o->n < MAXLINE - 2u) o->s[o->n++] = *s++; }
static void o_dec(out_t *o, uint32_t v) {
    char t[11]; int k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) { if (o->n < MAXLINE - 2u) o->s[o->n++] = t[--k]; else k--; }
}
static void o_hex(out_t *o, uint32_t v) {
    for (int k = 28; k >= 0; k -= 4) if (o->n < MAXLINE - 2u) o->s[o->n++] = "0123456789abcdef"[(v >> k) & 15u];
}

/* Zeile senden (in Paketen < 64 Byte, damit der Host nie auf ein kurzes Paket warten muss). Ohne Platz → verwerfen. */
static void send_line(out_t *o) {
    o->s[o->n++] = '\n';
    for (uint32_t off = 0; off < o->n; ) {
        if (USB_TXCOUNT(EP_TX) >= 6u) return;
        pkt_t *p = USB_MALLOC();
        if (!p) return;
        uint32_t k = o->n - off; if (k > 63u) k = 63u;
        for (uint32_t i = 0; i < k; i++) p->buf[i] = (uint8_t)o->s[off + i];
        p->len = (uint16_t)k; p->index = 0;
        USB_TX(EP_TX, p);
        off += k;
    }
}
static void reply(const char *a, const char *b) { out_t o; o.n = 0; o_str(&o, a); if (b) { o_str(&o, " "); o_str(&o, b); } send_line(&o); }

/* CRC-32 (zlib), Halbbyte-Tabelle */
static const uint32_t CRC_T[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };
static uint32_t crc_upd(uint32_t c, const uint8_t *d, uint32_t n) {
    while (n--) { c ^= *d++; c = (c >> 4) ^ CRC_T[c & 15u]; c = (c >> 4) ^ CRC_T[c & 15u]; }
    return c;
}

/* ---------- Ressourcen ---------- */
static void release(void) {
    if (UP->fil) { F_CLOSE(UP->fil); FREE(UP->fil); UP->fil = 0; }
    if (UP->dir) { F_CLOSEDIR(UP->dir); FREE(UP->dir); UP->dir = 0; }
    if (UP->fno) { FREE(UP->fno); UP->fno = 0; }
    if (UP->buf) { FREE(UP->buf); UP->buf = 0; }
    UP->state = S_IDLE;
}
static void fail(const char *why) {
    int partial = (UP->state == S_RECV || UP->state == S_VERIFY);
    release();
    if (partial) F_UNLINK(UP->path);                 /* keine halbe/kaputte Datei liegen lassen */
    if (why) reply("ERR", why);
}
static int open_file(const char *path, uint8_t mode) {
    UP->fil = MALLOC(FIL_SIZE);
    UP->buf = (uint8_t *)MALLOC(BUF_SIZE);
    if (!UP->fil || !UP->buf) { release(); return -1; }
    for (uint32_t i = 0; i < FIL_SIZE; i++) ((uint8_t *)UP->fil)[i] = 0;
    if (F_OPEN(UP->fil, path, mode) != 0) { FREE(UP->fil); UP->fil = 0; release(); return -2; }
    return 0;
}

/* ---------- Befehle ---------- */
static int valid_path(const char *p) {
    uint32_t n = slen(p);
    if (n < 2u || n >= MAXPATH || p[0] != '/') return 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = p[i];
        if ((uint8_t)c < 0x20u || c == '\\' || c == ':' || c == '*' || c == '?') return 0;
        if (c == '.' && p[i + 1] == '.') return 0;
    }
    return 1;
}
static int parse_u32(const char *s, uint32_t base, uint32_t *v) {
    uint32_t r = 0, k = 0;
    for (; *s; s++, k++) {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
        else if (base == 16u && *s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
        else if (base == 16u && *s >= 'A' && *s <= 'F') d = (uint32_t)(*s - 'A' + 10);
        else return 0;
        if (k >= (base == 16u ? 8u : 9u)) return 0;
        r = r * base + d;
    }
    *v = r; return k > 0;
}
static void copy_path(const char *p) { uint32_t i = 0; while (p[i] && i < MAXPATH - 1u) { UP->path[i] = p[i]; i++; } UP->path[i] = 0; }


/* ---------- Diagnose: Board_rebootToBootloader (0x85D0) Schritt fuer Schritt nachstellen ---------- */
static void wait_tx(uint32_t ms) {                        /* bis die Antworten draussen sind (USB laeuft im Interrupt) */
    uint32_t t = MILLIS();
    while (USB_TXCOUNT(EP_TX) != 0u && MILLIS() - t < ms) { }
}
static void diag_say(const char *a, uint32_t v1, uint32_t v2) {
    out_t o; o.n = 0; o_str(&o, "D "); o_str(&o, a); o_str(&o, " "); o_hex(&o, v1); o_str(&o, " "); o_hex(&o, v2);
    send_line(&o); wait_tx(300);
}
static void diag(const char *name, uint32_t level) {
    volatile uint32_t *b = BOOT_OBJ;
    diag_say("OBJ vptr/fs", b[0], b[1]);
    diag_say("OBJ pin", b[2] & 0xFFu, b[2]);
    if (level >= 1u) {
        struct { void *file; uint8_t ok; uint8_t _r[3]; } h; h.file = 0; h.ok = 0;
        diag_say("OPEN start", 0, 0);
        FS_OPEN(&h, (void *)b[1], "/Firmware/_fwinfo", 1, 0);
        diag_say("OPEN ok/file", h.ok, (uint32_t)(uintptr_t)h.file);
        if (h.ok) {
            int w = FS_WRITE((void *)b[1], name, slen(name), h.file);
            diag_say("WRITE", (uint32_t)w, slen(name));
            FS_CLOSE((void *)b[1], h.file);
            diag_say("CLOSE", 0, 0);
        }
        /* zuruecklesen mit eigenen FatFs-Aufrufen */
        if (open_file("/Firmware/_fwinfo", FA_READ) == 0) {
            uint32_t br = 0;
            F_READ(UP->fil, UP->buf, 63u, &br);
            out_t o; o.n = 0; o_str(&o, "D FWINFO "); o_dec(&o, br); o_str(&o, " ");
            for (uint32_t i = 0; i < br; i++) { char c = (char)UP->buf[i]; o.s[o.n] = 0; if (o.n < MAXLINE - 2u) o.s[o.n++] = (c >= 0x20 && c < 0x7F) ? c : '?'; }
            release(); send_line(&o); wait_tx(300);
        } else diag_say("FWINFO fehlt", 0, 0);
    }
    if (level >= 2u) {
        uint32_t pin = b[2] & 0xFFu;
        diag_say("PIN low", pin, 0);
        PINMODE(pin, 1u); DIGITALWRITE(pin, 0u);
        for (uint32_t k = 1; k <= 20u; k++) {               /* 10 s: reagiert der Bootloader-Chip? */
            uint32_t t = MILLIS(); while (MILLIS() - t < 500u) { }
            diag_say("WAIT", k, MILLIS());
        }
        DIGITALWRITE(pin, 1u); PINMODE(pin, 0u);
        diag_say("PIN frei", pin, 0);
    }
    if (level == 1u) { F_UNLINK("/Firmware/_fwinfo"); diag_say("FWINFO geloescht", 0, 0); }
    reply("OK", "DIAG");
}

static void handle_line(char *l) {
    char *arg[4]; int na = 0;
    arg[0] = arg[1] = arg[2] = arg[3] = 0;
    for (char *s = l; *s && na < 4; ) {                 /* in Woerter zerlegen; 025: "..." fuer Pfade mit Leerzeichen */
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        if (*s == '"') {
            *s++ = 0;
            arg[na++] = s;
            while (*s && *s != '"') s++;
            if (*s) *s++ = 0;
            continue;
        }
        arg[na++] = s;
        while (*s && *s != ' ') s++;
    }
    if (na == 0) return;
    if (UP->state != S_IDLE) { reply("ERR", "BUSY"); return; }

    if (seq(arg[0], "PING")) { reply("OK", "USBUP 1"); return; }

    if (seq(arg[0], "PUT") && na == 4) {
        uint32_t size, crc;
        if (!valid_path(arg[1]) || !parse_u32(arg[2], 10u, &size) || !parse_u32(arg[3], 16u, &crc)) { reply("ERR", "ARGS"); return; }
        if (seq(arg[1], ORIGINAL) || ends(arg[1], "/_fwinfo")) { reply("ERR", "PROTECTED"); return; }
        if (open_file(arg[1], FA_WRITE_NEW) != 0) { reply("ERR", "OPEN"); return; }
        copy_path(arg[1]);
        UP->size = size; UP->done = 0; UP->crc_expect = crc; UP->crc = 0xFFFFFFFFu; UP->buflen = 0;
        UP->last_ms = MILLIS(); UP->state = S_RECV;
        reply("OK", "READY");
        if (size == 0u) { UP->done = 0; }                 /* leere Datei: sofort pruefen (in poll) */
        return;
    }
    if (seq(arg[0], "CRC") && na == 2) {
        if (!valid_path(arg[1])) { reply("ERR", "ARGS"); return; }
        if (open_file(arg[1], FA_READ) != 0) { reply("ERR", "OPEN"); return; }
        copy_path(arg[1]);
        UP->done = 0; UP->crc = 0xFFFFFFFFu; UP->state = S_CRC;
        return;
    }
    if (seq(arg[0], "DEL") && na == 2) {
        if (!valid_path(arg[1])) { reply("ERR", "ARGS"); return; }
        if (seq(arg[1], ORIGINAL) || ends(arg[1], "/_fwinfo")) { reply("ERR", "PROTECTED"); return; }
        reply(F_UNLINK(arg[1]) == 0 ? "OK" : "ERR", 0);
        return;
    }
    if (seq(arg[0], "GET") && na == 2) {                 /* 017: Datei herunterladen */
        if (!valid_path(arg[1])) { reply("ERR", "ARGS"); return; }
        if (open_file(arg[1], FA_READ) != 0) { reply("ERR", "OPEN"); return; }
        copy_path(arg[1]);
        UP->done = 0; UP->crc = 0xFFFFFFFFu; UP->buflen = 0; UP->bufpos = 0; UP->count = 0;
        UP->last_ms = MILLIS(); UP->state = S_GET;
        reply("OK", "GET");
        return;
    }
    if (seq(arg[0], "LS") && na == 2) {
        if (!valid_path(arg[1]) && !seq(arg[1], "/")) { reply("ERR", "ARGS"); return; }
        UP->dir = MALLOC(DIR_SIZE); UP->fno = MALLOC(FNO_SIZE);
        if (!UP->dir || !UP->fno) { release(); reply("ERR", "MEM"); return; }
        for (uint32_t i = 0; i < DIR_SIZE; i++) ((uint8_t *)UP->dir)[i] = 0;
        if (F_OPENDIR(UP->dir, arg[1]) != 0) { FREE(UP->dir); UP->dir = 0; release(); reply("ERR", "OPEN"); return; }
        UP->count = 0; UP->state = S_LS;
        return;
    }
    if (seq(arg[0], "FLASH") && na == 2) {
        const char *n = arg[1];
        int bad = !starts(n, "PolyendPlay") || !ends(n, ".ppf") || slen(n) > 60u;
        for (const char *s = n; *s; s++) if (*s == '/' || (uint8_t)*s < 0x20u) bad = 1;
        if (bad) { reply("ERR", "NAME"); return; }
        { out_t o; o.n = 0; o_str(&o, "/Firmware/"); o_str(&o, n); o.s[o.n] = 0; copy_path(o.s); }
        if (open_file(UP->path, FA_READ) != 0) { reply("ERR", "MISSING"); return; }
        release();
        { uint32_t i = 0; while (n[i]) { UP->line[i] = n[i]; i++; } UP->line[i] = 0; }   /* Name aufheben */
        UP->last_ms = MILLIS(); UP->state = S_FLASH;
        reply("OK", "FLASHING");
        return;
    }
    if (seq(arg[0], "PEEK") && na == 2) {                /* PEEK <hex>: 4 Woerter aus dem RAM lesen */
        uint32_t a;
        if (!parse_u32(arg[1], 16u, &a) || (a & 3u) || a < 0x1FFF0000u || a > 0x2002FFF0u) { reply("ERR", "ARGS"); return; }
#ifndef HOST_TEST
        out_t o; o.n = 0; o_str(&o, "OK");
        for (int k = 0; k < 4; k++) { o_str(&o, " "); o_hex(&o, *(volatile uint32_t *)(a + 4u * (uint32_t)k)); }
        send_line(&o);
#else
        reply("OK", "PEEK");
#endif
        return;
    }
    /* 015: Multitrack-Konfiguration (mt_cfg_t bei 0x1FFFD480, siehe multitrack.c). Wirkt ab dem naechsten Audioblock.
     *   MT <preset 0|1|255> <flags 0|1>          preset 0 = A, 1 = B, 255 = eigene Tabelle; flags Bit 0 = Spur-Lautstaerke
     *   MTROUTE <10|11> <je Kanal 2 Hexziffern>  eigene Tabelle, z. B. MTROUTE 10 0001020304050607080A */
    if (seq(arg[0], "MT") && na == 3) {
        uint32_t p, f;
        if (!parse_u32(arg[1], 10u, &p) || !parse_u32(arg[2], 10u, &f) || (p > 1u && p != 255u) || f > 1u) { reply("ERR", "ARGS"); return; }
#ifndef NO_UM
        if (p <= 1u && um_set(1u, (int32_t)p) && um_set(2u, (int32_t)f)) { reply("OK", "MT"); return; }   /* 017: ueber das Menue */
#endif
#ifndef HOST_TEST
        if (*(volatile uint32_t *)0x1FFFD480u != 0x4D544346u) { reply("ERR", "NOMT"); return; }
        *(volatile uint8_t *)0x1FFFD484u = (uint8_t)p;
        *(volatile uint8_t *)0x1FFFD485u = (uint8_t)f;
#endif
        reply("OK", "MT");
        return;
    }
    if (seq(arg[0], "MTROUTE") && na == 3) {
        uint32_t n;
        if (!parse_u32(arg[1], 10u, &n) || (n != 10u && n != 11u) || slen(arg[2]) != 2u * n) { reply("ERR", "ARGS"); return; }
        uint8_t r[11];
        for (uint32_t c = 0; c < n; c++) {
            char h[3]; h[0] = arg[2][2u * c]; h[1] = arg[2][2u * c + 1u]; h[2] = 0;
            uint32_t v;
            if (!parse_u32(h, 16u, &v)) { reply("ERR", "ARGS"); return; }
            r[c] = (uint8_t)v;
        }
#ifndef HOST_TEST
        if (*(volatile uint32_t *)0x1FFFD480u != 0x4D544346u) { reply("ERR", "NOMT"); return; }
        volatile uint8_t *d = (volatile uint8_t *)(n == 10u ? 0x1FFFD488u : 0x1FFFD492u);
        for (uint32_t c = 0; c < n; c++) d[c] = r[c];
#else
        (void)r;
#endif
        reply("OK", "MTROUTE");
        return;
    }
#ifndef NO_UM
    if (seq(arg[0], "CFG") && (na == 1 || na == 3)) {
        static const char *const K[4] = { "usb", "mt", "fader", "creative" };
        if (na == 3) {
            uint32_t v; int k = -1;
            for (int i = 0; i < 4; i++) if (seq(arg[1], K[i])) k = i;
            if (k < 0 || !parse_u32(arg[2], 10u, &v) || !um_set((uint32_t)k, (int32_t)v)) { reply("ERR", "ARGS"); return; }
            reply("OK", "CFG");
            return;
        }
        out_t o; o.n = 0; o_str(&o, "OK");
        for (uint32_t i = 0; i < 4u; i++) { int v = um_get(i); o_str(&o, " "); if (v < 0) o_str(&o, "-"); else o_dec(&o, (uint32_t)v); }
        send_line(&o);
        return;
    }
#endif
    if (seq(arg[0], "REC") && na <= 2) {                  /* 024: REC [1|0] – Aufnahme USB-Eingang; Antwort OK st ds drops num err */
        if (!rec_request || !rec_status) { reply("ERR", "UNKNOWN"); return; }
        if (na == 2) {
            uint32_t v;
            if (!parse_u32(arg[1], 10u, &v) || v > 1u) { reply("ERR", "ARGS"); return; }
            if (!rec_request((int)v)) { reply("ERR", "STATE"); return; }
        }
        uint32_t st, ds, dr, num; int32_t err;
        rec_status(&st, &ds, &dr, &num, &err);
        out_t o; o.n = 0; o_str(&o, "OK");
        o_str(&o, " "); o_dec(&o, st); o_str(&o, " "); o_dec(&o, ds); o_str(&o, " "); o_dec(&o, dr);
        o_str(&o, " "); o_dec(&o, num); o_str(&o, " "); if (err < 0) { o_str(&o, "-"); o_dec(&o, (uint32_t)-err); } else o_dec(&o, (uint32_t)err);
        send_line(&o);
        return;
    }
    if (seq(arg[0], "DIAG") && na == 3) {                /* DIAG <PolyendPlay….ppf> <0|1|2> */
        uint32_t lv;
        if (!starts(arg[1], "PolyendPlay") || !ends(arg[1], ".ppf") || !parse_u32(arg[2], 10u, &lv) || lv > 2u) { reply("ERR", "ARGS"); return; }
        diag(arg[1], lv);
        return;
    }
    reply("ERR", "UNKNOWN");
}

/* ---------- Datenempfang ---------- */
static int flush_buf(void) {
    uint32_t bw = 0;
    if (UP->buflen == 0u) return 0;
    if (F_WRITE(UP->fil, UP->buf, UP->buflen, &bw) != 0 || bw != UP->buflen) return -1;
    UP->buflen = 0;
    return 0;
}
static void recv_complete(void) {
    if (flush_buf() != 0) { fail("WRITE"); return; }
    if (F_CLOSE(UP->fil) != 0) { FREE(UP->fil); UP->fil = 0; fail("CLOSE"); return; }
    if (F_OPEN(UP->fil, UP->path, FA_READ) != 0) { FREE(UP->fil); UP->fil = 0; fail("REOPEN"); return; }
    UP->crc = 0xFFFFFFFFu; UP->done = 0; UP->state = S_VERIFY;   /* zuruecklesen und pruefen */
}

static void feed(const uint8_t *d, uint32_t n) {
    while (n) {
        if (UP->state == S_RECV) {
            uint32_t k = UP->size - UP->done, room = BUF_SIZE - UP->buflen;
            if (k > n) k = n;
            if (k > room) k = room;
            for (uint32_t i = 0; i < k; i++) UP->buf[UP->buflen + i] = d[i];
            UP->buflen += k; UP->done += k; d += k; n -= k;
            if (UP->buflen == BUF_SIZE && flush_buf() != 0) { fail("WRITE"); return; }
            if (UP->done == UP->size) recv_complete();
            continue;
        }
        char c = (char)*d++; n--;
        if (c == '\r') continue;
        if (c == '\n') {
            UP->line[UP->linelen] = 0;
            if (UP->overflow) reply("ERR", "LINE"); else handle_line(UP->line);
            UP->linelen = 0; UP->overflow = 0;
            continue;
        }
        if (UP->linelen < MAXLINE - 1u) UP->line[UP->linelen++] = c; else UP->overflow = 1;
    }
}

/* ---------- GET: Puffer [u32 Laenge][Daten] paketweise senden, hoechstens GET_QUEUE Pakete in der Warteschlange
 * (der usb_malloc-Pool hat nur 30 Pakete fuer CDC, MIDI und Empfang). ---------- */
#define GET_QUEUE   8u
#define GET_CHUNK   (BUF_SIZE - 4u)
static void put_len(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static int get_drain(void) {                              /* 1 = Puffer leer */
    while (UP->bufpos < UP->buflen && USB_TXCOUNT(EP_TX) < GET_QUEUE) {
        pkt_t *p = USB_MALLOC();
        if (!p) break;
        uint32_t k = UP->buflen - UP->bufpos; if (k > 63u) k = 63u;   /* < 64: nie auf ein Nullpaket angewiesen */
        for (uint32_t i = 0; i < k; i++) p->buf[i] = UP->buf[UP->bufpos + i];
        p->len = (uint16_t)k; p->index = 0;
        USB_TX(EP_TX, p);
        UP->bufpos += k; UP->last_ms = MILLIS();
    }
    return UP->bufpos >= UP->buflen;
}
static void get_work(void) {
    if (!get_drain()) {
        if (MILLIS() - UP->last_ms > TIMEOUT_MS) release();    /* Host liest nicht mehr ab: still abbrechen */
        return;
    }
    if (UP->count) {                                       /* Endblock ist draussen → Abschlusszeile */
        out_t o; o.n = 0; o_str(&o, "OK "); o_dec(&o, UP->done); o_str(&o, " "); o_hex(&o, ~UP->crc);
        release(); send_line(&o);
        return;
    }
    uint32_t br = 0;
    if (F_READ(UP->fil, UP->buf + 4, GET_CHUNK, &br) != 0 || br > GET_CHUNK) {
        put_len(UP->buf, 0xFFFFFFFFu); UP->buflen = 4; UP->bufpos = 0;
        while (!get_drain()) { if (MILLIS() - UP->last_ms > TIMEOUT_MS) break; }
        release(); reply("ERR", "READ");
        return;
    }
    UP->crc = crc_upd(UP->crc, UP->buf + 4, br); UP->done += br;
    put_len(UP->buf, br); UP->buflen = 4u + br; UP->bufpos = 0;
    if (br == 0u) UP->count = 1;                           /* Laenge 0 = Ende */
    get_drain();
}

/* ---------- Hintergrundarbeit (Pruefen, CRC, LS, Flash, GET) ---------- */
static void work(void) {
    switch (UP->state) {
    case S_RECV:
        if (UP->done == UP->size) { recv_complete(); break; }
        if (MILLIS() - UP->last_ms > TIMEOUT_MS) fail("TIMEOUT");
        break;
    case S_VERIFY:
    case S_CRC:
        for (int k = 0; k < 4; k++) {                        /* bis 16 KB pro Durchlauf */
            uint32_t br = 0;
            if (F_READ(UP->fil, UP->buf, BUF_SIZE, &br) != 0) { fail("READ"); return; }
            UP->crc = crc_upd(UP->crc, UP->buf, br); UP->done += br;
            if (br < BUF_SIZE) {
                uint32_t crc = ~UP->crc, st = UP->state;
                out_t o; o.n = 0;
                if (st == S_VERIFY) {
                    if (UP->done != UP->size) { fail("SIZE"); return; }
                    if (crc != UP->crc_expect) { fail("CRC"); return; }
                    release();
                    o_str(&o, "OK "); o_hex(&o, crc);
                } else {
                    release();
                    o_str(&o, "OK "); o_dec(&o, UP->done); o_str(&o, " "); o_hex(&o, crc);
                }
                send_line(&o);
                return;
            }
        }
        break;
    case S_LS:
        while (USB_TXCOUNT(EP_TX) < 3u) {
            uint8_t *f = (uint8_t *)UP->fno;
            if (F_READDIR(UP->dir, f) != 0 || f[0x1A] == 0) {
                out_t o; o.n = 0; o_str(&o, "OK "); o_dec(&o, UP->count);
                release(); send_line(&o);
                return;
            }
            out_t o; o.n = 0;
            if (f[0x0C] & 0x10u) o_str(&o, "D ");
            else {
                uint32_t sz = (uint32_t)f[0] | (uint32_t)f[1] << 8 | (uint32_t)f[2] << 16 | (uint32_t)f[3] << 24;
                o_str(&o, "F "); o_dec(&o, sz); o_str(&o, " ");
            }
            o_str(&o, (const char *)(f + 0x1A));
            send_line(&o); UP->count++;
        }
        break;
    case S_GET:
        get_work();
        break;
    case S_FLASH:
        if (MILLIS() - UP->last_ms > FLASH_DELAY_MS) {
            UP->state = S_IDLE;
            REBOOT(UP->line);                                 /* kehrt nicht zurueck */
        }
        break;
    default:
        break;
    }
}

static void up_poll(void) {
    if (UP->magic != MAGIC) {
        UP->state = S_IDLE; UP->linelen = 0; UP->overflow = 0;
        UP->fil = 0; UP->dir = 0; UP->fno = 0; UP->buf = 0;
        UP->magic = MAGIC;
    }
    if (!USB_CONFIGURED()) {                                  /* Kabel ab: aufraeumen, nichts senden */
        if (UP->state != S_IDLE && UP->state != S_FLASH) fail(0);
        UP->linelen = 0; UP->overflow = 0;
        return;
    }
    /* Nur annehmen, wenn wir nicht gerade pruefen/listen (sonst staut der Host per NAK). */
    for (int k = 0; k < 64 && (UP->state == S_IDLE || UP->state == S_RECV); k++) {
        pkt_t *p = USB_RX(EP_RX);
        if (!p) break;
        if (p->len > p->index && p->len <= 64u) feed(p->buf + p->index, (uint32_t)(p->len - p->index));
        USB_FREE(p);
        if (UP->state == S_RECV) UP->last_ms = MILLIS();
    }
    work();
}

__attribute__((used, section(".text.entry_loop")))
void loop_hook(void *hal) {
    HAL_UPDATE(hal);
    up_poll();
#ifndef NO_UM
    um_poll();                                                /* 017: Menue "USB Audio" anwenden, Datei lesen/schreiben */
#ifndef NO_SL
    lr_poll();                                                /* USB recording status */
#endif
#endif
    if (rec_poll) rec_poll();                                 /* 024: USB-Eingang → Samples/USB Rec */
}

#ifdef HOST_TEST
int host_up_state(void) { return (int)UP->state; }
int host_up_busy_resources(void) { return (UP->fil != 0) + (UP->dir != 0) + (UP->fno != 0) + (UP->buf != 0); }
#endif
