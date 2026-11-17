/* Patch 024 – USB-Eingang (Patch 020, "Stereo + In") als Sample auf die SD-Karte aufnehmen
 * Polyend Play Rev A, Firmware 1.6.0 – ENTWURF, nur am PC getestet.
 *
 *  - Audio-ISR: ui_pre (usbin.c) ruft rec_capture mit dem 128-Frame-Block des USB-Eingangs. Waehrend der Aufnahme
 *    wird (L+R)/2 als Mono-Sample in einen Ring (4096 Samples = 93 ms) geschrieben – das Play spielt Samples ohnehin mono.
 *  - Hauptschleife (loop_hook → rec_poll): Ring in Stuecken von 2048 Samples per FatFs nach
 *    "Samples/USB_Rec/Rec_NNN.wav" schreiben (naechste freie Nummer, FA_CREATE_NEW), beim Stopp WAV-Kopf mit den
 *    endgueltigen Laengen nachtragen (f_lseek 0). Hoechstens 6 min (Sample-Grenze des Play).
 *  - Steuerung: rec_request(1/0) – USB-Befehl "REC 1/0" (usbup.c); spaeter Shift + Taste.
 *  - Nur in der Variante "Stereo + In": Der Ring liegt im Multitrack-Ring (0x1FFFDE00), der dort unbenutzt ist.
 *    Wechselt die Variante, wird die Aufnahme beendet.
 *
 * RAM: FIL 0x1FFFD630 (0x250) · Ring 0x1FFFDE00 (8 KB) – im Multitrack-Bereich, nur waehrend der Aufnahme benutzt.
 *      rec_t 0x1FFF9A80 (0x80) · rx_t 0x1FFF9A08 (0x30) – seit 12.11. AUSSERHALB: lagen vorher im Sendepuffer
 *      MT_TX0 (0x1FFFD630–0x1FFFDA0E); "Stereo + Multi" hat sie ueberschrieben → REC-Pille blieb stehen, keine Aufnahme mehr.
 */
#include <stdint.h>

#define REC_MAGIC    0x52454324u
#define CAP          4096u                              /* statischer Ring (int16-Samples) */
#define CAP_HEAP     8192u                              /* 026 Stereo: Ring aus dem Heap (16 KB = 93 ms), sonst statisch (46 ms) */
#define CHUNK        2048u
#ifndef MAX_SAMPLES
#define MAX_SAMPLES  (44100u * 360u)                    /* 6 min (Host-Test setzt weniger) */
#endif
#define FIL_SIZE     0x250u
#define FA_WRITE     0x02u
#define FA_CREATE_NEW 0x04u
#define FR_EXIST     8
#define UI_MAGIC     0x55A0D120u

enum { R_IDLE = 0, R_START = 1, R_REC = 2, R_STOP = 3 };

typedef struct {               /* per PEEK 1FFF9A80 lesbar */
    uint32_t magic;            /* 00 */
    uint32_t state;            /* 04 R_* */
    uint32_t head;             /* 08 Samples vom Audio-ISR */
    uint32_t tail;             /* 0C Samples in die Datei geschrieben */
    uint32_t drops;            /* 10 verlorene Samples (Ring voll) */
    uint32_t num;              /* 14 Nummer der aktuellen/letzten Datei */
    int32_t  err;              /* 18 letzter FatFs-Fehler (0 = ok), -1 = falsche Variante, -2 = keine Nummer frei */
    uint32_t t_start;          /* 1C millis */
    uint32_t files;            /* 20 fertige Dateien */
    int32_t  peak;             /* 24 Spitzenpegel der Aufnahme */
    uint32_t stops;            /* 28 */
    int32_t  menu;             /* 2C Menuezeile "USB Rec" (0 Stop / 1 Rec), EnumMenuItem schreibt hierher */
    char     path[48];         /* 30 */
    int32_t  menu_seen;        /* 60 zuletzt gesehener Menuewert */
    int32_t  imp_slot;         /* 64 026: Pool-Platz der letzten Aufnahme (-1 = nicht importiert) */
    int32_t  imp_res;          /* 68 026: Ergebnis FUN_0001dcf4 (0 ok, 1 Speicher voll, 2 > 255 Samples, -1 nicht moeglich) */
    uint32_t assigned;         /* 6C 026: 1 = der gewaehlten Spur zugewiesen */
    uint32_t ch;               /* 70 026: Kanaele der laufenden Aufnahme (1/2) */
    uint32_t ring;             /* 74 026: Ringadresse (statisch oder Heap) */
    uint32_t cap;              /* 78 026: Ringgroesse in Samples (Zweierpotenz) */
    uint32_t heap;             /* 7C 026: 1 = Ring aus dem Heap (beim Ende freigeben) */
} rec_t;
_Static_assert(sizeof(rec_t) == 0x80, "rec_t");

