/* PC-Testlauf fuer usbup.c: Attrappen fuer Teensy-USB (Paket-Pool wie im Geraet) und FatFs (echte Dateien unter ./sd_test) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct pkt { uint16_t len, index; struct pkt *next; uint8_t buf[64]; } pkt_t;
void loop_hook(void *hal);
int host_up_state(void);
int host_up_busy_resources(void);

uint8_t host_up_ram[4096] __attribute__((aligned(8)));
uint8_t host_usb_configured = 1;
uint32_t host_millis = 0;
static const char *ROOT = "sd_test";

/* ---- USB: Pool von 30 Paketen, RX-Warteschlange, TX-Mitschnitt ---- */
#define POOL 30
static pkt_t pool[POOL]; static int used[POOL]; static int pool_used = 0;
static pkt_t *rxq[POOL]; static int rxn = 0;
static uint8_t *hsend; static size_t hsend_len = 0, hsend_pos = 0;   /* vom "PC" zu sendende Bytes */
static char txbuf[1 << 20]; static size_t txlen = 0;
static int txcount_fake = 0;
pkt_t *host_usb_malloc(void) {
    for (int i = 0; i < POOL; i++) if (!used[i]) { used[i] = 1; pool_used++; pool[i].len = pool[i].index = 0; pool[i].next = 0; return &pool[i]; }
    return 0;
}
void host_usb_free(pkt_t *p) {
    int i = (int)(p - pool);
    if (i < 0 || i >= POOL || !used[i]) { fprintf(stderr, "usb_free: ungueltiges Paket\n"); exit(1); }
    used[i] = 0; pool_used--;
}
pkt_t *host_usb_rx(uint32_t ep) {
    if (ep != 2) { fprintf(stderr, "usb_rx: falscher EP %u\n", ep); exit(1); }
    if (!rxn) return 0;
    pkt_t *p = rxq[0]; memmove(rxq, rxq + 1, (size_t)(rxn - 1) * sizeof rxq[0]); rxn--;
    return p;
}
void host_usb_tx(uint32_t ep, pkt_t *p) {
    if (ep != 3) { fprintf(stderr, "usb_tx: falscher EP %u\n", ep); exit(1); }
    if (p->len == 0 || p->len >= 64) { fprintf(stderr, "usb_tx: Paketlaenge %u\n", p->len); exit(1); }
    memcpy(txbuf + txlen, p->buf, p->len); txlen += p->len;
    host_usb_free(p);
}
uint32_t host_usb_txcount(uint32_t ep) { (void)ep; return (uint32_t)txcount_fake; }
/* "Interrupt": Host-Daten in Pakete packen, solange der Pool reicht (sonst NAK) */
static void usb_isr(int maxpk) {
    while (hsend_pos < hsend_len && maxpk-- > 0) {
        pkt_t *p = host_usb_malloc();
        if (!p) return;
        size_t k = hsend_len - hsend_pos; if (k > 64) k = 64;
        memcpy(p->buf, hsend + hsend_pos, k); p->len = (uint16_t)k; hsend_pos += k;
        rxq[rxn++] = p;
    }
}
static void host_write(const void *d, size_t n) {
    hsend = realloc(hsend, hsend_len + n); memcpy(hsend + hsend_len, d, n); hsend_len += n;
}
static void host_puts(const char *s) { host_write(s, strlen(s)); }

/* ---- Heap ---- */
static int allocs = 0;
void *host_malloc(uint32_t n) { allocs++; return malloc(n); }
void host_free(void *p) { if (p) allocs--; free(p); }
void host_hal_update(void *h) { (void)h; }

