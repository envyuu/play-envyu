/* Patch 018 – Randomize-Typ „Note In Scale“ fuer MIDI-Spuren, Polyend Play Rev A, Firmware 1.6.0
 *
 * Zwei neue Eintraege in der MIDI-Randomize-Typenliste (Index 4 und 5):
 *   4 „In C Dorian“   – Randomize-% = Anteil der Steps, die eine neue Zufallsnote bekommen.
 *                       Die neue Note liegt in der Projekt-Scale (Scale Type + Root Note) im Bereich
 *                       Originalnote ±12 Halbtoene (begrenzt auf 12…119), nie gleich der Originalnote.
 *   5 „In C Dorian+“  – wie 4, aber ab 51 % wird ein Teil der geaenderten Steps chromatisch AUSSERHALB
 *                       der Scale gewuerfelt: Anteil = (p − 50 %) / 2, also hoechstens 25 % bei 100 %.
 * Ist die Projekt-Scale „Chromatic“, wird Dur (Major) auf der Root Note verwendet (Anzeige „In C Major“).
 * Der Name im Popup („Randomize Type“) wird bei jeder Anzeige frisch aus Scale Type + Root Note gebaut.
 *
 * Technik:
 *   - Die MIDI-Typenliste (Original RAM 0x1FFF2190, 4 × 0x64 Byte, direkt dahinter die Step-Action-Liste)
 *     wird nach Flash verlegt (NIS_LIST, 6 × 0x64 Byte). Eintraege 0–3 sind Kopien des Originals
 *     (.data-Image bzw. Randomize_initTypes 0x38E74), die Liste wird nach dem Start nie beschrieben.
 *   - Neuer Generator (vtable NIS_VT, Methode 0 = nis_gen_entry). FUN_000389a0 ruft Generatoren mit
 *     (gen, triple, kandidat, original) auf; der Prozentwert steht dort in s16 (vpush d8), der Zeiger auf
 *     den Zufallszustand des Steps in r7 – beide callee-saved, nis_gen_entry reicht sie an nis_gen weiter.
 *   - Patch-Stellen siehe build.sh (6 Literale, 2 Grenzen 3→5, 1 bl in FUN_00038d74).
 * RAM: 0x1FFFB900 … 0x1FFFB93F (Zustand + zwei Namenspuffer), Rest bis 0x1FFFB9FF frei.
 * Mit -DHOST_TEST wird gegen Attrappen fuer einen PC-Testlauf uebersetzt.
 */
#include <stdint.h>

#define NIS_NTYPES   6u                  /* 4 Original + 2 neue */
#define NIS_NOTE_LO  12                  /* wie FUN_000389a0 fuer MIDI-Steps (Minimum fest 12) */
#define NIS_NOTE_HI  119                 /* wie der Scale-Filter FUN_0003bccc (hoechste Note 0x77) */
#define NIS_SPAN     12                  /* ±1 Oktave um die Originalnote */
#define NIS_NAMEMAX  16u                 /* Zeichen, die im grossen Font sicher passen („Sample In Folder“) */
#define NIS_MAGIC    0x4E495331u         /* "NIS1" */

#ifdef HOST_TEST
extern uint8_t  host_nis_ram[256];
extern uint32_t host_scale_type, host_scale_root;
extern uint16_t host_scale_masks_rw[39];
extern void host_strncpy(char *d, const char *s, uint32_t n);
#define NIS_RAM          ((uintptr_t)host_nis_ram)
#define NIS_NAME(o)      ((const char *)(host_nis_ram + (o)))
#define SCALE_TYPE       host_scale_type
#define SCALE_ROOT       host_scale_root
#define SCALE_MASK(t)    host_scale_masks_rw[(t)]
#define ORIG_STRNCPY(d,s,n) host_strncpy((d),(s),(n))
#define FW_PTR(a)        ((const void *)0)              /* Firmware-Adressen gibt es im Host-Test nicht */
#define FW_NAME(a, s)    (s)
#else
#define NIS_RAM          0x1FFFB900u
#define NIS_NAME(o)      ((const char *)(NIS_RAM + (o)))
/* Projekt-Einstellungen: App 0x1FFF44A8 (FUN_00019e04) + 0x2A20 (FUN_0001a1ec) = 0x1FFF6EC8 */
#define SCALE_TYPE       (*(volatile uint32_t *)0x1FFF70ACu)   /* +0x1E4 scaleType 0…38 */
#define SCALE_ROOT       (*(volatile uint32_t *)0x1FFF70B0u)   /* +0x1E8 scaleRootNote 0…11 */
#define SCALE_MASK(t)    (((const uint16_t *)0x000AC838u)[(t)])  /* Bit n = Halbton n ueber Root */
#define ORIG_STRNCPY(d,s,n) (((void (*)(char *, const char *, uint32_t))0x000369FDu)((d),(s),(n)))
#define FW_PTR(a)        ((const void *)(uintptr_t)(a))
#define FW_NAME(a, s)    ((const char *)(uintptr_t)(a))
#endif