/* 045 (11.11.): Warten auf Signal / Pause bei Stille. Seit 12.11. bei 0x1FFF9A08 (frei zwischen GR_PLAYING und rs_t).
 * Waehrend gewartet bzw. pausiert wird, schreibt der Audio-ISR weiter in den Ring, behaelt aber nur die letzten PRE
 * Frames (Vorlauf, damit der Anfang des ersten Tons nicht fehlt) und schiebt tail selbst nach; der Schreiber in der
 * Hauptschleife ruht dann. skip zaehlt die so verworfenen Samples: Laenge der Datei = tail − skip. */
typedef struct {
    uint32_t waiting;          /* 00 1 = noch kein Signal seit dem Start */
    uint32_t pend;             /* 04 1 = Stille erkannt, Schreiber leert den Ring, neue Bloecke werden verworfen */
    uint32_t paused;           /* 08 1 = pausiert (wie waiting) */
    uint32_t skip;             /* 0C verworfene Samples (Ring) */
    uint32_t quiet;            /* 10 Frames Stille in Folge */
    uint32_t w_on, p_on;       /* 14 Einstellungen beim Start */
    uint32_t pauses;           /* 1C Zaehler */
} rx_t;
_Static_assert(sizeof(rx_t) <= 0x30, "rx_t");
#define THR       104                                   /* −50 dBFS (32768 · 10^(−50/20)) */
#define PRE       512u                                  /* Vorlauf in Frames (11,6 ms); PRE · 2 < CHUNK: der Schreiber greift nicht ein */
#define HOLD      44100u                                /* Pause erst nach 1 s Stille (Hall, Pausen in der Musik) */
_Static_assert(PRE * 2u < CHUNK, "Vorlauf < CHUNK");

