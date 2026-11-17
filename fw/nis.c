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

#define NIS_NTYPES   15u                 /* 4 Original + 2 Note In Scale + 3 (045: Length, Humanize, Octave Jump) + 5 (11.11.) */
#define AUD_NTYPES   18u                 /* 045 (11.11.): 10 Original + Humanize, Octave Jump + 5 wie MIDI */
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
/* 13.11.: Listen, Generatoren und Namen liegen im Kopfblock (guard.c): das Original zeigt direkt darauf (Literale) */
#ifdef HOST_TEST
#define HEAD
#else
#define HEAD __attribute__((section(".head.data")))
void sn_gen(void);                                   /* guard.c: Weiche vor nis_gen_entry */
#endif
#ifdef HOST_TEST
static const uint32_t NIS_VT[3] = { (uint32_t)(uintptr_t)nis_gen_entry, 0, 0 };
#else
HEAD static const uint32_t NIS_VT[3] = { (uint32_t)(uintptr_t)sn_gen, 0, 0 };   /* wie 0xA59BC: {fn, 0, 0} */
#endif
HEAD static const uint32_t OCT_GEN[1] = { 0x000A59BCu };   /* = Heap-Objekt von „1/2 Octave(s)“: nur die vtable (FUN_00038668) */
/* 045: 2 Humanize, 3 Octave ±1; 11.11.: 4 Shuffle, 5 Melodic Walk, 6 Accent, 7 Call & Response, 8 Thin Out
 * (4/5/7 brauchen die anderen Steps der Spur → Kontext aus rz_value, Einbacken zweistufig in rz_bake) */
enum { M_SCALE = 0, M_SCALEP = 1, M_HUMAN = 2, M_OCT = 3, M_SHUF = 4, M_WALK = 5, M_ACC = 6, M_CR = 7, M_THIN = 8, M_SCAT = 9, M_N = 10 };
/* 12.11.: Swap Steps (M_SHUF), Scatter Steps (M_SCAT), Thin Out (M_THIN) = Step-Operationen: ganze Steps tauschen/verschieben/
 * ausschalten. Wert-Generator laesst alles stehen; Vorschau ueber pf_trigger_hook (rz_preview_src), Einbacken in rz_bake. */
HEAD static const gen_t NIS_GEN[M_N] = { { NIS_VT, 0 }, { NIS_VT, 1 }, { NIS_VT, 2 }, { NIS_VT, 3 }, { NIS_VT, 4 },
                                    { NIS_VT, 5 }, { NIS_VT, 6 }, { NIS_VT, 7 }, { NIS_VT, 8 }, { NIS_VT, 9 } };
/* Generator „Sample in Folder/Pool“ (0xA59C8, Objekt +4 = 1 Ordner / 0 Pool) – im Original Heap-Objekte */
HEAD static const uint32_t SMP_GEN[2][2] = { { 0x000A59C8u, 1u }, { 0x000A59C8u, 0u } };

/* ---------- Verlegte MIDI-Typenliste (Flash, 6 × 0x64 Byte) ---------- */
typedef struct { int32_t idx; uint32_t range; const void *gen; } triple_t;   /* range: low s16 = min, high s16 = max */
typedef struct { const char *name; triple_t t[8]; } rec_t;
_Static_assert(sizeof(rec_t) == 0x64 || sizeof(void *) != 4, "Datensatz 0x64 Byte");
#define END { -1, 0, 0 }
#define ENDS END, END, END, END, END, END, END
#define ENDS6 END, END, END, END, END, END
#define ENDS5 END, END, END, END, END
#define ENDS4 END, END, END, END
#define ENDS3 END, END, END
/* Namen im Kopf (sonst laegen die Zeichenketten in .rodata hinten) */
#define HS(id, txt) HEAD static const char id[] = txt
HS(S_NLEN, "Note Length"); HS(S_HUM, "Humanize"); HS(S_OCTJ, "Octave Jump"); HS(S_SWAP, "Swap Steps"); HS(S_SCAT, "Scatter Steps");
HS(S_WALK, "Melodic Walk"); HS(S_ACC, "Accent"); HS(S_CR, "Call & Response"); HS(S_THIN, "Thin Out");
__attribute__((used, aligned(4))) HEAD
const rec_t NIS_LIST[NIS_NTYPES] = {
    { FW_NAME(0x000A5980u, "Filter CC"), { { 2, 0x1388EC78u, 0 },              ENDS } },   /* .data: CC-Platz 2, ±5000 */
    { FW_NAME(0x000A5944u, "1 Octave"),  { { 6, 0x000CFFF4u, OCT_GEN },        ENDS } },   /* Randomize_initTypes */
    { FW_NAME(0x000A5950u, "2 Octaves"), { { 6, 0x0018FFE8u, OCT_GEN },        ENDS } },
    { FW_NAME(0x0009EC98u, "Velocity"),  { { 0, 0x0000F128u, 0 },              ENDS } },   /* .data: −3800…0 */
    { NIS_NAME(0x10u),   { { 6, 0x000CFFF4u, &NIS_GEN[0] },    ENDS } },   /* In <Scale> */
    { NIS_NAME(0x28u),   { { 6, 0x000CFFF4u, &NIS_GEN[1] },    ENDS } },   /* In <Scale>+ */
    /* 045: Ergebnis = Wert + r × Prozent (r gleichverteilt im Bereich, auf die Step-Grenzen begrenzt; Randomize_applyTriple) */
    { S_NLEN, { { 1, 0x01E0FFC4u, 0 },                 ENDS } },   /* idx 1 (0…3840): −60…+480 */
    { S_HUM,    { { 20, 0x0004FFFCu, &NIS_GEN[2] },      ENDS } },   /* idx 20 Micromove: eigener Generator (Firmware setzt MIDI-Minimum fest 12) */
    { S_OCTJ, { { 6, 0x000CFFF4u, &NIS_GEN[3] },       ENDS } },   /* idx 6 Note: ±1 Oktave (0xA59D4 springt nur nach oben) */
    /* 11.11.: Bereich 0 – der Generator rechnet selbst (Kandidat unbenutzt) */
    { S_SWAP,      { { 18, 0, &NIS_GEN[M_SHUF] }, ENDS } },   /* 12.11.: ganze Steps tauschen, Rhythmus bleibt */
    { S_SCAT,   { { 18, 0, &NIS_GEN[M_SCAT] }, ENDS } },   /* 12.11.: Steps auf freie Plaetze verteilen */
    { S_WALK,    { { 6, 0, &NIS_GEN[M_WALK] },  ENDS } },
    { S_ACC,          { { 0, 0, &NIS_GEN[M_ACC] },   ENDS } },
    { S_CR, { { 6, 0, &NIS_GEN[M_CR] },    ENDS } },
    { S_THIN,        { { 18, 0, &NIS_GEN[M_THIN] }, ENDS } },   /* 12.11.: Steps wirklich aus (vorher Chance) */
};