/* ---------- Zustand im RAM (64 Byte) ---------- */
typedef struct {
    uint32_t magic;
    uint32_t calls;                      /* Diagnose: Generator-Aufrufe */
    uint32_t changed;                    /*            davon geaenderte Steps */
    uint32_t outside;                    /*            davon ausserhalb der Scale */
    char     name[2][24];                /* Typnamen fuer Eintrag 4 und 5 */
} nis_t;
#define NS ((volatile nis_t *)NIS_RAM)
_Static_assert(sizeof(nis_t) <= 0x40, "RAM 0x1FFFB900..0x1FFFB93F");
_Static_assert(__builtin_offsetof(nis_t, name) == 0x10 && sizeof(((nis_t *)0)->name[0]) == 0x18, "Namen bei +0x10/+0x28");

/* ---------- Generator-Objekte (Flash) ---------- */
typedef struct { const void *vt; uint32_t mode; } gen_t;
void nis_gen_entry(void);
static const uint32_t NIS_VT[3] = { (uint32_t)(uintptr_t)nis_gen_entry, 0, 0 };   /* wie 0xA59BC: {fn, 0, 0} */
static const uint32_t OCT_GEN[1] = { 0x000A59BCu };   /* = Heap-Objekt von „1/2 Octave(s)“: nur die vtable (FUN_00038668) */
static const gen_t NIS_GEN[2] = { { NIS_VT, 0 }, { NIS_VT, 1 } };

/* ---------- Verlegte MIDI-Typenliste (Flash, 6 × 0x64 Byte) ---------- */
typedef struct { int32_t idx; uint32_t range; const void *gen; } triple_t;   /* range: low s16 = min, high s16 = max */
typedef struct { const char *name; triple_t t[8]; } rec_t;
_Static_assert(sizeof(rec_t) == 0x64 || sizeof(void *) != 4, "Datensatz 0x64 Byte");
#define END { -1, 0, 0 }
#define ENDS END, END, END, END, END, END, END
__attribute__((used, aligned(4)))
const rec_t NIS_LIST[NIS_NTYPES] = {
    { FW_NAME(0x000A5980u, "Filter CC"), { { 2, 0x1388EC78u, 0 },              ENDS } },   /* .data: CC-Platz 2, ±5000 */
    { FW_NAME(0x000A5944u, "1 Octave"),  { { 6, 0x000CFFF4u, OCT_GEN },        ENDS } },   /* Randomize_initTypes */
    { FW_NAME(0x000A5950u, "2 Octaves"), { { 6, 0x0018FFE8u, OCT_GEN },        ENDS } },
    { FW_NAME(0x0009EC98u, "Velocity"),  { { 0, 0x0000F128u, 0 },              ENDS } },   /* .data: −3800…0 */
    { NIS_NAME(0x10u),   { { 6, 0x000CFFF4u, &NIS_GEN[0] },    ENDS } },   /* In <Scale> */
    { NIS_NAME(0x28u),   { { 6, 0x000CFFF4u, &NIS_GEN[1] },    ENDS } },   /* In <Scale>+ */
};

