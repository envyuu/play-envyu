/* Patch 045 – Kopfzeile zeigt die Engine der gewaehlten Spur. Polyend Play Rev A, Firmware 1.6.0.
 *
 * Statuszeile der Firmware (FUN_00029f7c, alle 100 ms aus FUN_00030814 bei 0x30840, Objekt = Bildschirme+0x38):
 *   +0x04 Widget-Manager · +0x0C Zeiger {Pattern-Index, Projektname[20]} · +0x24 Widget rechts ("Pattern %d")
 *   +0x28 Widget links (Projektname) · +0x44 Kopie des Namens (nur bei Aenderung neu geschrieben)
 *   +0x60 aktiv · +0x61 erzwingt Neuschreiben · +0x62 Meldung steht (1,5 s), dann nicht anfassen.
 * Gewaehlte Sample-Spuren mit Slicer (Bit t in 0x1FFF4518) → links "T3 Slicer 16" bzw. "Slicer T1 T3". Keine mehr → +0x61 = 1,
 * die Firmware schreibt den Projektnamen beim naechsten Lauf selbst zurueck. Die Mitte bleibt frei.
 * Hook: bl bei 0x30840 → hdr_hook. RAM: hd_t bei 0x1FFFB680.
 */
#include <stdint.h>
#include "slicer.h"
#include "slview.h"

#define HD_MAGIC 0x4EAD0450u
typedef struct {
    uint32_t magic, own, key, flip;
    char     txt[2][24];
} hd_t;
_Static_assert(sizeof(hd_t) <= 0x80, "hd_t");

#ifdef HOST_TEST
uint8_t host_hd_ram[0x80];
#define HD          ((volatile hd_t *)(void *)host_hd_ram)
extern uint8_t host_sl_ram[];
#define SLR         ((volatile sl_t *)(void *)host_sl_ram)
extern uint32_t host_sel_mask;
#define SEL_AUDIO() (host_sel_mask & 0xFFu)
uint32_t host_hd_orig, host_hd_set; const char *host_hd_txt;
static void orig_status(uint32_t o) { (void)o; host_hd_orig++; }
static void set_left(uint32_t o, const char *t) { (void)o; host_hd_set++; host_hd_txt = t; }
#define R8(a)  (0u)
static void force(uint32_t o) { (void)o; }
#else
_Static_assert(0x1FFFB680u + sizeof(hd_t) <= 0x1FFFB900u, "hd_t vor NIS");
#define HD          ((volatile hd_t *)0x1FFFB680u)
#define SLR         ((volatile sl_t *)0x1FFF9800u)
#define SEL_AUDIO() (*(volatile uint32_t *)0x1FFF4518u & 0xFFu)
#define R8(a)       (*(volatile uint8_t *)(uintptr_t)(a))
#define R32(a)      (*(volatile uint32_t *)(uintptr_t)(a))
static void orig_status(uint32_t o) { ((void (*)(uint32_t))0x00029F7Du)(o); }
static void set_left(uint32_t o, const char *t) {
    uint32_t mgr = R32(o + 4u), w = R32(o + 0x28u);
    if (mgr < 0x1FFF0000u || mgr >= 0x20030000u) return;
    const uint32_t *vt = (const uint32_t *)(uintptr_t)R32(mgr);
    ((void (*)(uint32_t, uint32_t, const char *))vt[0x34 / 4])(mgr, w, t);
    ((void (*)(uint32_t, uint32_t))vt[0x08 / 4])(mgr, w);
    ((void (*)(uint32_t))vt[0x0C / 4])(mgr);
}
static void force(uint32_t o) { R8(o + 0x61u) = 1; }
#endif

void hd_reset(void) {
    volatile uint32_t *z = (volatile uint32_t *)(void *)HD;
    for (uint32_t i = 0; i < sizeof(hd_t) / 4u; i++) z[i] = 0;
    HD->magic = HD_MAGIC;
}

static uint32_t pu(char *b, uint32_t n, uint32_t v) { char t[4]; uint32_t k = 0; do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 3u); while (k) b[n++] = t[--k]; return n; }
static uint32_t ps(char *b, uint32_t n, const char *s) { while (*s && n < 22u) b[n++] = *s++; return n; }

/* Text fuer die gewaehlten Spuren, 0 = keine Slicer-Spur gewaehlt. key = Fingerabdruck (nur bei Aenderung schreiben). */
uint32_t hd_text(char *b, uint32_t *key) {
    volatile sl_t *s = SLR;
    if (s->magic != SL_MAGIC) return 0;
    uint32_t m = SEL_AUDIO(), sl = 0, cnt = 0, first = 8u;
    for (uint32_t t = 0; t < 8u; t++) if ((m >> t) & 1u) { cnt++; if (sl_slices(s->mode[t])) { sl |= 1u << t; if (first == 8u) first = t; } }
    if (!sl) return 0;
    uint32_t n = 0;
    if (cnt == 1u) {
        uint32_t ns = sl_slices(s->mode[first]);
        b[n++] = 'T'; n = pu(b, n, first + 1u); n = ps(b, n, " Slicer ");
        if (sv_beat_on(first)) n = ps(b, n, "Beat"); else n = pu(b, n, ns);
        *key = 0x100u | first | (uint32_t)s->mode[first] << 4 | (uint32_t)sv_beat_on(first) << 12;
    } else {
        n = ps(b, n, "Slicer");
        for (uint32_t t = 0; t < 8u; t++) if ((sl >> t) & 1u) { b[n++] = ' '; b[n++] = 'T'; n = pu(b, n, t + 1u); }
        *key = 0x20000u | sl | cnt << 8;
    }
    b[n] = 0;
    return n;
}

/* bl bei 0x30840 (FUN_00030814 → FUN_00029f7c(Bildschirme+0x38)) */
__attribute__((used, section(".text.entry_hdr")))
void hdr_hook(uint32_t o) {
    volatile hd_t *h = HD;
    uint32_t f61 = R8(o + 0x61u);
    orig_status(o);
    if (h->magic != HD_MAGIC || !R8(o + 0x60u) || R8(o + 0x62u)) return;   /* aus oder Meldung steht: Firmware zuerst */
    char *b = (char *)(uintptr_t)h->txt[h->flip & 1u];
    uint32_t key = 0;
    uint32_t n = hd_text(b, &key);
    if (!n) {
        if (h->own) { force(o); h->own = 0; h->key = 0; }                 /* Projektname zurueck (Firmware schreibt ihn neu) */
        return;
    }
    if (h->own && key == h->key && !f61 && !R8(o + 0x61u)) return;       /* unveraendert */
    set_left(o, b);
    h->flip ^= 1u; h->own = 1; h->key = key;
}