/* ---------- 045 (11.11.): verlegte Audio-Typenliste (Original RAM 0x1FFF28FC, 10 × 0x64) ---------- */
__attribute__((used, aligned(4))) HEAD
const rec_t AUD_LIST[AUD_NTYPES] = {
    { FW_NAME(0x000A5920u, "Sample In Folder"), { { 9, 0x001EFFE2u, SMP_GEN[0] }, ENDS } },
    { FW_NAME(0x000A5934u, "Sample In Pool"),   { { 9, 0x00FFFF01u, SMP_GEN[1] }, ENDS } },
    { FW_NAME(0x000A598Cu, "Sample Length"),    { { 10, 0x1FFFE001u, 0 }, { 11, 0x1FFFE001u, 0 }, ENDS6 } },
    { FW_NAME(0x000A599Cu, "Texture"),          { { 2, 0x1388EC78u, 0 }, { 3, 0x1388EC78u, 0 }, { 5, 0x1388EC78u, 0 }, ENDS5 } },
    { FW_NAME(0x000A5944u, "1 Octave"),         { { 6, 0x000CFFF4u, OCT_GEN }, ENDS } },
    { FW_NAME(0x000A5950u, "2 Octaves"),        { { 6, 0x0018FFE8u, OCT_GEN }, ENDS } },
    { FW_NAME(0x0009EBCCu, "Volume"),           { { 0, 0x0000F128u, 0 }, ENDS } },
    { FW_NAME(0x0009D4C0u, "Space"),            { { 1, 0x2710D8F0u, 0 }, { 7, 0x1388EC78u, 0 }, { 8, 0x2710D8F0u, 0 }, ENDS5 } },
    { FW_NAME(0x000A595Cu, "Nuke"),             { { 9, 0x0005FFFBu, SMP_GEN[0] }, { 2, 0x1388EC78u, 0 }, { 3, 0x1388EC78u, 0 }, { 5, 0x1388EC78u, 0 }, { 1, 0x2710D8F0u, 0 }, ENDS3 } },
    { FW_NAME(0x000A5964u, "Duke Nuke"),        { { 9, 0x001EFFE2u, SMP_GEN[1] }, { 2, 0x1388EC78u, 0 }, { 3, 0x1388EC78u, 0 }, { 5, 0x1388EC78u, 0 }, { 1, 0x2710D8F0u, 0 }, ENDS3 } },
    { S_HUM,        { { 20, 0x0004FFFCu, &NIS_GEN[M_HUMAN] }, ENDS } },
    { S_OCTJ,     { { 6, 0x000CFFF4u, &NIS_GEN[M_OCT] },    ENDS } },
    { S_SWAP,      { { 18, 0, &NIS_GEN[M_SHUF] }, ENDS } },
    { S_SCAT,   { { 18, 0, &NIS_GEN[M_SCAT] }, ENDS } },
    { S_WALK,    { { 6, 0, &NIS_GEN[M_WALK] },  ENDS } },
    { S_ACC,          { { 0, 0, &NIS_GEN[M_ACC] },   ENDS } },
    { S_CR, { { 6, 0, &NIS_GEN[M_CR] },    ENDS } },
    { S_THIN,        { { 18, 0, &NIS_GEN[M_THIN] }, ENDS } },
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

/* ---------- 045 (11.11.): Kontext des Steps (Spur, Position, Auswahl) ----------
 * rz_value ersetzt die zwei Aufrufe von Randomizer_valueFor (0x38B60 Einbacken, 0x38D5C Vorschau) und merkt sich
 * Randomizer-Objekt und Step. Der Step liegt im Pattern (Engine 0x200025C4: PAT0/PAT1 + 0x10 + t·0xBA8 + 0x128 + i·42),
 * daraus folgen Spur t und Position i; die Auswahl steht im Editor (self+4 → +4 Paarliste (Spur, Step), Anzahl +0x800). */
typedef struct {
    uint32_t magic;            /* 00 */
    uint32_t self, step;       /* 04 aktueller Aufruf */
    uint32_t phase;            /* 0C Einbacken: 0 aus, 1 sammeln, 2 schreiben */
    uint32_t n, cap, buf;      /* 10 Puffer der gesammelten Werte (Heap, int16) */
    uint32_t bakes;            /* 1C Zaehler */
} rz_t;
#define RZ_MAGIC 0x525A3131u
#ifdef HOST_TEST
extern uint8_t host_rz_ram[0x20];
#define RZ ((volatile rz_t *)(void *)host_rz_ram)
#else
#define RZ ((volatile rz_t *)0x1FFF9A60u)
_Static_assert(sizeof(rz_t) <= 0x20, "rz_t 0x1FFF9A60..0x1FFF9A7F");
#define PAT0 0x200025D0u
#define PAT1 0x2000E060u
#endif
#define TRK_SIZE 0xBA8u
#define STP_OFS  0x128u
#define STP_SIZE 42u

/* 12.11.: Paarliste der Auswahl (Spur, Step) liegt am Geraet im SDRAM (PEEK: 0x70346954)! Vorher nur internes RAM erlaubt →
 * Step-Operationen taten nichts, Walk/Call & Response nahmen die ganze Spur. Liste 0x804 Byte. */
#define PR_OK(p) ((((p) >= 0x1FFF0000u && (p) < 0x2003F000u) || ((p) >= 0x70000000u && (p) < 0x71FFF000u)) && !((p) & 3u))
typedef struct { uint32_t tb, i, n; uint32_t seed; uint8_t list[64]; uint32_t pos; } ctx_t;   /* list = gewaehlte, belegte Steps */

#ifndef HOST_TEST
static int vt_ok(uint32_t st) {
    if (st < 0x1FFF0000u || st >= 0x2003FF00u || (st & 1u)) return 0;
    uint32_t vt = *(const volatile uint32_t *)st;
    return vt >= 0x9A000u && vt < 0xAE000u;
}
static int st_on(uint32_t st) { return vt_ok(st) && ((((uint32_t (*)(uint32_t))(((const uint32_t *)*(const volatile uint32_t *)st)[3]))(st)) & 0xFFu) != 0u; }
static int st_get(uint32_t st, uint32_t idx, int *v) {
    if (!vt_ok(st)) return 0;
    uint16_t x = 0;
    uint32_t ok = ((uint32_t (*)(uint32_t, uint32_t, uint16_t *))(((const uint32_t *)*(const volatile uint32_t *)st)[6]))(st, idx, &x);
    *v = (int16_t)x;
    return (ok & 0xFFu) != 0u;
}
static void st_lim(uint32_t st, uint32_t idx, int *lo, int *hi) {
    int16_t a = 0, b = 0;
    if (vt_ok(st)) ((void (*)(uint32_t, uint32_t, int16_t *, int16_t *))(((const uint32_t *)*(const volatile uint32_t *)st)[7]))(st, idx, &a, &b);
    *lo = a; *hi = b;
}
static int ctx_get(ctx_t *c) {
    uint32_t st = RZ->step, self = RZ->self;
    if (RZ->magic != RZ_MAGIC || !st || self < 0x1FFF0000u || self >= 0x2003FF00u || (self & 3u)) return 0;
    const uint32_t pats[2] = { PAT0, PAT1 };
    uint32_t t = 8u;
    for (uint32_t p = 0; p < 2u; p++) {
        uint32_t b = pats[p] + 0x10u;
        if (st < b || st >= b + 16u * TRK_SIZE) continue;   /* 12.11.: MIDI-Spuren 8–15 liegen dahinter (vorher nur 0–7!) */
        uint32_t off = st - b, r;
        t = off / TRK_SIZE; r = off % TRK_SIZE;
        if (r < STP_OFS || (r - STP_OFS) % STP_SIZE != 0u) return 0;
        c->i = (r - STP_OFS) / STP_SIZE;
        c->tb = b + t * TRK_SIZE + STP_OFS;
        break;
    }
    if (t >= 16u) return 0;
    c->seed = *(const volatile uint32_t *)(self + 0x10u);
    uint32_t m[2] = { 0, 0 };
    uint32_t sel = *(const volatile uint32_t *)(self + 4u), pr = 0;
    if (sel >= 0x1FFF0000u && sel < 0x2003FF00u && !(sel & 3u)) pr = *(const volatile uint32_t *)(sel + 4u);
    if (PR_OK(pr)) {
        uint32_t cnt = *(const volatile uint32_t *)(pr + 0x800u);
        if (cnt > 0x400u) cnt = 0;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t tr = *(const volatile uint8_t *)(pr + 2u * k), sp = *(const volatile uint8_t *)(pr + 2u * k + 1u);
            if (tr == t && sp < 64u) m[sp >> 5] |= 1u << (sp & 31u);
        }
    }
    if (!(m[0] | m[1])) { m[0] = m[1] = 0xFFFFFFFFu; }   /* keine Auswahl lesbar: ganze Spur */
    c->n = 0; c->pos = 0xFFu;
    for (uint32_t j = 0; j < 64u; j++) {
        if (!((m[j >> 5] >> (j & 31u)) & 1u)) continue;
        if (j != c->i && !st_on(c->tb + j * STP_SIZE)) continue;
        if (j == c->i) c->pos = c->n;
        c->list[c->n++] = (uint8_t)j;
    }
    return c->pos != 0xFFu;
}
#else
static int st_on(uint32_t st) { (void)st; return 0; }
static int st_get(uint32_t st, uint32_t idx, int *v) { (void)st; (void)idx; (void)v; return 0; }
static void st_lim(uint32_t st, uint32_t idx, int *lo, int *hi) { (void)st; (void)idx; *lo = -32768; *hi = 32767; }
static int ctx_get(ctx_t *c) { (void)c; return 0; }      /* Host-Test: kein Pattern → Kontext-Typen lassen den Wert */
#endif