/* ---- FatFs ---- */
static int fail_write_after = -1;   /* Schreibfehler nach n Aufrufen simulieren */
static void mk(char *out, const char *p) { snprintf(out, 512, "%s%s", ROOT, p); }
int host_f_open(void *fp, const char *path, uint8_t mode) {
    char p[512]; mk(p, path);
    FILE *f = fopen(p, mode == 0x01 ? "rb" : mode == 0x0A ? "wb" : "?");
    if (!f) return 4;
    *(FILE **)fp = f; return 0;
}
int host_f_read(void *fp, void *buf, uint32_t n, uint32_t *br) { *br = (uint32_t)fread(buf, 1, n, *(FILE **)fp); return 0; }
int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw) {
    if (fail_write_after == 0) { *bw = 0; return 1; }
    if (fail_write_after > 0) fail_write_after--;
    *bw = (uint32_t)fwrite(buf, 1, n, *(FILE **)fp); return 0;
}
int host_f_close(void *fp) { FILE *f = *(FILE **)fp; *(FILE **)fp = 0; return f ? fclose(f) : 9; }
int host_f_unlink(const char *path) { char p[512]; mk(p, path); return unlink(p) == 0 ? 0 : 4; }
typedef struct { DIR *d; char base[512]; } hdir_t;
int host_f_opendir(void *dp, const char *path) {
    hdir_t *h = malloc(sizeof *h); mk(h->base, path);
    h->d = opendir(h->base); if (!h->d) { free(h); return 5; }
    *(hdir_t **)dp = h; return 0;
}
int host_f_closedir(void *dp) { hdir_t *h = *(hdir_t **)dp; if (h) { closedir(h->d); free(h); } *(hdir_t **)dp = 0; return 0; }
int host_f_readdir(void *dp, void *fno) {
    hdir_t *h = *(hdir_t **)dp; uint8_t *f = fno; struct dirent *e;
    memset(f, 0, 0x120);
    do e = readdir(h->d); while (e && e->d_name[0] == '.');
    if (!e) return 0;
    char p[1024]; struct stat st; snprintf(p, sizeof p, "%s/%s", h->base, e->d_name); stat(p, &st);
    uint64_t sz = (uint64_t)st.st_size; memcpy(f, &sz, 8);
    f[0x0C] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    snprintf((char *)f + 0x1A, 256, "%s", e->d_name);
    return 0;
}
static char rebooted[128] = "";
void host_reboot(void *obj, const char *name) { (void)obj; snprintf(rebooted, sizeof rebooted, "%s", name); }

/* ---- Diagnose-Attrappen ---- */
uint32_t host_boot_obj[3] = { 0x9AF38u, 0x1234u, 0x51u };
static int pin_low_count = 0;
void host_fs_open(void *out, void *fs, const char *path, int mode, char buffered) {
    (void)fs; (void)buffered;
    char p[512]; mk(p, path); FILE *f = fopen(p, mode ? "wb" : "rb");
    ((void **)out)[0] = f; ((uint8_t *)out)[sizeof(void *)] = f != 0;
}
int host_fs_write(void *fs, const void *buf, uint32_t n, void *file) { (void)fs; return (int)fwrite(buf, 1, n, file); }
void host_fs_close(void *fs, void *file) { (void)fs; fclose(file); }
void host_pinmode(uint32_t pin, uint32_t mode) { (void)pin; (void)mode; }
void host_digitalwrite(uint32_t pin, uint32_t val) { if (pin == 0x51 && !val) pin_low_count++; }
static volatile int millis_thread_on = 0;
/* ---- Hilfen ---- */
static uint32_t crc32(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *d++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u)); }
    return ~c;
}
static void run(int loops) {
    for (int i = 0; i < loops; i++) { usb_isr(4 + rand() % 12); loop_hook(0); host_millis += 1 + (uint32_t)(rand() % 4); }
}
static void run_until_idle(int max) { for (int i = 0; i < max; i++) { run(1); if (host_up_state() == 0 && hsend_pos == hsend_len) break; } run(5); }
static int fails = 0;
static void check(const char *what, int ok) { printf("Pruefung %-46s %s\n", what, ok ? "OK" : "FEHLER"); if (!ok) fails++; }
static int last_reply_is(const char *s) {
    if (txlen == 0) return 0;
    txbuf[txlen] = 0;
    char *end = txbuf + txlen - 1; if (*end != '\n') return 0;
    char *b = end; while (b > txbuf && b[-1] != '\n') b--;
    return strncmp(b, s, strlen(s)) == 0;
}
static void clear_tx(void) { txlen = 0; }
static int file_exists(const char *p) { char q[512]; mk(q, p); return access(q, F_OK) == 0; }
static int file_equals(const char *p, const uint8_t *d, size_t n) {
    char q[512]; mk(q, p); FILE *f = fopen(q, "rb"); if (!f) return 0;
    uint8_t *b = malloc(n + 1); size_t r = fread(b, 1, n + 1, f); fclose(f);
    int ok = r == n && memcmp(b, d, n) == 0; free(b); return ok;
}
static void put(const char *path, const uint8_t *d, size_t n, uint32_t crc, size_t send_n) {
    char cmd[256]; snprintf(cmd, sizeof cmd, "PUT %s %zu %08x\n", path, n, crc);
    host_puts(cmd); run(10);
    host_write(d, send_n);
}