#ifdef HOST_TEST
extern uint8_t host_rec_ram[0x2400];          /* FIL + rec_t + Ring */
extern uint32_t host_ui_mode;
extern uint32_t host_millis;
extern int host_f_open(void *fp, const char *path, uint8_t mode);
extern int host_f_write(void *fp, const void *buf, uint32_t n, uint32_t *bw);
extern int host_f_lseek(void *fp, uint64_t ofs);
extern int host_f_close(void *fp);
extern int host_f_mkdir(const char *path);
#define FIL_RAM   ((void *)host_rec_ram)
#define REC       ((volatile rec_t *)(void *)(host_rec_ram + 0x250))
#define RX        ((volatile rx_t *)(void *)(host_rec_ram + 0x2D0))
extern int host_rec_wait, host_rec_pause;
#define REC_WAIT()  (host_rec_wait)
#define REC_PAUSE() (host_rec_pause)
#define RING      ((int16_t *)(void *)(host_rec_ram + 0x300))
#define UI_ON()   (host_ui_mode != 0)
#define MILLIS()  (host_millis)
#define F_OPEN    host_f_open
#define F_WRITE   host_f_write
#define F_LSEEK   host_f_lseek
#define F_CLOSE   host_f_close
#define F_MKDIR   host_f_mkdir
extern int host_stereo;
#define STEREO()  (host_stereo)
#define ALLOC_RING(n) malloc(n)
#define FREE_RING(p)  free(p)
#define BARRIER() __asm__ volatile("" ::: "memory")
#else
#define FIL_RAM   ((void *)0x1FFFD630u)
#define REC       ((volatile rec_t *)0x1FFF9A80u)      /* 12.11.: bis 0x1FFF9AFF, davor rz_t (nis.c) bis 0x1FFF9A7F, dahinter wv_t 0x1FFF9B00 */
#define RX        ((volatile rx_t *)0x1FFF9A08u)       /* 12.11.: bis 0x1FFF9A37, davor GRP/GR_PLAYING 0x1FFF9A00/04, dahinter rs_t 0x1FFF9A40 */
_Static_assert(0x1FFF9A08u + sizeof(rx_t) <= 0x1FFF9A40u && 0x1FFF9A80u + sizeof(rec_t) <= 0x1FFF9B00u, "rec_t/rx_t ausserhalb des Multitrack-Bereichs 0x1FFFD400–0x1FFFFF00");
int um_rec_wait(void) __attribute__((weak));            /* usbmenu.c (045) */
int um_rec_pause(void) __attribute__((weak));
#define REC_WAIT()  (!um_rec_wait || um_rec_wait())
#define REC_PAUSE() (!um_rec_pause || um_rec_pause())
#define RING      ((int16_t *)0x1FFFDE00u)
#define UI_ON()   (*(volatile uint32_t *)0x1FFF9000u == UI_MAGIC && *(volatile uint32_t *)0x1FFF9004u != 0u)
#define MILLIS()  (*(volatile uint32_t *)0x20000D7Cu)
#define F_OPEN    ((int (*)(void *, const char *, uint8_t))0x0000FB39u)
#define F_WRITE   ((int (*)(void *, const void *, uint32_t, uint32_t *))0x00010089u)
#define F_LSEEK   ((int (*)(void *, uint64_t))0x00010571u)   /* 025: FSIZE_t ist 64 Bit (exFAT-Build): ofs in r2:r3! */
#define F_CLOSE   ((int (*)(void *))0x000104F5u)
#define F_MKDIR   ((int (*)(const char *))0x00010A79u)
int um_rec_stereo(void) __attribute__((weak));          /* usbmenu.c (026): Einstellung "Rec" */
#define STEREO()  (um_rec_stereo && um_rec_stereo())
#define ALLOC_RING(n) (((void *(*)(uint32_t))0x00056F5Du)(n))    /* Heap_alloc (Hauptschleife) */
#define FREE_RING(p)  (((void (*)(void *))0x00056F6Du)(p))
#define BARRIER() __asm__ volatile("dmb" ::: "memory")
#endif

static const char DIR[] = "Samples/Live Recordings";  /* 026: Wunsch Nutzer (USB-Befehle koennen seit 025 "…") */
static const char POOL_FOLDER[] = "Live Recordings";   /* Ordner im Sample-Pool (max. 16 Zeichen) */

void rec_init(void) {
    volatile uint32_t *z = (volatile uint32_t *)(void *)REC;
    for (uint32_t i = 0; i < sizeof(rec_t) / 4u; i++) z[i] = 0;
    REC->ch = 1u; REC->ring = (uint32_t)(uintptr_t)RING; REC->cap = CAP;
    volatile uint32_t *x = (volatile uint32_t *)(void *)RX;
    for (uint32_t i = 0; i < sizeof(rx_t) / 4u; i++) x[i] = 0;
    BARRIER();
    REC->magic = REC_MAGIC;
}