/* Zufall je (Typ-Seed, Position, Zweck): 16 Bit, deterministisch → Vorschau und Einbacken gleich */
static uint32_t hrnd(uint32_t seed, uint32_t j, uint32_t salt) {
    uint32_t x = (seed ^ (j * 0x9E3779B1u) ^ (salt * 0x85EBCA77u)) % 0x7FFFFFFFu;
    if (!x) x = 1u;
    x = minstd(x); x = minstd(x); x = minstd(x);
    return r16(x);
}

/* d Scale-Stufen von n aus (Vorzeichen = Richtung), in [lo, hi] */
static int scale_move(const scale_t *s, int n, int d, int lo, int hi) {
    int dir = d < 0 ? -1 : 1, k = d < 0 ? -d : d;
    while (k-- > 0) {
        int m = n;
        do { m += dir; } while (m >= lo && m <= hi && !in_scale(s, m));
        if (m < lo || m > hi) break;
        n = m;
    }
    return n;
}

static int note_at(const ctx_t *c, uint32_t k, int dflt) {
    int v;
    return st_get(c->tb + (uint32_t)c->list[k] * STP_SIZE, 6u, &v) ? v : dflt;
}

/* Melodic Walk: eine Linie ab der ersten gewaehlten Note, je Step 0…2 Scale-Stufen, nie weiter als ±7 Halbtoene */
static int walk_at(const ctx_t *c, uint32_t upto, int anchor, int lo, int hi) {
    scale_t s = cur_scale();
    int w = anchor;
    for (uint32_t k = 1; k <= upto; k++) {
        uint32_t j = c->list[k], r = hrnd(c->seed, j, 4);
        int d = (int)(r % 4u);                             /* 0 halten, 1/2/3 → 1, 1, 2 Stufen */
        d = d == 0 ? 0 : d == 3 ? 2 : 1;
        int up = (int)((r >> 4) & 1u);
        if (w - anchor > 7) up = 0;
        if (anchor - w > 7) up = 1;
        if (d) w = scale_move(&s, w, up ? d : -d, lo, hi);
    }
    return w;
}