/* ---------- Namen ---------- */
static const char ROOTS[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
/* Kurzformen der 39 Firmware-Scales (Liste 0x9F704, Masken 0xAC838), hoechstens 10 Zeichen */
static const char SCALES[39][11] = {
    "Major",      /* 0 Chromatic → Ersatz Dur */
    "Minor", "Major", "Dorian", "Lydian", "Lydian Min", "Locrian", "Phrygian", "Phryg Dom",
    "Mixolydian", "Mel Minor", "Harm Minor", "BeBop Maj", "BeBop Dor", "BeBop Mixo", "Blues Min",
    "Blues Maj", "Penta Min", "Penta Maj", "Hung Minor", "Ukrainian", "Marva", "Todi", "Whole Tone",
    "Diminish", "Super Locr", "Hirajoshi", "In Sen", "Yo", "Iwato", "Whole Half", "Kumoi", "Overtone",
    "Dbl Harm", "Indian", "Gypsy Min", "Neapol Maj", "Neapol Min", "Enigmatic",
};
#define MAJOR_MASK 0x0AB5u               /* = Maske von Scale 2 „Major“ */

static void ensure(void) {
    if (NS->magic != NIS_MAGIC) {
        NS->calls = 0; NS->changed = 0; NS->outside = 0;
        NS->name[0][0] = 0; NS->name[1][0] = 0;
        NS->magic = NIS_MAGIC;
    }
}

typedef struct { uint32_t type, root, mask; } scale_t;
static scale_t cur_scale(void) {
    scale_t s;
    s.type = SCALE_TYPE; s.root = SCALE_ROOT;
    if (s.type > 38u) s.type = 0;
    if (s.root > 11u) s.root = 0;
    s.mask = SCALE_MASK(s.type) & 0xFFFu;
    if (s.type == 0u || s.mask == 0u) s.mask = MAJOR_MASK;    /* Chromatic (oder unbrauchbar) → Dur */
    return s;
}

static uint32_t put(volatile char *d, uint32_t k, const char *s) {
    while (*s && k < NIS_NAMEMAX + 4u) d[k++] = *s++;
    return k;
}

/* „In C Dorian“ / „In C Dorian+“; laenger als 16 Zeichen → ohne „In “ */
void nis_refresh(void) {
    ensure();
    scale_t s = cur_scale();
    for (uint32_t v = 0; v < 2u; v++) {
        volatile char *d = NS->name[v];
        uint32_t rl = ROOTS[s.root][1] ? 2u : 1u, sl = 0;
        while (SCALES[s.type][sl]) sl++;
        uint32_t len = 3u + rl + 1u + sl + v;              /* "In " + Root + " " + Scale + "+" */
        uint32_t k = 0;
        if (len <= NIS_NAMEMAX) k = put(d, k, "In ");
        k = put(d, k, ROOTS[s.root]);
        k = put(d, k, " ");
        k = put(d, k, SCALES[s.type]);
        if (v) k = put(d, k, "+");
        d[k] = 0;
    }
}

/* Ersatz fuer den strncpy-Aufruf 0x38DC0 in FUN_00038d74 (Anzeige des Typnamens) */
__attribute__((used, section(".text.entry_nisname")))
void nis_name_copy(char *dst, const char *src, uint32_t n) {
    if (src == NIS_NAME(0x10u) || src == NIS_NAME(0x28u)) nis_refresh();
    ORIG_STRNCPY(dst, src, n);
}

/* ---------- Zufall ---------- */
/* minstd_rand (48271, 2^31−1) per Schrage wie FUN_0003876c – nur 32-Bit-Division (udiv) */
static uint32_t minstd(uint32_t x) {
    uint32_t hi = x / 44488u, lo = x % 44488u;
    uint32_t a = lo * 48271u, b = hi * 3399u;
    return a >= b ? a - b : a - b + 0x7FFFFFFFu;
}
/* 16-Bit-Wert aus einem minstd-Wert (1…2^31−2) */
static uint32_t r16(uint32_t x) { return ((x - 1u) >> 15) & 0xFFFFu; }

/* IEEE-754-Bits (0…1) → Anteil in 1/65536 (0…65536), ohne Gleitkomma-Bibliothek */
uint32_t nis_q16(uint32_t bits) {
    if (bits & 0x80000000u) return 0;                     /* negativ (oder −0) */
    uint32_t e = (bits >> 23) & 0xFFu;
    if (e >= 127u) return 65536u;                         /* ≥ 1.0, Inf, NaN */
    uint32_t m = (bits & 0x7FFFFFu) | 0x800000u;
    uint32_t sh = 134u - e;                               /* Wert·2^16 = m · 2^(e−127−23+16) */
    return sh >= 32u ? 0u : m >> sh;
}

/* ---------- Generator ---------- */
static int in_scale(const scale_t *s, int note) {
    int d = (note - (int)s->root) % 12; if (d < 0) d += 12;
    return (int)((s->mask >> d) & 1u);
}

__attribute__((used))
int nis_gen(const gen_t *gen, const void *triple, int cand, int orig, uint32_t pct_bits, uint32_t *seed) {
    (void)triple; (void)cand;
    ensure();
    NS->calls++;
    uint32_t p = nis_q16(pct_bits);
    if (p == 0u) return orig;
    /* Zustand des Steps (lokale Variable in FUN_00038a74); falls unplausibel: eigener Ersatz */
    uint32_t x = 0;
#ifdef HOST_TEST
    if (seed) x = *seed;
#else
    uintptr_t sp = (uintptr_t)seed;
    if ((sp & 3u) == 0u && sp >= 0x1FFF0000u && sp < 0x20030000u) x = *seed;
#endif
    x %= 0x7FFFFFFFu; if (x == 0u) x = 1u;
    x = minstd(x); uint32_t u1 = r16(x);                  /* aendern? */
    x = minstd(x); uint32_t u2 = x;                       /* Wahl in der Scale */
    x = minstd(x); uint32_t u3 = r16(x);                  /* ausserhalb? */
    x = minstd(x); uint32_t u4 = x;                       /* Wahl ausserhalb */
    if (u1 >= p) return orig;

    scale_t s = cur_scale();
    int lo = orig - NIS_SPAN, hi = orig + NIS_SPAN;
    if (lo < NIS_NOTE_LO) lo = NIS_NOTE_LO;
    if (hi > NIS_NOTE_HI) hi = NIS_NOTE_HI;
    uint32_t nin = 0, nout = 0;
    for (int n = lo; n <= hi; n++) if (n != orig) { if (in_scale(&s, n)) nin++; else nout++; }

    uint32_t mode = gen ? gen->mode : 0u;
    int want_out = 0;
    if (mode == 1u && p > 32768u && nout > 0u && u3 < (p - 32768u) / 2u) want_out = 1;
    if (!want_out && nin == 0u) return orig;
    uint32_t pick = want_out ? u4 % nout : u2 % nin, k = 0;
    for (int n = lo; n <= hi; n++) {
        if (n == orig || in_scale(&s, n) == want_out) continue;
        if (k++ == pick) {
            NS->changed++; if (want_out) NS->outside++;
            return n;
        }
    }
    return orig;                                          /* nicht erreichbar */
}

#ifndef HOST_TEST
/* vtable-Methode: (r0 gen, r1 triple, r2 kandidat, r3 original); s16 = Prozent, r7 = &Zufallszustand
 * (beide in FUN_000389a0 gesetzt und callee-saved) → nis_gen(r0…r3, s16-Bits, r7) */
__attribute__((naked, used, section(".text.entry_nisgen")))
void nis_gen_entry(void) {
    __asm volatile (
        "push  {r4, lr}\n"
        "sub   sp, sp, #8\n"
        ".short 0xEE18, 0x4A10\n"        /* vmov r4, s16 */
        "str   r4, [sp, #0]\n"
        "str   r7, [sp, #4]\n"
        "bl    nis_gen\n"
        "add   sp, sp, #8\n"
        "pop   {r4, pc}\n");
}
#else
void nis_gen_entry(void) {}
const char *host_nis_name(int v) { return (const char *)NS->name[v]; }
uint32_t host_nis_stat(int k) { return k == 0 ? NS->calls : k == 1 ? NS->changed : NS->outside; }
#endif