/* Audio-ISR (aus ui_pre, nur bei gueltigem Eingangsblock) */
void rec_capture(const int16_t *l, const int16_t *r) {
    volatile rec_t *s = REC;
    if (s->magic != REC_MAGIC || s->state != R_REC) return;
    volatile rx_t *x = RX;
    uint32_t ch = s->ch;
    int32_t blk = 0;                                        /* 045: Spitze dieses Blocks (Signal/Stille) */
    for (uint32_t k = 0; k < 128u; k++) {
        int32_t a = l[k] < 0 ? -l[k] : l[k], b = r[k] < 0 ? -r[k] : r[k];
        if (a > blk) blk = a;
        if (b > blk) blk = b;
    }
    int loud = blk >= THR;
    if (x->pend) {                                          /* Ring wird geleert: Stille verwerfen, bis tail == head */
        if (loud) { x->pend = 0; x->quiet = 0; }            /* Signal kam zurueck, bevor geleert war: weiter aufnehmen */
        else if (s->tail == s->head) { x->pend = 0; x->paused = 1; x->pauses++; }
        else return;
    }
    uint32_t head = s->head, used = head - s->tail, cap = s->cap, mask = cap - 1u;
    int16_t *ring = (int16_t *)(uintptr_t)s->ring;
    int32_t pk = s->peak & 0xFFFF, bp = (int32_t)((uint32_t)s->peak >> 16);   /* 045: unten Spitze seit Start, oben seit dem letzten rec_level() */
    uint32_t k = 0, n = 0;
    for (; k < 128u && used + ch <= cap; k++) {
        if (ch == 2u) {
            ring[(head + n) & mask] = l[k]; ring[(head + n + 1u) & mask] = r[k];
            n += 2u; used += 2u;
            int32_t a = l[k] < 0 ? -l[k] : l[k], b = r[k] < 0 ? -r[k] : r[k];
            if (a > pk) pk = a;
            if (b > pk) pk = b;
            if (a > bp) bp = a;
            if (b > bp) bp = b;
        } else {
            int32_t m = ((int32_t)l[k] + r[k]) >> 1;
            ring[(head + n) & mask] = (int16_t)m;
            n++; used++;
            if (m < 0) m = -m;
            if (m > pk) pk = m;
            if (m > bp) bp = m;
        }
    }
    if (pk > 0xFFFF) pk = 0xFFFF;
    if (bp > 0xFFFF) bp = 0xFFFF;
    s->peak = (int32_t)((uint32_t)pk | ((uint32_t)bp << 16));
    if (k < 128u) s->drops += (128u - k) * ch;
    BARRIER();
    s->head = head + n;
    if (x->waiting || x->paused) {                          /* 045: nur den Vorlauf behalten */
        if (loud) { x->waiting = 0; x->paused = 0; x->quiet = 0; return; }
        uint32_t t = s->tail, keep = PRE * ch, h = head + n;
        if (h - t > keep) { uint32_t d = h - keep - t; x->skip += d; BARRIER(); s->tail = t + d; }
        return;
    }
    if (x->p_on) {
        if (loud) x->quiet = 0;
        else if ((x->quiet += 128u) >= HOLD) { x->quiet = 0; x->pend = 1; }
    }
}

/* 045: Audio-ISR ohne Eingangsblock (Stream steht, Vorpuffer, Unterlauf): Stille aufnehmen, damit die Zeit
 * weiterlaeuft – mit "Pause On Silence" wird sie wie jede Stille uebersprungen. */
static const int16_t ZERO[128];
void rec_silence(void) {
    volatile rec_t *s = REC;
    if (s->magic != REC_MAGIC || s->state != R_REC) return;
    rec_capture(ZERO, ZERO);
}

/* Steuerung: 1 = starten, 0 = stoppen. Rueckgabe 1 = angenommen. */
int rec_request(int on) {
    volatile rec_t *s = REC;
    if (s->magic != REC_MAGIC) return 0;
    if (on) {
        if (s->state != R_IDLE) return 0;
        if (!UI_ON()) { s->err = -1; s->menu = 0; return 0; }
        s->state = R_START;
        return 1;
    }
    if (s->state == R_REC) { s->state = R_STOP; return 1; }
    if (s->state == R_START) { s->state = R_IDLE; return 1; }
    return 0;
}