/* Kontext-Typen; p = Anteil 0…65536; Rueckgabe neuer Wert */
static int ctx_gen(uint32_t mode, uint32_t idx, int orig, uint32_t p) {
    ctx_t c;
    uint32_t st = RZ->step;
    if (!ctx_get(&c)) return orig;
    if (c.i != c.list[c.pos]) return orig;
    if (!st_on(st)) return orig;                          /* leere Steps bleiben */
    int lo, hi; st_lim(st, idx, &lo, &hi);
    if (idx == 6u) { if (lo < NIS_NOTE_LO) lo = NIS_NOTE_LO; if (hi > NIS_NOTE_HI) hi = NIS_NOTE_HI; }
    int v = orig;
    switch (mode) {                                       /* 12.11.: M_SHUF/M_THIN/M_SCAT sind Step-Operationen (unten) */
    case M_WALK: {
        if (c.pos == 0u || hrnd(c.seed, c.i, 3) >= p) return orig;
        int a = note_at(&c, 0, orig);
        if (a < lo) a = lo;
        if (a > hi) a = hi;
        v = walk_at(&c, c.pos, a, lo, hi);
        break;
    }
    case M_ACC: {                                         /* Viertel + einzelne Zufallsakzente laut, Rest leiser */
        int acc = (c.i % 4u) == 0u || hrnd(c.seed, c.i, 6) < p / 4u;
        if (acc) v = orig + (int)(((uint32_t)(hi - orig) * p) >> 16);
        else     v = orig - (int)(((uint32_t)(orig - lo) * (p / 2u)) >> 16);
        break;
    }
    case M_CR: {                                          /* zweite Haelfte = erste Haelfte, letzte Antwortnote anders */
        uint32_t half = c.n / 2u;
        if (half == 0u || c.pos < c.n - half) return orig;
        uint32_t src = c.pos - (c.n - half);
        if (hrnd(c.seed, c.i, 7) >= p && p < 65536u) return orig;
        v = note_at(&c, src, orig);
        if (c.pos == c.n - 1u) {                          /* Antwort endet anders: 1–2 Stufen, Richtung zufaellig */
            scale_t s = cur_scale();
            uint32_t r = hrnd(c.seed, c.i, 8);
            int w = scale_move(&s, v, (r & 1u) ? (int)(1u + ((r >> 1) & 1u)) : -(int)(1u + ((r >> 1) & 1u)), lo, hi);
            if (w == v) w = scale_move(&s, v, (r & 1u) ? -1 : 1, lo, hi);
            v = w;
        }
        break;
    }
    default: return orig;
    }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v != orig) NS->changed++;
    return v;
}