#ifndef SIM
int main(void) {
    srand(1);
    system("rm -rf sd_test && mkdir -p sd_test/Firmware sd_test/Samples && echo original > sd_test/Firmware/PolyendPlay_v1.6.0.ppf");

    printf("== 1) PING, unbekannt, ueberlange Zeile\n");
    host_puts("PING\r\n"); run(20); check("PING → OK USBUP 1", last_reply_is("OK USBUP 1"));
    clear_tx(); host_puts("FOO bar\n"); run(20); check("unbekannter Befehl → ERR UNKNOWN", last_reply_is("ERR UNKNOWN"));
    clear_tx(); { char l[400]; memset(l, 'x', 399); l[399] = 0; host_puts(l); host_puts("\n"); } run(30);
    check("ueberlange Zeile → ERR LINE", last_reply_is("ERR LINE"));
    clear_tx(); host_puts("PING\n"); run(20); check("danach wieder normal", last_reply_is("OK USBUP 1"));

    printf("== 2) PUT 4 MB (wie eine .ppf)\n");
    size_t N = 4172609; uint8_t *data = malloc(N); for (size_t i = 0; i < N; i++) data[i] = (uint8_t)rand();
    uint32_t c = crc32(data, N);
    clear_tx(); put("/Firmware/PolyendPlay_test.ppf", data, N, c, N);
    uint32_t t0 = host_millis; run_until_idle(200000);
    char exp[64]; snprintf(exp, sizeof exp, "OK %08x", c);
    check("Antwort OK <crc>", last_reply_is(exp));
    check("Datei identisch", file_equals("/Firmware/PolyendPlay_test.ppf", data, N));
    check("keine Ressourcen/Heap offen", host_up_busy_resources() == 0 && allocs == 0);
    check("alle USB-Pakete zurueck", pool_used == 0);
    printf("   (simulierte Dauer %u ms, nur Schleifendurchlaeufe)\n", host_millis - t0);

    printf("== 3) PUT mit falscher CRC\n");
    clear_tx(); put("/Firmware/PolyendPlay_bad.ppf", data, 100000, 0x12345678u, 100000); run_until_idle(100000);
    check("ERR CRC", last_reply_is("ERR CRC"));
    check("Datei geloescht", !file_exists("/Firmware/PolyendPlay_bad.ppf"));

    printf("== 4) PUT abgebrochen (zu wenig Daten) → Timeout\n");
    clear_tx(); put("/Firmware/PolyendPlay_half.ppf", data, 100000, crc32(data, 100000), 50000); run(3000);
    check("ERR TIMEOUT", last_reply_is("ERR TIMEOUT"));
    check("Datei geloescht", !file_exists("/Firmware/PolyendPlay_half.ppf"));
    check("keine Ressourcen offen", host_up_busy_resources() == 0 && allocs == 0);

    printf("== 5) Schutz und Pfadpruefung\n");
    clear_tx(); host_puts("PUT /Firmware/PolyendPlay_v1.6.0.ppf 5 00000000\n"); run(20); check("Original ueberschreiben → PROTECTED", last_reply_is("ERR PROTECTED"));
    clear_tx(); host_puts("DEL /Firmware/PolyendPlay_v1.6.0.ppf\n"); run(20); check("Original loeschen → PROTECTED", last_reply_is("ERR PROTECTED"));
    check("Original unveraendert", file_equals("/Firmware/PolyendPlay_v1.6.0.ppf", (const uint8_t *)"original\n", 9));
    clear_tx(); host_puts("PUT /Firmware/_fwinfo 5 00000000\n"); run(20); check("_fwinfo → PROTECTED", last_reply_is("ERR PROTECTED"));
    clear_tx(); host_puts("PUT /../x 5 00000000\n"); run(20); check("'..' → ERR ARGS", last_reply_is("ERR ARGS"));
    clear_tx(); host_puts("PUT rel 5 00000000\n"); run(20); check("relativer Pfad → ERR ARGS", last_reply_is("ERR ARGS"));
    clear_tx(); host_puts("PUT /a 99999999999 00000000\n"); run(20); check("Groesse zu gross → ERR ARGS", last_reply_is("ERR ARGS"));
    clear_tx(); host_puts("PUT /a 5 xyz\n"); run(20); check("CRC kein Hex → ERR ARGS", last_reply_is("ERR ARGS"));
    clear_tx(); host_puts("PUT /Gibtsnicht/a 5 00000000\n"); run(20); check("Ordner fehlt → ERR OPEN", last_reply_is("ERR OPEN"));

    printf("== 6) Kleine Datei, leere Datei, CRC, LS, DEL\n");
    clear_tx(); put("/Samples/kick.wav", data, 777, crc32(data, 777), 777); run_until_idle(10000);
    snprintf(exp, sizeof exp, "OK %08x", crc32(data, 777)); check("777 Byte → OK", last_reply_is(exp));
    clear_tx(); put("/Samples/leer.txt", data, 0, crc32(data, 0), 0); run_until_idle(1000);
    check("0 Byte → OK 00000000", last_reply_is("OK 00000000") && file_exists("/Samples/leer.txt"));
    clear_tx(); host_puts("CRC /Firmware/PolyendPlay_test.ppf\n"); run_until_idle(100000);
    snprintf(exp, sizeof exp, "OK %zu %08x", N, c); check("CRC-Befehl", last_reply_is(exp));
    clear_tx(); host_puts("CRC /nix\n"); run(20); check("CRC fehlende Datei → ERR OPEN", last_reply_is("ERR OPEN"));
    clear_tx(); host_puts("LS /Firmware\n"); run_until_idle(1000); txbuf[txlen] = 0;
    check("LS listet beide .ppf", strstr(txbuf, "F 4172609 PolyendPlay_test.ppf\n") && strstr(txbuf, "F 9 PolyendPlay_v1.6.0.ppf\n") && last_reply_is("OK 2"));
    clear_tx(); host_puts("LS /\n"); run_until_idle(1000); txbuf[txlen] = 0;
    check("LS / zeigt Ordner", strstr(txbuf, "D Firmware\n") && strstr(txbuf, "D Samples\n"));
    clear_tx(); host_puts("DEL /Samples/kick.wav\n"); run(20); check("DEL → OK", last_reply_is("OK") && !file_exists("/Samples/kick.wav"));
    clear_tx(); host_puts("DEL /Samples/kick.wav\n"); run(20); check("DEL nochmal → ERR", last_reply_is("ERR"));

    printf("== 7) BUSY waehrend der Pruefung\n");
    clear_tx(); put("/Firmware/PolyendPlay_b.ppf", data, N, c, N); host_puts("PING\n"); run_until_idle(200000);
    txbuf[txlen] = 0;
    snprintf(exp, sizeof exp, "OK %08x", c);
    check("Upload OK, PING waehrend Pruefung → ERR BUSY", strstr(txbuf, exp) && (strstr(txbuf, "ERR BUSY\n") || strstr(txbuf, "OK USBUP 1\n")));

    printf("== 8) Schreibfehler (Karte voll)\n");
    clear_tx(); fail_write_after = 3; put("/Firmware/PolyendPlay_full.ppf", data, 100000, crc32(data, 100000), 100000); run_until_idle(100000);
    fail_write_after = -1;
    /* Rest der gesendeten Daten landet im Zeilenparser → mehrere ERR LINE/UNKNOWN moeglich; entscheidend: ERR WRITE kam und Datei weg */
    txbuf[txlen] = 0;
    check("ERR WRITE", strstr(txbuf, "ERR WRITE\n") != 0);
    check("Datei geloescht", !file_exists("/Firmware/PolyendPlay_full.ppf"));
    clear_tx(); host_puts("\nPING\n"); run(30); check("danach wieder bereit", last_reply_is("OK USBUP 1"));

    printf("== 9) Kabel ab waehrend Upload\n");
    clear_tx(); put("/Firmware/PolyendPlay_k.ppf", data, 200000, crc32(data, 200000), 100000); run(5);
    host_usb_configured = 0; run(5); hsend_pos = hsend_len; rxn = 0; memset(used, 0, sizeof used); pool_used = 0; host_usb_configured = 1;
    check("Datei geloescht, nichts offen", !file_exists("/Firmware/PolyendPlay_k.ppf") && host_up_busy_resources() == 0 && allocs == 0);
    clear_tx(); host_puts("PING\n"); run(30); check("danach wieder bereit", last_reply_is("OK USBUP 1"));

    printf("== 10) FLASH\n");
    clear_tx(); host_puts("FLASH ../x.ppf\n"); run(20); check("FLASH mit / → ERR NAME", last_reply_is("ERR NAME"));
    clear_tx(); host_puts("FLASH Other.ppf\n"); run(20); check("FLASH falscher Name → ERR NAME", last_reply_is("ERR NAME"));
    clear_tx(); host_puts("FLASH PolyendPlay_fehlt.ppf\n"); run(20); check("FLASH fehlende Datei → ERR MISSING", last_reply_is("ERR MISSING") && !rebooted[0]);
    clear_tx(); host_puts("FLASH PolyendPlay_test.ppf\n"); run(20);
    check("OK FLASHING, noch kein Neustart", last_reply_is("OK FLASHING") && !rebooted[0]);
    run(150); check("Neustart mit reinem Dateinamen", strcmp(rebooted, "PolyendPlay_test.ppf") == 0);
    check("keine Ressourcen offen", host_up_busy_resources() == 0 && allocs == 0);

    printf("== 10b) DIAG\n");
    clear_tx(); host_puts("DIAG PolyendPlay_test.ppf 1\n"); run(20); txbuf[txlen] = 0;
    check("DIAG 1: Objekt, Open, Write, FWINFO-Inhalt", strstr(txbuf, "D OBJ vptr/fs 0009af38 00001234") && strstr(txbuf, "D OPEN ok/file 00000001")
          && strstr(txbuf, "D FWINFO 20 PolyendPlay_test.ppf") && last_reply_is("OK DIAG"));
    check("DIAG 1: _fwinfo wieder geloescht, kein Pin", !file_exists("/Firmware/_fwinfo") && pin_low_count == 0 && allocs == 0);
    clear_tx(); host_puts("DIAG Other.ppf 1\n"); run(20); check("DIAG falscher Name → ERR ARGS", last_reply_is("ERR ARGS"));
    clear_tx(); host_puts("DIAG PolyendPlay_test.ppf 3\n"); run(20); check("DIAG Stufe 3 → ERR ARGS", last_reply_is("ERR ARGS"));
    printf("== 11) TX-Warteschlange voll (Host liest nicht)\n");
    txcount_fake = 6; clear_tx(); host_puts("PING\n"); run(20); check("Antwort verworfen statt Stau", txlen == 0 && pool_used == 0);
    txcount_fake = 0;

    printf("== 11b) GET (Patch 017)\n");
    {
        size_t sizes[] = { 0, 1, 63, 64, 4091, 4092, 4093, 100000 };
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            size_t n = sizes[si];
            char q[512]; mk(q, "/Samples/g.bin"); FILE *f = fopen(q, "wb"); fwrite(data, 1, n, f); fclose(f);
            clear_tx(); host_puts("GET /Samples/g.bin\n"); run_until_idle(200000);
            /* Antwort zerlegen: "OK GET\n" [u32 len][daten]... len 0, "OK <n> <crc>\n" */
            int ok = txlen >= 7 && memcmp(txbuf, "OK GET\n", 7) == 0;
            size_t pos = 7, got = 0; uint8_t *rx = malloc(n + 1); int frames = 0;
            while (ok) {
                if (pos + 4 > txlen) { ok = 0; break; }
                uint32_t L = (uint32_t)(uint8_t)txbuf[pos] | (uint32_t)(uint8_t)txbuf[pos+1] << 8 | (uint32_t)(uint8_t)txbuf[pos+2] << 16 | (uint32_t)(uint8_t)txbuf[pos+3] << 24;
                pos += 4;
                if (L == 0) break;
                if (L > 4092 || pos + L > txlen || got + L > n) { ok = 0; break; }
                memcpy(rx + got, txbuf + pos, L); got += L; pos += L; frames++;
            }
            char want[64]; snprintf(want, sizeof want, "OK %zu %08x\n", n, crc32(data, n));
            ok = ok && got == n && memcmp(rx, data, n) == 0 && txlen - pos == strlen(want) && memcmp(txbuf + pos, want, strlen(want)) == 0;
            char what[64]; snprintf(what, sizeof what, "GET %zu Byte (%d Bloecke) bitgleich", n, frames);
            check(what, ok);
            free(rx);
        }
        check("GET: nichts offen, Pakete zurueck", host_up_busy_resources() == 0 && allocs == 0 && pool_used == 0);
        clear_tx(); host_puts("GET /Samples/fehlt.bin\n"); run(20); check("GET fehlende Datei → ERR OPEN", last_reply_is("ERR OPEN"));
        clear_tx(); host_puts("GET ../x\n"); run(20); check("GET ungueltiger Pfad → ERR ARGS", last_reply_is("ERR ARGS"));
        /* Host liest nicht ab: Warteschlange voll → nach Zeitlimit still abbrechen, danach wieder bedienbar */
        txcount_fake = 8; clear_tx(); host_puts("GET /Samples/g.bin\n"); run(3000);
        check("GET ohne Abnehmer: Zeitlimit, Ruhezustand", host_up_state() == 0 && host_up_busy_resources() == 0 && allocs == 0);
        txcount_fake = 0; clear_tx(); host_puts("PING\n"); run(20); check("danach PING → OK", last_reply_is("OK USBUP 1"));
        /* Kabel waehrend GET ab */
        clear_tx(); host_puts("GET /Samples/g.bin\n"); run(3); host_usb_configured = 0; run(3); host_usb_configured = 1; run(5);
        check("GET + Kabel ab: aufgeraeumt", host_up_state() == 0 && host_up_busy_resources() == 0 && allocs == 0);
    }

    printf("== 12) Zufallstest (Befehle, Muell, Abbrueche)\n");
    const char *cmds[] = { "PING\n", "LS /\n", "LS /Firmware\n", "CRC /Firmware/PolyendPlay_test.ppf\n", "DEL /Samples/z.bin\n",
                           "PUT /Samples/z.bin 3000 00000000\n", "FLASH PolyendPlay_nix.ppf\n", "\n\n", "PUT /Samples/z.bin 5000 ", "LS\n", "GET /Samples/r.bin\n" };
    for (int it = 0; it < 4000; it++) {
        int r = rand() % 15;
        if (r == 14) host_puts(cmds[10]);
        else if (r < 10) host_puts(cmds[r]);
        else if (r < 12) { uint8_t junk[300]; int n = rand() % 300; for (int i = 0; i < n; i++) junk[i] = (uint8_t)rand(); host_write(junk, (size_t)n); }
        else if (r == 12) { host_usb_configured = 0; run(2); hsend_pos = hsend_len; rxn = 0; memset(used, 0, sizeof used); pool_used = 0; host_usb_configured = 1; }
        else { char cmd[128]; size_t n = (size_t)(rand() % 20000); snprintf(cmd, sizeof cmd, "PUT /Samples/r.bin %zu %08x\n", n, crc32(data, n)); host_puts(cmd); run(5); host_write(data, n); }
        run(1 + rand() % 50);
        if (txlen > (1u << 19)) txlen = 0;
    }
    host_puts("\n"); run_until_idle(400000); run(4000);
    check("Zufallstest: Ende im Ruhezustand", host_up_state() == 0);
    check("Zufallstest: kein Heap-Leck, nichts offen", host_up_busy_resources() == 0 && allocs == 0);
    check("Zufallstest: alle USB-Pakete zurueck", pool_used == 0 && rxn == 0);
    check("Zufallstest: Original unveraendert", file_equals("/Firmware/PolyendPlay_v1.6.0.ppf", (const uint8_t *)"original\n", 9));

    free(data); free(hsend);
    printf("GESAMT: %s (%d Fehler)\n", fails ? "FEHLER" : "alles OK", fails);
    return fails != 0;
}
#else
/* Simulator: usbup.c hinter einem Pseudo-Terminal, fuer End-zu-End-Tests mit play_upload.py */
#include <fcntl.h>
#include <termios.h>
int main(void) {
    system("rm -rf sd_test && mkdir -p sd_test/Firmware sd_test/Samples && echo original > sd_test/Firmware/PolyendPlay_v1.6.0.ppf");
    int m = posix_openpt(O_RDWR | O_NOCTTY); grantpt(m); unlockpt(m);
    struct termios t; tcgetattr(m, &t); cfmakeraw(&t); tcsetattr(m, TCSANOW, &t);
    fcntl(m, F_SETFL, O_NONBLOCK);
    printf("%s\n", ptsname(m)); fflush(stdout);
    for (;;) {
        uint8_t b[4096]; ssize_t n = read(m, b, sizeof b);
        if (n > 0) host_write(b, (size_t)n);
        usb_isr(16); loop_hook(0); host_millis += 1;
        if (txlen) { ssize_t w = write(m, txbuf, txlen); (void)w; txlen = 0; }
        if (rebooted[0]) { printf("REBOOT %s\n", rebooted); fflush(stdout); rebooted[0] = 0; }
        if (n <= 0) usleep(200);
    }
}
#endif