/* Zustand fuer Anzeige/USB: Zustand, Zehntelsekunden, verlorene Samples, Dateinummer */
/* 045: Spitze des Eingangs seit dem letzten Aufruf (0…32768), fuer den Pegel in der REC-Anzeige; setzt sie zurueck */
uint32_t rec_level(void) {
    volatile rec_t *s = REC;
    if (s->magic != REC_MAGIC) return 0;
    uint32_t v = (uint32_t)s->peak >> 16;
    s->peak = (int32_t)((uint32_t)s->peak & 0xFFFFu);
    return v;
}

void rec_status(uint32_t *st, uint32_t *ds, uint32_t *drops, uint32_t *num, int32_t *err) {
    volatile rec_t *s = REC;
    *st = s->magic == REC_MAGIC ? s->state : 0xFFu;
    uint32_t ch = s->ch ? s->ch : 1u;
    *ds = ((s->state == R_REC || s->state == R_STOP ? s->head : s->tail) - RX->skip) / ch * 10u / 44100u;   /* 045: ohne Verworfenes */
    *drops = s->drops; *num = s->num; *err = s->err;
}

static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }

void rec_wav_header(uint8_t *h, uint32_t samples, uint32_t ch) {
    static const uint8_t T[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                                   0x44,0xAC,0,0, 0x88,0x58,0x01,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    for (uint32_t i = 0; i < 44u; i++) h[i] = T[i];
    put32(h + 4, 36u + samples * 2u);
    put32(h + 40, samples * 2u);
    if (ch == 2u) { h[22] = 2; put32(h + 28, 44100u * 4u); h[32] = 4; }   /* 026: Stereo */
}

static void make_path(volatile char *p, uint32_t n) {
    uint32_t i = 0;
    for (const char *d = DIR; *d; d++) p[i++] = *d;
    const char *f = "/Rec_";
    for (; *f; f++) p[i++] = *f;
    p[i++] = (char)('0' + n / 100u % 10u); p[i++] = (char)('0' + n / 10u % 10u); p[i++] = (char)('0' + n % 10u);
    p[i++] = '.'; p[i++] = 'w'; p[i++] = 'a'; p[i++] = 'v'; p[i] = 0;
}

/* 026: Aufnahme wie "Load Wav" der Sample-Auswahl ins Projekt laden (FUN_000433bc nachgebildet):
 * Vorschau stoppen, Audio anhalten falls der Sequencer steht, FUN_0001dcf4(ps, ordner, namen, 1, 0), Audio wieder an. */
#ifdef HOST_TEST
extern int host_import(const char *dir, const char *name);
static int pool_free_slot(void) { return 0; }
static int do_import(const char *dir, const char *name) { return host_import(dir, name); }
#else
#define PS        0x1FFF6EC8u                                  /* App + 0x2A20 (Projekt-Ablage) */
#define AENG      0x1FFF69E8u                                  /* App + 0x2540 (Audio-Engine) */
#define ENGINE    0x200025C4u                                  /* Sequencer/Engine, vtable 0xA99B4 */
static int pool_free_slot(void) {
    uint32_t list = *(volatile uint32_t *)(PS + 0x418u);
    if (list < 0x1FFF0000u || list >= 0x20030000u) return -1;
    for (uint32_t i = 0; i < 255u; i++) {
        const volatile uint32_t *e = ((const volatile uint32_t *(*)(uint32_t, uint32_t))0x0001AE01u)(list, i);
        if (e[0] == e[1]) return (int)i;
    }
    return -1;
}
/* Pool-Ordner (samplesMetadata, pb 0x9FE98): list+8 → {u32 ordner[256]; u32 anzahl @0x400; char name[20][17] @0x404}.
 * Ordner "Live Recordings" suchen bzw. anlegen; Rueckgabe Index (0, wenn nichts geht = wie "Load Wav" ohne Ordner). */
static uint32_t pool_folder(void) {
    uint32_t list = *(volatile uint32_t *)(PS + 0x418u);
    if (list < 0x1FFF0000u || list >= 0x20030000u) return 0;
    uint32_t m = *(volatile uint32_t *)(list + 8u);
    if (!((m >= 0x1FFF0000u && m < 0x20030000u) || (m >= 0x70000000u && m < 0x72000000u)) || (m & 3u)) return 0;
    volatile uint32_t *cnt = (volatile uint32_t *)(m + 0x400u);
    uint32_t n = *cnt;
    if (n > 20u) return 0;
    for (uint32_t i = 0; i < n; i++) {
        volatile const char *nm = (volatile const char *)(m + 0x404u + 17u * i);
        uint32_t k = 0; while (k < 16u && POOL_FOLDER[k] && nm[k] == POOL_FOLDER[k]) k++;
        if (!POOL_FOLDER[k] && nm[k] == 0) return i;
    }
    if (n >= 20u) return 0;
    volatile char *nm = (volatile char *)(m + 0x404u + 17u * n);
    uint32_t k = 0; for (; POOL_FOLDER[k] && k < 16u; k++) nm[k] = POOL_FOLDER[k];
    for (; k < 17u; k++) nm[k] = 0;
    *cnt = n + 1u;
    return n;
}
static int do_import(const char *dir, const char *name) {
    if (*(volatile uint32_t *)ENGINE != 0x000A99B4u) return -1;
    uint32_t folder = pool_folder();
    const char *names[1] = { name };
    ((void (*)(uint32_t))0x0003741Du)(AENG);                   /* FUN_0003741c: Vorschau aus */
    uint32_t vt = *(volatile uint32_t *)ENGINE;
    int playing = ((int (*)(uint32_t))(*(volatile uint32_t *)(vt + 0x28u)))(ENGINE);
    if (!playing) ((void (*)(uint32_t, uint32_t))0x00037425u)(AENG, 0u);   /* Audio anhalten wie die Firmware */
    int r = ((int (*)(uint32_t, const char *, const char *const *, int, uint32_t))0x0001DCF5u)(PS, dir, names, 1, folder);   /* 5. Argument = Pool-Ordner (FUN_0001d9ec: list+8 [slot] = ordner) */
    ((void (*)(uint32_t, uint32_t))0x00037425u)(AENG, 1u);
    return r;
}
#endif

/* 026: neues Sample der gewaehlten Audio-Spur geben – wie der Sample-Regler: Editor (SeqController+0x24B4 =
 * 0x1FFF6994) vt+0x1C (FUN_0003b088: Parameter i um delta aendern; ohne gehaltene Steps = Spur). Erst ganz nach unten
 * (Wertebereich klemmt, ParamRange 🔶), dann auf den Platz. Nur wenn die Spur Audio ist (Tabelle 0x1FFF7918). */
#ifdef HOST_TEST
extern int host_assign(int slot);
static int assign_track(int slot) { return host_assign(slot); }
#else
int sv_assign_sample(int slot) __attribute__((weak));
static int assign_track(int slot) {
    uint32_t ed = *(volatile uint32_t *)0x1FFF6994u, tab = *(volatile uint32_t *)0x1FFF6998u;
    if (ed < 0x1FFF0000u || ed >= 0x20030000u || (ed & 3u) || *(volatile uint32_t *)ed != 0x000A5A54u || tab != 0x1FFF7918u) return 0;
    /* 041: Slicer-/Granular-Spur: Sample der Spur setzen. Der Weg ueber den Regler (unten) wird dort als "naechstes Sample" gedeutet (seit 029)
     * und liesse die neue Aufnahme unbenutzt. */
    if (sv_assign_sample && sv_assign_sample(slot)) return 1;
    void (*chg)(uint32_t, int, int) = (void (*)(uint32_t, int, int))(*(volatile uint32_t *)(0x000A5A54u + 0x1Cu));
    chg(ed, 9, -32767);
    if (slot) chg(ed, 9, slot);
    return 1;
}
#endif

static void import_last(void) {
    volatile rec_t *s = REC;
    s->imp_slot = -1; s->imp_res = -1;
    if (s->tail - RX->skip < 441u * s->ch) { s->imp_res = 3; return; }            /* unter 10 ms (kein USB-Audio): nicht laden */
    int slot = pool_free_slot();
    if (slot < 0) { s->imp_res = 2; return; }
    char dir[sizeof DIR]; for (uint32_t i = 0; i < sizeof DIR; i++) dir[i] = DIR[i];
    int r = do_import(dir, (const char *)s->path + sizeof DIR);   /* "Rec_NNN.wav" hinter "Samples/USB_Rec/" */
    s->imp_res = r;
    if (r == 0) { s->imp_slot = slot; s->assigned = assign_track(slot); }
}

void rec_last_import(int32_t *slot, int32_t *res) { *slot = REC->imp_slot; *res = REC->imp_res; }

static void finish(void) {
    volatile rec_t *s = REC;
    uint8_t h[44];
    rec_wav_header(h, s->tail - RX->skip, s->ch);
    if (s->heap) { FREE_RING((void *)(uintptr_t)s->ring); s->heap = 0; s->ring = (uint32_t)(uintptr_t)RING; s->cap = CAP; }
    uint32_t bw = 0;
    int r = F_LSEEK(FIL_RAM, 0);
    if (r == 0) r = F_WRITE(FIL_RAM, h, 44u, &bw);
    int c = F_CLOSE(FIL_RAM);
    if (r == 0 && c == 0 && bw == 44u) { s->files++; import_last(); }
    else s->err = r ? r : c ? c : 99;
    s->stops++;
    s->state = R_IDLE;
    s->menu = 0;
}

static int write_some(uint32_t max_chunks, int all) {
    volatile rec_t *s = REC;
    for (uint32_t c = 0; c < max_chunks; c++) {
        uint32_t avail = s->head - s->tail;
        uint32_t cap = s->cap;
        if (avail > cap) avail = cap;
        if (avail == 0u || (!all && avail < CHUNK)) return 0;
        uint32_t idx = s->tail & (cap - 1u), n = avail;
        if (n > cap - idx) n = cap - idx;
        if (n > CHUNK) n = CHUNK;
        uint32_t bw = 0;
        int r = F_WRITE(FIL_RAM, (int16_t *)(uintptr_t)s->ring + idx, n * 2u, &bw);
        if (r != 0 || bw != n * 2u) { s->err = r ? r : 98; return -1; }
        BARRIER();
        s->tail += n;
    }
    return 0;
}

static void rec_step(void);

/* fuer usbmenu.c: Wertzeiger der Menuezeile */
volatile int32_t *rec_menu_value(void) { return REC->magic == REC_MAGIC ? &REC->menu : 0; }

/* Hauptschleife */
void rec_poll(void) {
    volatile rec_t *s = REC;
    if (s->magic != REC_MAGIC || s->state > R_STOP) {        /* 12.11.: Zustand zerstoert → neu anlegen statt dauerhaft blockiert */
        rec_init();                                         /* Heap-Ring nicht freigeben: Zeiger ist dann Muell */
        return;
    }
    if (s->menu != s->menu_seen) {                          /* Menuezeile vom Nutzer geaendert (Menue laeuft auch hier) */
        int32_t want = s->menu ? 1 : 0, busy = s->state != R_IDLE;
        if (want != busy) (void)rec_request(want);
    }
    rec_step();
    s->menu = s->state != R_IDLE;                           /* Anzeige folgt dem Zustand (auch bei Start per USB) */
    s->menu_seen = s->menu;
}

static void rec_step(void) {
    volatile rec_t *s = REC;
    switch (s->state) {
    case R_START: {
        if (!UI_ON()) { s->err = -1; s->state = R_IDLE; s->menu = 0; return; }
        s->ch = STEREO() ? 2u : 1u;                         /* 026: Einstellung "Rec" Mono/Stereo */
        (void)F_MKDIR("Samples"); (void)F_MKDIR(DIR);    /* gibt es meist schon (FR_EXIST) */
        volatile uint8_t *f = (volatile uint8_t *)FIL_RAM;
        int r = FR_EXIST; uint32_t n = s->num;
        for (uint32_t tries = 0; tries < 999u && r == FR_EXIST; tries++) {
            n = n % 999u + 1u;
            make_path(s->path, n);
            for (uint32_t i = 0; i < FIL_SIZE; i++) f[i] = 0;
            r = F_OPEN(FIL_RAM, (const char *)s->path, FA_WRITE | FA_CREATE_NEW);
        }
        if (r != 0) { s->err = r == FR_EXIST ? -2 : r; s->state = R_IDLE; s->menu = 0; return; }
        uint8_t h[44]; uint32_t bw = 0;
        rec_wav_header(h, 0, s->ch);
        if (F_WRITE(FIL_RAM, h, 44u, &bw) != 0 || bw != 44u) { s->err = 97; F_CLOSE(FIL_RAM); s->state = R_IDLE; s->menu = 0; return; }
        s->ring = (uint32_t)(uintptr_t)RING; s->cap = CAP; s->heap = 0;
        if (s->ch == 2u) {                                  /* Stereo: groesserer Ring, falls der Heap ihn hergibt */
            void *p = ALLOC_RING(CAP_HEAP * 2u);
            if (p && !((uintptr_t)p & 3u)) { s->ring = (uint32_t)(uintptr_t)p; s->cap = CAP_HEAP; s->heap = 1; }
            else if (p) FREE_RING(p);
        }
        s->num = n; s->err = 0; s->drops = 0; s->peak = 0;
        s->tail = 0;
        {   volatile rx_t *x = RX;                          /* 045: Einstellungen gelten fuer die ganze Aufnahme */
            x->skip = 0; x->quiet = 0; x->pend = 0; x->paused = 0;
            x->w_on = REC_WAIT() ? 1u : 0u; x->p_on = REC_PAUSE() ? 1u : 0u;
            x->waiting = x->w_on; }
        BARRIER();
        s->head = 0;
        s->t_start = MILLIS();
        BARRIER();
        s->state = R_REC;
        return;
    }
    case R_REC:
        if (!UI_ON() || s->head - RX->skip >= MAX_SAMPLES * s->ch) {
            s->state = R_STOP;                              /* 12.11.: sofort abschliessen – nach einem Variantenwechsel */
            rec_step();                                     /* gehoert der FIL-Bereich bald dem Multitrack-Sendepuffer */
            return;
        }
        if (RX->waiting || RX->paused) return;              /* 045: Ring gehoert dem ISR (nur Vorlauf) */
        if (write_some(2u, RX->pend ? 1 : 0) < 0) { finish(); return; }
        return;
    case R_STOP:
        {   volatile rx_t *x = RX;                          /* 045: Vorlauf ohne Signal bzw. Stille nicht schreiben */
            if (x->waiting || x->paused || x->pend) { uint32_t d = s->head - s->tail; x->skip += d; s->tail = s->head; }
            x->waiting = 0; x->paused = 0; x->pend = 0; }
        if (write_some(s->cap / CHUNK + 1u, 1) < 0) { finish(); return; }
        finish();
        return;
    default:
        return;
    }
}