__attribute__((used))
int nis_gen(const gen_t *gen, const void *triple, int cand, int orig, uint32_t pct_bits, uint32_t *seed) {
    (void)triple; (void)cand;
    ensure();
    NS->calls++;
    uint32_t p = nis_q16(pct_bits);
    if (p == 0u) return orig;
    {   uint32_t m0 = gen ? gen->mode : 0u;                 /* 11.11.: Kontext-Typen */
        if (m0 >= M_SHUF && m0 < M_N) return ctx_gen(m0, triple ? (uint32_t)((const int32_t *)triple)[0] : 0u, orig, p); }
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
    {   /* 045: Modi ohne Scale, vom Originalwert aus (Anteil der geaenderten Steps = Prozent) */
        uint32_t m_ = gen ? gen->mode : 0u;
        if (m_ == 2u) {                                   /* Humanize: Micromove ±1…4, Grenzen −11…+11 (MIDI und Audio) */
            int d = (int)((u2 >> 11) % 8u); d = d < 4 ? d - 4 : d - 3;   /* hoehere Bits: minstd ist unten gleichfoermig */
            int v = orig + d; if (v < -11) v = -11; if (v > 11) v = 11;
            if (v != orig) NS->changed++;
            return v;
        }
        if (m_ == 3u) {                                   /* Octave ±1: Richtung zufaellig, an den Grenzen die andere */
            int nlo = NIS_NOTE_LO;                        /* 11.11.: Audio-Note 0…119 */
            { int a, b; if (RZ->step) { st_lim(RZ->step, 6u, &a, &b); if (b == 119 && a == 0) nlo = 0; } }
            int v = ((u2 >> 17) & 1u) ? orig + 12 : orig - 12;
            if (v < nlo || v > NIS_NOTE_HI) v = (v > orig) ? orig - 12 : orig + 12;
            if (v < nlo || v > NIS_NOTE_HI) return orig;
            NS->changed++;
            return v;
        }
    }

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

/* ---------- 045 (11.11.): Aufrufe von Randomizer_valueFor / Randomizer_bake ---------- */
#define ORIG_VALUEFOR ((int (*)(uint32_t, uint32_t, int, uint32_t))0x00038A75u)
#define ORIG_BAKE     ((void (*)(uint32_t, uint32_t))0x00038B01u)
#define RZ_MALLOC(n)  (((void *(*)(uint32_t))0x00056F5Du)(n))
#define RZ_FREE(p)    (((void (*)(void *))0x00056F6Du)(p))

static void rz_ensure(void) {
    volatile rz_t *z = RZ;
    if (z->magic != RZ_MAGIC) { z->self = 0; z->step = 0; z->phase = 0; z->n = 0; z->cap = 0; z->buf = 0; z->bakes = 0; z->magic = RZ_MAGIC; }
}

/* 0x38D5C (Vorschau aus Randomizer_modify): verschachtelte Aufrufe (Sequencer-IRQ) stellen den Kontext wieder her */
__attribute__((used, section(".text.entry_rzprev")))
int rz_value_prev(uint32_t self, uint32_t k, int value, uint32_t step) {
    rz_ensure();
    uint32_t os = RZ->self, ot = RZ->step;
    RZ->self = self; RZ->step = step;
    int v = ORIG_VALUEFOR(self, k, value, step);
    RZ->self = os; RZ->step = ot;
    return v;
}

/* 0x38B60 (Randomizer_bake): in Phase 1 nur sammeln (alten Wert zurueckschreiben), in Phase 2 die Sammlung schreiben */
__attribute__((used, section(".text.entry_rzbake")))
int rz_value_bake(uint32_t self, uint32_t k, int value, uint32_t step) {
    rz_ensure();
    volatile rz_t *z = RZ;
    uint32_t os = z->self, ot = z->step;
    z->self = self; z->step = step;
    int v = (int16_t)ORIG_VALUEFOR(self, k, value, step);
    z->self = os; z->step = ot;
    int16_t *b = (int16_t *)(uintptr_t)z->buf;
    if (z->phase == 1u) { if (b && z->n < z->cap) b[z->n] = (int16_t)v; z->n++; return value; }
    if (z->phase == 2u) { if (b && z->n < z->cap) v = b[z->n]; z->n++; }
    return v;
}

static const rec_t *cur_rec(uint32_t self) {
    uint32_t sel = *(const volatile uint32_t *)(self + 4u), ti = *(const volatile uint32_t *)(self + 0xCu);
    if (sel < 0x1FFF0000u || sel >= 0x2003FF00u) return 0;
    if (*(const volatile uint8_t *)(sel + 0x84u)) return ti < NIS_NTYPES ? &NIS_LIST[ti] : 0;
    return ti < AUD_NTYPES ? &AUD_LIST[ti] : 0;
}

/* ---------- 12.11.: Step-Operationen Swap Steps / Scatter Steps / Thin Out ----------
 * Plan je Spur: src[j] = Position, deren Step nach dem Einbacken an j steht (j = unveraendert, 0xFF = leer).
 * Bereich = gewaehlte Positionen der Spur innerhalb der Spurlaenge (Spur+0xC0). Anzahl = Anteil p der Steps (gerundet).
 *   Swap:    gesetzte Steps tauschen untereinander (Sattolo: jeder Teilnehmer landet woanders), Rhythmus bleibt.
 *   Scatter: gesetzte Steps wandern auf freie Plaetze (erst freie, dann Plaetze anderer Wanderer); Anzahl bleibt.
 *   Thin Out: gesetzte Steps aus; Viertel bis 75 % geschuetzt, mindestens ein Step bleibt.
 * Deterministisch aus Seed (+0x10) und Anteil → Vorschau (pf_trigger_hook) = Ergebnis (rz_bake). */
#define RZ_OBJ    0x1FFF4640u                             /* Randomizer im SeqCtl (+0x160): vt 0xA59F8, +4 Auswahl, +0xC Typ, +0x30 Anteil (float) */
static uint32_t step_mode(uint32_t self) {
    const rec_t *r = cur_rec(self);
    if (!r) return M_N;
    const gen_t *g = (const gen_t *)r->t[0].gen;
    if (g == &NIS_GEN[M_SHUF]) return M_SHUF;
    if (g == &NIS_GEN[M_SCAT]) return M_SCAT;
    if (g == &NIS_GEN[M_THIN]) return M_THIN;
    return M_N;
}
/* die m Eintraege mit kleinstem Schluessel markieren (stabil: bei Gleichstand die fruehere Position) */
static void pick(const uint8_t *L, uint32_t n, uint32_t m, uint32_t seed, uint32_t salt, uint8_t *mark) {
    uint32_t key[64];
    for (uint32_t k = 0; k < n; k++) { key[k] = hrnd(seed, L[k], salt); mark[k] = 0; }
    for (uint32_t c = 0; c < m && c < n; c++) {
        uint32_t best = 0xFFu;
        for (uint32_t k = 0; k < n; k++) if (!mark[k] && (best == 0xFFu || key[k] < key[best])) best = k;
        mark[best] = 1;
    }
}
static uint32_t share(uint32_t n, uint32_t p) { return p >= 65536u ? n : (n * p + 32768u) >> 16; }

int rz_plan(uint32_t self, uint32_t trk, uint32_t t, uint32_t mode, uint32_t p, uint8_t *src) {
    if (t >= 16u || !p) return 0;
    uint32_t len = *(const volatile uint32_t *)(trk + 0xC0u);
    if (len < 1u || len > 64u) return 0;
    uint32_t m2[2] = { 0, 0 };
    uint32_t sel = *(const volatile uint32_t *)(self + 4u), pr = 0;
    if (sel >= 0x1FFF0000u && sel < 0x2003FF00u && !(sel & 3u)) pr = *(const volatile uint32_t *)(sel + 4u);
    if (!PR_OK(pr)) return 0;
    uint32_t cnt = *(const volatile uint32_t *)(pr + 0x800u);
    if (cnt > 0x400u) return 0;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t tr = *(const volatile uint8_t *)(pr + 2u * k), sp = *(const volatile uint8_t *)(pr + 2u * k + 1u);
        if (tr == t && sp < len) m2[sp >> 5] |= 1u << (sp & 31u);
    }
    if (!(m2[0] | m2[1])) return 0;
    uint32_t seed = *(const volatile uint32_t *)(self + 0x10u);
    uint8_t on[64], L[64], E[64], mark[64]; uint32_t n = 0, ne = 0;
    for (uint32_t j = 0; j < 64u; j++) {
        src[j] = (uint8_t)j; on[j] = 0;
        if (!((m2[j >> 5] >> (j & 31u)) & 1u)) continue;
        on[j] = (uint8_t)st_on(trk + STP_OFS + j * STP_SIZE);
        if (on[j]) L[n++] = (uint8_t)j; else E[ne++] = (uint8_t)j;
    }
    if (mode == M_SHUF) {
        if (n < 2u) return 0;
        uint32_t m = share(n, p); if (m < 2u) m = 2u;
        pick(L, n, m, seed, 1u, mark);
        uint8_t P[64], Q[64]; uint32_t np = 0;
        for (uint32_t k = 0; k < n; k++) if (mark[k]) P[np++] = L[k];
        for (uint32_t k = 0; k < np; k++) Q[k] = P[k];
        for (uint32_t k = np - 1u; k > 0u; k--) {          /* Sattolo: ein Zyklus, kein Step bleibt stehen */
            uint32_t q = hrnd(seed, k, 2u) % k;
            uint8_t x = Q[k]; Q[k] = Q[q]; Q[q] = x;
        }
        for (uint32_t k = 0; k < np; k++) src[P[k]] = Q[k];
        return 1;
    }
    if (mode == M_SCAT) {
        if (!n || !ne) return 0;
        uint32_t m = share(n, p); if (m < 1u) m = 1u;
        pick(L, n, m, seed, 11u, mark);
        uint8_t W[64], pool[128]; uint32_t nw = 0, npool = 0;
        for (uint32_t k = 0; k < n; k++) if (mark[k]) W[nw++] = L[k];
        for (uint32_t k = 0; k < ne; k++) pool[npool++] = E[k];
        for (uint32_t k = npool; k > 1u; k--) { uint32_t q = hrnd(seed, k, 12u) % k; uint8_t x = pool[k - 1u]; pool[k - 1u] = pool[q]; pool[q] = x; }
        for (uint32_t k = 0; k < nw; k++) pool[npool++] = W[(k + 1u) % nw];   /* reichen die freien Plaetze nicht: Plaetze anderer Wanderer */
        for (uint32_t k = 0; k < nw; k++) src[W[k]] = 0xFFu;
        for (uint32_t k = 0; k < nw; k++) src[pool[k]] = W[k];
        return 1;
    }
    if (mode == M_THIN) {
        if (n < 2u) return 0;
        uint8_t C[64]; uint32_t nc = 0;
        for (uint32_t k = 0; k < n; k++) if (!((L[k] % 4u) == 0u && p < 49152u)) C[nc++] = L[k];
        uint32_t m = share(nc, p);
        if (m >= n) m = n - 1u;                            /* mindestens ein Step bleibt */
        if (!m) return 0;
        pick(C, nc, m, seed, 9u, mark);
        for (uint32_t k = 0; k < nc; k++) if (mark[k]) src[C[k]] = 0xFFu;
        return 1;
    }
    return 0;
}

