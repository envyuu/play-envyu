/* Host-Test fuer usbrec.c (Patch 024): Aufnahme des USB-Eingangs als WAV auf die SD-Karte.
 * gcc -m32 -O1 -g -DHOST_TEST -fsanitize=address,undefined -o rec_test rec_test.c && ./rec_test [seed]
 */
#define HOST_TEST 1
#define MAX_SAMPLES 500000u                               /* statt 6 min (Speicher im Test) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbrec.c"

uint8_t host_rec_ram[0x2400];
uint32_t host_ui_mode, host_millis;
int host_stereo;
static int imports; static char imp_dir[64], imp_name[64];
static int assigned_slot = -1;
int host_assign(int slot) { assigned_slot = slot; return 1; }
int host_import(const char *dir, const char *name) { imports++; snprintf(imp_dir, sizeof imp_dir, "%s", dir); snprintf(imp_name, sizeof imp_name, "%s", name); return 0; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FEHLER %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- FatFs-Attrappe ---- */
#define NF 8
static struct { char path[64]; uint8_t *d; uint32_t len, cap; int used; } fs[NF];
static int fail_write_after = -1, nwrites = 0, mkdirs = 0;
typedef struct { int idx; uint32_t pos; int open; } hf_t;
static int find(const char *p) { for (int i = 0; i < NF; i++) if (fs[i].used && !strcmp(fs[i].path, p)) return i; return -1; }
int host_f_mkdir(const char *path) { (void)path; mkdirs++; return FR_EXIST; }
int host_f_open(void *fp, const char *path, uint8_t mode) {
    CHECK(mode == (FA_WRITE | FA_CREATE_NEW), "Modus CREATE_NEW|WRITE");
    if (find(path) >= 0) return FR_EXIST;
    int i = 0; while (i < NF && fs[i].used) i++;
    if (i == NF) return 7;
    fs[i].used = 1; snprintf(fs[i].path, sizeof fs[i].path, "%s", path); fs[i].len = 0; fs[i].cap = 1 << 16; fs[i].d = malloc(fs[i].cap);
    hf_t *h = (hf_t *)fp; h->idx = i; h->pos = 0; h->open = 1;
    return 0;
}
int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw) {
    hf_t *h = (hf_t *)fp;
    CHECK(h->open, "write auf offene Datei");
    if (fail_write_after == 0 || !fs[h->idx].used) { *bw = 0; return 1; }   /* Fehler bzw. Karte "gewechselt" */
    if (fail_write_after > 0) fail_write_after--;
    while (h->pos + n > fs[h->idx].cap) { fs[h->idx].cap *= 2; fs[h->idx].d = realloc(fs[h->idx].d, fs[h->idx].cap); }
    memcpy(fs[h->idx].d + h->pos, buf, n); h->pos += n; if (h->pos > fs[h->idx].len) fs[h->idx].len = h->pos;
    *bw = n; nwrites++;
    return 0;
}
int host_f_lseek(void *fp, uint64_t ofs) {
    CHECK(ofs == 0, "lseek nur auf 0"); hf_t *h = (hf_t *)fp; if (!fs[h->idx].used) return 1; h->pos = (uint32_t)ofs; return 0; }
int host_f_close(void *fp) { hf_t *h = (hf_t *)fp; CHECK(h->open, "close einmal"); h->open = 0; return 0; }
static void fs_reset(void) { for (int i = 0; i < NF; i++) { free(fs[i].d); memset(&fs[i], 0, sizeof fs[i]); } }