static uint32_t rz_pct(uint32_t self) {
    uint32_t bits = *(const volatile uint32_t *)(self + 0x30u);
    return (bits & 0x7FFFFFFFu) ? nis_q16(bits) : 0u;
}

/* Vorschau (pf_trigger_hook, Sequencer): Position j der Spur trk (Spurobjekt) → -1 unveraendert, -2 still, sonst Quell-Position */
int rz_preview_src(uint32_t trk, uint32_t t, uint32_t j) {
    uint32_t self = RZ_OBJ;
    if (*(const volatile uint32_t *)self != 0x000A59F8u || j >= 64u) return -1;
    uint32_t p = rz_pct(self);
    if (!p) return -1;
    uint32_t mode = step_mode(self);
    if (mode == M_N) return -1;
    uint8_t src[64];
    if (!rz_plan(self, trk, t, mode, p, src)) return -1;
    if (src[j] == j) return -1;
    return src[j] == 0xFFu ? -2 : (int)src[j];
}

/* 12.11.: sichtbare Vorschau. Editor-vtable 0xA5A54 +0x2C (Wort 0xA5A80) = FUN_0003a3d4(seite, reihe, spalte) zeichnet ein
 * Step-Pad; "an" liest es ueber Step-vt+0xC (Bit 7 von +0x29). Weicht die Vorschau an diesem Pad ab, wird das Bit fuer den
 * Zeichenaufruf auf den Vorschau-Zustand gesetzt (Interrupts aus, damit der Sequencer es nicht sieht) und danach
 * zurueckgesetzt → Farben/Auswahl/Playhead exakt wie Original. Seite+4 = SeqCtl, SeqCtl+4 = Pattern, Seite+0x112 = MIDI,
 * Step-Seite der Spur bei SeqCtl + (spur + 0x904)·4. */
#define ORIG_PADDRAW ((void (*)(uint32_t, uint32_t, uint32_t))0x0003A3D5u)
static uint32_t irq_off(void) { uint32_t m; __asm volatile ("mrs %0, primask\n cpsid i" : "=r"(m) :: "memory"); return m; }
static void irq_restore(uint32_t m) { __asm volatile ("msr primask, %0" :: "r"(m) : "memory"); }

static uint32_t track_base(uint32_t pat, uint32_t t) {     /* Spurbasis wie rz_apply: pat->vt[0](t) → Spur->vt[0](0) − 0x128 */
    if (pat < 0x1FFF0000u || pat >= 0x2003F000u || (pat & 3u)) return 0;
    uint32_t pvt = *(const volatile uint32_t *)pat;
    if (!((pvt >= 0x9A000u && pvt < 0xAE000u) || (pvt >= 0x1FFF0000u && pvt < 0x2003F000u))) return 0;
    uint32_t to = ((uint32_t (*)(uint32_t, uint32_t))(*(const volatile uint32_t *)pvt))(pat, t);
    if (to < 0x1FFF0000u || to >= 0x2003F000u || (to & 3u)) return 0;
    uint32_t tvt = *(const volatile uint32_t *)to;
    if (!((tvt >= 0x9A000u && tvt < 0xAE000u) || (tvt >= 0x1FFF0000u && tvt < 0x2003F000u))) return 0;
    uint32_t s0 = ((uint32_t (*)(uint32_t, uint32_t))(*(const volatile uint32_t *)tvt))(to, 0u);
    if (s0 < 0x1FFF0000u + STP_OFS || s0 >= 0x2003F000u) return 0;
    return s0 - STP_OFS;
}

__attribute__((used, section(".text.entry_rzpad")))
void rz_pad_draw(uint32_t pg, uint32_t row, uint32_t col) {
    uint32_t self = RZ_OBJ, ctl = *(const volatile uint32_t *)(pg + 4u);
    uint32_t p = 0, mode = M_N;
    if (row < 8u && col < 16u && *(const volatile uint32_t *)self == 0x000A59F8u && (p = rz_pct(self)) != 0u
        && (mode = step_mode(self)) != M_N && ctl >= 0x1FFF0000u && ctl < 0x2003F000u) {
        uint32_t t = row + (*(const volatile uint8_t *)(pg + 0x112u) ? 8u : 0u);
        uint32_t j = col + *(const volatile uint32_t *)(ctl + (t + 0x904u) * 4u) * 16u;
        uint32_t trk = track_base(*(const volatile uint32_t *)(ctl + 4u), t);
        uint8_t src[64];
        if (trk && j < 64u && rz_plan(self, trk, t, mode, p, src) && src[j] != j) {
            volatile uint8_t *f = (volatile uint8_t *)(trk + STP_OFS + j * STP_SIZE + 0x29u);
            uint8_t want = src[j] == 0xFFu ? 0u : 0x80u, old = *f;
            if ((old & 0x80u) != want) {
                uint32_t m = irq_off();
                *f = (uint8_t)((old & 0x7Fu) | want);
                ORIG_PADDRAW(pg, row, col);
                *f = old;
                irq_restore(m);
                return;
            }
        }
    }
    ORIG_PADDRAW(pg, row, col);
}

/* Hauptschleife (aus pf_poll): Vorschau geaendert (Anteil, Seed, Typ, an/aus) → alle Step-Pads neu (FUN_000213b4(SeqCtl)) */
#define RZ_KEY (*(volatile uint32_t *)0x1FFF9FD8u)          /* pf_t+0x58 */
void rz_poll(void) {
    uint32_t self = RZ_OBJ, ctl = 0x1FFF44E0u;
    uint32_t key = 0;
    if (*(const volatile uint32_t *)self == 0x000A59F8u) {
        uint32_t p = rz_pct(self), mode = p ? step_mode(self) : M_N;
        if (mode != M_N) key = (p ^ (*(const volatile uint32_t *)(self + 0x10u) * 31u) ^ (mode << 24) ^ *(const volatile uint32_t *)(self + 4u)) | 1u;
    }
    if (key == RZ_KEY) return;
    RZ_KEY = key;
    uint32_t page = *(const volatile uint32_t *)(ctl + 0x24B4u);
    if (page >= 0x1FFF0000u && page < 0x2003F000u && *(const volatile uint32_t *)page == 0x000A5A54u)
        ((void (*)(uint32_t))0x000213B5u)(ctl);             /* FUN_000213b4: 8 × 16 Step-Pads ueber vt+0x2C */
}