static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static int16_t L[128], R[128];
static uint32_t seq;                    /* naechster Frame-Zaehler des "PCs" */
static void block(void) {               /* Audio-ISR: ein Eingangsblock */
    for (int k = 0; k < 128; k++) { L[k] = (int16_t)(seq * 3u); R[k] = (int16_t)(seq * 5u + 1u); seq++; }
    rec_capture(L, R);
}
static int16_t expect_at(uint32_t f) { return (int16_t)(((int32_t)(int16_t)(f * 3u) + (int16_t)(f * 5u + 1u)) >> 1); }

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* prueft Kopf + Inhalt; first = Frame-Zaehler des ersten aufgenommenen Samples; Rueckgabe Sampleanzahl */
static uint32_t check_wav(const char *path, uint32_t first, int contiguous) {
    int i = find(path);
    CHECK(i >= 0, "Datei %s vorhanden", path);
    if (i < 0) return 0;
    const uint8_t *d = fs[i].d;
    uint32_t n = (fs[i].len - 44u) / 2u;
    CHECK(fs[i].len >= 44 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVEfmt ", 8) && !memcmp(d + 36, "data", 4), "WAV-Kopf");
    CHECK(u32(d + 4) == 36u + 2u * n && u32(d + 40) == 2u * n, "Laengen im Kopf (%u Samples)", n);
    CHECK(u32(d + 24) == 44100 && d[22] == 1 && d[34] == 16 && u32(d + 28) == 88200, "mono 16 Bit 44100");
    if (contiguous) {
        int bad = 0;
        for (uint32_t k = 0; k < n; k++) { int16_t v = (int16_t)(d[44 + 2 * k] | d[45 + 2 * k] << 8); if (v != expect_at(first + k)) bad++; }
        CHECK(bad == 0, "Inhalt = (L+R)/2 lueckenlos (%d Fehler)", bad);
    }
    return n;
}

static void boot(void) { fs_reset(); memset(host_rec_ram, 0xA5, sizeof host_rec_ram); rec_init(); host_ui_mode = 1; seq = 0; fail_write_after = -1; }

int main(int argc, char **argv) {
    if (argc > 1) rng = (uint32_t)strtoul(argv[1], 0, 0);
    uint32_t st, ds, dr, num; int32_t err;

    printf("== 1. Grundablauf\n");
    boot();
    host_ui_mode = 0;
    CHECK(rec_request(1) == 0 && REC->err == -1 && REC->state == R_IDLE, "ohne 'Stereo + In' abgelehnt");
    host_ui_mode = 1;
    CHECK(rec_request(1) == 1 && REC->state == R_START, "Start angenommen");
    CHECK(rec_request(1) == 0, "zweiter Start abgelehnt");
    block(); CHECK(REC->head == 0, "vor dem Oeffnen nichts im Ring");
    rec_poll();
    CHECK(REC->state == R_REC && find("Samples/Live Recordings/Rec_001.wav") >= 0 && mkdirs == 2, "Rec 001.wav angelegt, Ordner angelegt");
    uint32_t first = seq;
    for (int b = 0; b < 2000; b++) { block(); if (b % 3 == 2) rec_poll(); }
    CHECK(REC->drops == 0, "keine Verluste bei normalem Schreibtempo");
    rec_status(&st, &ds, &dr, &num, &err);
    CHECK(st == R_REC && ds == 2000u * 128u * 10u / 44100u && num == 1, "Status: laeuft, %u Zehntelsekunden", ds);
    CHECK(rec_request(0) == 1 && REC->state == R_STOP, "Stopp angenommen");
    block();                                                    /* nach Stopp nichts mehr */
    rec_poll();
    CHECK(REC->state == R_IDLE && REC->files == 1 && REC->err == 0, "fertig, 1 Datei");
    CHECK(imports == 1 && !strcmp(imp_dir, "Samples/Live Recordings") && !strcmp(imp_name, "Rec_001.wav") && REC->imp_slot == 0 && REC->imp_res == 0 && assigned_slot == 0 && REC->assigned == 1, "026: ins Projekt geladen und Spur zugewiesen (%s / %s)", imp_dir, imp_name);
    uint32_t n = check_wav("Samples/Live Recordings/Rec_001.wav", first, 1);
    CHECK(n == 2000u * 128u, "alle %u Samples in der Datei", n);
    CHECK(rec_request(0) == 0, "Stopp ohne Aufnahme abgelehnt");

    printf("== 2. Naechste freie Nummer\n");
    rec_request(1); rec_poll(); first = seq;
    for (int b = 0; b < 10; b++) block();
    rec_request(0); rec_poll();
    CHECK(REC->num == 2 && check_wav("Samples/Live Recordings/Rec_002.wav", first, 1) == 1280, "Rec 002.wav, 1280 Samples");
    rec_init(); REC->num = 0;                                   /* Neustart: Nummer vergessen → 001/002 belegt → 003 */
    rec_request(1); rec_poll();
    CHECK(REC->num == 3 && REC->state == R_REC, "nach Neustart Rec 003");
    rec_request(0); rec_poll();
    CHECK(check_wav("Samples/Live Recordings/Rec_003.wav", 0, 0) == 0, "leere Aufnahme: gueltige WAV mit 0 Samples");

    printf("== 3. Langsame Karte → Verluste gezaehlt\n");
    boot();
    rec_request(1); rec_poll();
    uint32_t captured = 0;
    for (int b = 0; b < 3000; b++) { block(); captured += 128; if (b % 60 == 59) rec_poll(); }   /* nur alle 174 ms */
    rec_request(0); rec_poll();
    n = check_wav("Samples/Live Recordings/Rec_001.wav", 0, 0);
    CHECK(REC->drops > 0 && n + REC->drops == captured, "verloren %u + geschrieben %u = %u", REC->drops, n, captured);

    printf("== 4. Variante wechselt / Schreibfehler / Startfehler\n");
    boot();
    rec_request(1); rec_poll(); first = seq;
    for (int b = 0; b < 100; b++) { block(); rec_poll(); }
    host_ui_mode = 0; rec_poll(); rec_poll();
    CHECK(REC->state == R_IDLE && REC->files == 1, "Variante weg → Aufnahme beendet und abgeschlossen");
    CHECK(check_wav("Samples/Live Recordings/Rec_001.wav", first, 1) == 12800, "Datei vollstaendig");
    host_ui_mode = 1;
    rec_request(1); rec_poll();
    fail_write_after = 1;
    for (int b = 0; b < 100; b++) { block(); rec_poll(); }
    CHECK(REC->state == R_IDLE && REC->err != 0, "Schreibfehler → beendet, Fehler %d", REC->err);
    fail_write_after = -1;
    for (int i = 0; i < NF; i++) { fs[i].used = 1; snprintf(fs[i].path, sizeof fs[i].path, "x%d", i); }
    rec_request(1); rec_poll();
    CHECK(REC->state == R_IDLE && REC->err == 7, "Karte voll (kein Eintrag frei) → Fehler, kein Absturz (%d)", REC->err);
    rec_request(1); rec_request(0);
    CHECK(REC->state == R_IDLE, "Start + sofort Stopp vor dem Oeffnen → nichts");

    printf("== 5. Laengengrenze (Test: 500000 Samples, Geraet: 6 min)\n");
    boot();
    rec_request(1); rec_poll();
    uint32_t blocks = MAX_SAMPLES / 128u + 20u;
    for (uint32_t b = 0; b < blocks; b++) { block(); if (b % 2) rec_poll(); }
    rec_poll(); rec_poll();
    CHECK(REC->state == R_IDLE && REC->files == 1, "automatisch beendet");
    n = check_wav("Samples/Live Recordings/Rec_001.wav", 0, 0);
    CHECK(n >= MAX_SAMPLES && n <= MAX_SAMPLES + CAP, "Laenge %u = Grenze (+ Ringinhalt)", n);

    printf("== 5b. Menuezeile USB Rec\n");
    boot();
    volatile int32_t *mv = rec_menu_value();
    CHECK(mv && *mv == 0, "Zeiger der Menuezeile, Stop");
    *mv = 1; rec_poll();
    CHECK(REC->state == R_START || REC->state == R_REC, "Rec im Menue → Start");
    rec_poll(); first = seq;
    for (int b = 0; b < 50; b++) { block(); rec_poll(); }
    *mv = 0; rec_poll(); rec_poll();
    CHECK(REC->state == R_IDLE && REC->files == 1 && *mv == 0, "Stop im Menue → Datei fertig");
    CHECK(check_wav("Samples/Live Recordings/Rec_001.wav", first, 1) == 6400, "6400 Samples");
    host_ui_mode = 0; *mv = 1; rec_poll(); rec_poll();
    CHECK(*mv == 0 && REC->state == R_IDLE, "ohne 'Stereo + In': Menue springt auf Stop zurueck");
    host_ui_mode = 1;
    for (int i = 0; i < NF; i++) { fs[i].used = 1; snprintf(fs[i].path, sizeof fs[i].path, "y%d", i); }
    *mv = 1; for (int i = 0; i < 20; i++) rec_poll();
    CHECK(*mv == 0 && REC->state == R_IDLE, "Startfehler: Menue auf Stop, keine Endlos-Wiederholung");

    printf("== 5c. Stereo (026)\n");
    boot(); host_stereo = 1;
    rec_request(1); rec_poll(); first = seq;
    CHECK(REC->ch == 2 && REC->heap == 1 && REC->cap == CAP_HEAP, "Stereo: Heap-Ring 8192 Samples");
    for (int b = 0; b < 300; b++) { block(); if (b % 3 == 2) rec_poll(); }
    rec_request(0); rec_poll();
    {   int i = find("Samples/Live Recordings/Rec_001.wav"); const uint8_t *d = fs[i].d; uint32_t nn = (fs[i].len - 44u) / 4u, bad = 0;
        CHECK(d[22] == 2 && u32(d + 28) == 176400 && d[32] == 4 && u32(d + 40) == nn * 4u, "Stereo-Kopf (%u Frames)", nn);
        for (uint32_t k = 0; k < nn; k++) { int16_t a = (int16_t)(d[44 + 4*k] | d[45 + 4*k] << 8), c = (int16_t)(d[46 + 4*k] | d[47 + 4*k] << 8);
            if (a != (int16_t)((first + k) * 3u) || c != (int16_t)((first + k) * 5u + 1u)) bad++; }
        CHECK(nn == 300u * 128u && bad == 0 && REC->drops == 0, "L/R lueckenlos (%u Fehler)", bad);
        CHECK(REC->heap == 0 && REC->cap == CAP, "Heap-Ring freigegeben"); }
    host_stereo = 0;

    printf("== 6. Zufallstest\n");
    boot();
    long inv = 0;
    for (int i = 0; i < 300000; i++) {
        uint32_t r = rnd() % 1000;
        if (r < 2) rec_request(1);
        else if (r < 4) rec_request(0);
        else if (r < 5) host_ui_mode = rnd() & 1 ? 1 : (rnd() & 1);
        else if (r < 6) { fs_reset(); }
        else if (r < 8) { volatile int32_t *m = rec_menu_value(); *m = (int32_t)(rnd() & 1); }
        else if (r < 9) host_stereo = (int)(rnd() & 1);
        else if (r < 600) block();
        else rec_poll();
        if (REC->state == R_REC && REC->head - REC->tail > REC->cap) inv++;
        if (REC->state > R_STOP) inv += 1000;
    }
    rec_request(0); rec_poll(); rec_poll();
    CHECK(inv == 0 && REC->state == R_IDLE, "Invarianten (%ld), am Ende ruhig", inv);
    fs_reset();
    printf("%s: %d Pruefungen, %d Fehler\n", fails ? "FEHLER" : "OK", checks, fails);
    return fails != 0;
}