/* Einbacken: Plan auf die Steps anwenden (Daten +4…+41 kopieren; leer = Wort +0x26 (gesetzt-Bits + an) auf 0) */
static void rz_apply(uint32_t self, uint32_t pat, uint32_t mode, uint32_t p) {
    if (pat < 0x1FFF0000u || pat >= 0x2003F000u || (pat & 3u)) return;
    uint32_t pvt = *(const volatile uint32_t *)pat;
    if (!((pvt >= 0x9A000u && pvt < 0xAE000u) || (pvt >= 0x1FFF0000u && pvt < 0x2003F000u))) return;   /* Flash (Geraet) oder RAM (Test-Attrappe) */
    for (uint32_t t = 0; t < 16u; t++) {
        /* Spur wie die Firmware: pat->vt[0](pat, t) → Spur, Spur->vt[0](spur, 0) → Step 0; Spurbasis = Step 0 − 0x128 */
        uint32_t to = ((uint32_t (*)(uint32_t, uint32_t))(*(const volatile uint32_t *)pvt))(pat, t);
        if (to < 0x1FFF0000u || to >= 0x2003F000u || (to & 3u)) continue;
        uint32_t tvt = *(const volatile uint32_t *)to;
        if (!((tvt >= 0x9A000u && tvt < 0xAE000u) || (tvt >= 0x1FFF0000u && tvt < 0x2003F000u))) continue;
        uint32_t s0 = ((uint32_t (*)(uint32_t, uint32_t))(*(const volatile uint32_t *)tvt))(to, 0u);
        if (s0 < 0x1FFF0000u + STP_OFS || s0 >= 0x2003F000u) continue;
        uint32_t trk = s0 - STP_OFS;
        uint8_t src[64];
        if (!rz_plan(self, trk, t, mode, p, src)) continue;
        uint8_t *tmp = (uint8_t *)RZ_MALLOC(64u * 38u);
        if (!tmp) return;
        for (uint32_t j = 0; j < 64u; j++)
            for (uint32_t b = 0; b < 38u; b++) tmp[j * 38u + b] = *(const volatile uint8_t *)(trk + STP_OFS + j * STP_SIZE + 4u + b);
        for (uint32_t j = 0; j < 64u; j++) {
            if (src[j] == j) continue;
            volatile uint8_t *d = (volatile uint8_t *)(trk + STP_OFS + j * STP_SIZE + 4u);
            if (src[j] == 0xFFu) { for (uint32_t b = 0x22u; b < 0x26u; b++) d[b] = 0; }   /* Step+0x26…0x29 */
            else for (uint32_t b = 0; b < 38u; b++) d[b] = tmp[(uint32_t)src[j] * 38u + b];
        }
        RZ_FREE(tmp);
        RZ->bakes++;
    }
}

/* 0x21B26: Randomizer_bake(self, pattern). Typen, die andere Steps lesen, zweimal durchlaufen lassen:
 * erst alle Werte aus dem unveraenderten Pattern sammeln, dann schreiben (sonst liest Shuffle schon vertauschte Steps). */
__attribute__((used, section(".text.entry_rzbakeall")))
void rz_bake(uint32_t self, uint32_t pat) {
    rz_ensure();
    {   uint32_t mode = step_mode(self);                   /* 12.11.: Step-Operationen */
        if (mode != M_N) {
            uint32_t p = rz_pct(self), seed = *(const volatile uint32_t *)(self + 0x10u);
            ORIG_BAKE(self, pat);                          /* Werte bleiben (Generator laesst sie), setzt 0 % und neue Seeds */
            uint32_t s2 = *(volatile uint32_t *)(self + 0x10u);
            *(volatile uint32_t *)(self + 0x10u) = seed;
            if (p) rz_apply(self, pat, mode, p);
            *(volatile uint32_t *)(self + 0x10u) = s2;
            return;
        }
    }
    const rec_t *r = cur_rec(self);
    uint32_t nt = 0, ctx = 0;
    if (r) for (; nt < 8u && r->t[nt].idx != -1; nt++) {
        const gen_t *g = (const gen_t *)r->t[nt].gen;
        for (uint32_t m = M_SHUF; m < M_N; m++) if (g == &NIS_GEN[m] && (m == M_WALK || m == M_CR)) ctx = 1;
    }
    uint32_t sel = *(const volatile uint32_t *)(self + 4u), pr = sel ? *(const volatile uint32_t *)(sel + 4u) : 0u;
    uint32_t cnt = PR_OK(pr) ? *(const volatile uint32_t *)(pr + 0x800u) : 0u;
    if (!ctx || cnt == 0u || cnt > 0x400u) { ORIG_BAKE(self, pat); return; }
    uint32_t cap = cnt * nt;
    int16_t *b = (int16_t *)RZ_MALLOC(cap * 2u + 4u);
    if (!b) { ORIG_BAKE(self, pat); return; }
    uint32_t save[9];
    for (uint32_t i = 0; i < 9u; i++) save[i] = *(volatile uint32_t *)(self + 0x10u + 4u * i);   /* Seeds 8 + Prozent */
    volatile rz_t *z = RZ;
    z->buf = (uint32_t)(uintptr_t)b; z->cap = cap; z->n = 0; z->phase = 1;
    ORIG_BAKE(self, pat);                                   /* setzt am Ende 0 % und neue Seeds */
    for (uint32_t i = 0; i < 9u; i++) *(volatile uint32_t *)(self + 0x10u + 4u * i) = save[i];
    z->n = 0; z->phase = 2;
    ORIG_BAKE(self, pat);
    z->phase = 0; z->buf = 0; z->cap = 0; z->bakes++;
    RZ_FREE(b);
}
#else
uint8_t host_rz_ram[0x20];
void nis_gen_entry(void) {}
const char *host_nis_name(int v) { return (const char *)NS->name[v]; }
uint32_t host_nis_stat(int k) { return k == 0 ? NS->calls : k == 1 ? NS->changed : NS->outside; }
#endif
