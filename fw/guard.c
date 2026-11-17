/* 045 (13.11.2026) – Sicherheitsnetz: Kopfblock am Anfang des angehaengten Codes (ab 0xB069C).
 *
 * Problem: Alle Patchstellen im Original-Image springen in den angehaengten Code. Ist der nur teilweise oder fehlerhaft
 * geschrieben (Bootloader schreibt von vorn nach hinten → meist fehlt das ENDE), stuerzt das Geraet beim Start ab.
 *
 * Loesung (Kopf = alles, was das Original direkt erreicht, liegt vorn):
 *   SN_HDR      Kopfdaten {Magic, Laenge, CRC-32, Weichen}, von veneer.py nach dem Linken eingetragen.
 *   sn_guard    init_array (0xAE80C) zeigt hierher: CRC ueber den ganzen angehaengten Code pruefen.
 *                 ok  → Merker 0, sl_init() wie bisher.
 *                 bad → Merker SN_BAD, Original-Init (ParamDefs_ctor 0x271D1) + RAM-Puffer mit Original-Inhalt fuellen.
 *   SN_VENEERS  Weichen (je 24 Byte, von veneer.py erzeugt): jede Code-Patchstelle zeigt auf eine Weiche:
 *                 ldr ip,=SN_FLAG; ldr ip,[ip]; cmp ip,#SN_BAD; beq.w <Original-Ziel>; b.w <unser Code>
 *   sn_gen      Generator-Methode der Randomize-Listen (NIS_VT): bad → Wert unveraendert zurueck.
 *   Listen NIS_LIST/AUD_LIST + Namen liegen ebenfalls im Kopf (nis.c, Abschnitte .head.*).
 * Merker: nur der Wert SN_BAD fuehrt ins Original; leerer/zufaelliger RAM = normal (Tests, Kaltstart vor sn_guard).
 * Grenze: Ist der Kopf selbst beschaedigt, hilft nur der Not-Flash (kurz Reset waehrend des Betriebs).
 * RAM: SN_FLAG 0x1FFF9A38 (frei zwischen rx_t 0x1FFF9A08–37 und rs_t 0x1FFF9A40).
 */
#include <stdint.h>

#define SN_MAGIC   0x564E4553u                    /* "SENV" */
#define SN_BAD     0xA5A5A5A5u                    /* als Thumb-2-Immediate kodierbar (0xXYXYXYXY) */
#define SN_NVEN    52u   /* 17.11.: 80 → 52 (43 belegt), Code muss unter 0xC3000 bleiben (Bootloader schreibt darueber nicht) */
#define SN_VSIZE   24u
#define BASE       0x000B069Cu

typedef struct { uint32_t magic, len, crc, nven; } sn_hdr_t;

#ifdef HOST_TEST
uint32_t host_sn_flag;
#define SN_FLAG    host_sn_flag
#else
#define SN_FLAG    (*(volatile uint32_t *)0x1FFF9A38u)
#endif

__attribute__((used, section(".head.hdr"), aligned(4)))
const volatile sn_hdr_t SN_HDR = { SN_MAGIC, 0u, 0u, 0u };          /* Laenge/CRC traegt veneer.py ein */

__attribute__((used, section(".head.ven"), aligned(4)))
const volatile uint8_t SN_VENEERS[SN_NVEN * SN_VSIZE] = { 0 };       /* Weichen traegt veneer.py ein */

/* CRC-32 (IEEE, reflektiert), ohne Tabelle; das Feld crc selbst zaehlt als 0 */
__attribute__((section(".head.text")))
uint32_t sn_crc(const volatile uint8_t *p, uint32_t n, uint32_t skip) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = (i >= skip && i < skip + 4u) ? 0u : p[i];
        c ^= b;
        for (uint32_t k = 0; k < 8u; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

#ifndef HOST_TEST
void sl_init(void);

/* Original-Inhalte fuer die RAM-Ziele der Datenpatches */
__attribute__((section(".head.data"), aligned(4)))
static const uint32_t SN_NAMES[32] = {           /* Delay (0xA86B4…) dann Reverb (0xA8E34…): Original-Namenszeiger */
    0xa6078, 0xa6080, 0xa6090, 0xa60a0, 0xa60ac, 0xa60bc, 0xa60c4, 0xa60d4, 0x9d3fc, 0x9d404, 0x9d410, 0x9d41c, 0x9d428, 0x9d434, 0x9d43c, 0x9d44c,
    0xa5fbc, 0xa5fc8, 0xa5fd0, 0xa5fdc, 0xa5fec, 0xa5ff8, 0xa6004, 0xa6010, 0xa6018, 0xa6020, 0xa602c, 0xa603c, 0xa6044, 0xa6054, 0xa605c, 0xa606c };

__attribute__((section(".head.text")))
static void sn_stock(void) {
    ((void (*)(void))0x000271D1u)();                                /* Original init_array-Eintrag (ParamDefs_ctor) */
    for (uint32_t i = 0; i < 32u; i++) {                            /* Effektnamen: Puffer 0x1FFFB400 + 16·i (16 B) */
        const volatile char *src = (const volatile char *)SN_NAMES[i];
        volatile char *dst = (volatile char *)(0x1FFFB400u + 16u * i);
        uint32_t k = 0;
        for (; k < 15u && src[k]; k++) dst[k] = src[k];
        for (; k < 16u; k++) dst[k] = 0;
    }
    volatile uint8_t *l = (volatile uint8_t *)0x1FFFD370u;          /* USB-Deskriptorliste (Literal 0x131AC → um_t.list) */
    const volatile uint8_t *f = (const volatile uint8_t *)0x0009BAC0u;
    for (uint32_t i = 0; i < 0x6Cu; i++) l[i] = f[i];
    *(volatile uint32_t *)(0x1FFFD370u + 16u) = 0x1FFF1DD8u;        /* Eintrag 1 (Konfiguration) wie im Original */
    *(volatile uint16_t *)(0x1FFFD370u + 20u) = 0x00B5u;
    for (uint32_t i = 0; i < 0x40u; i += 4u) *(volatile uint32_t *)(0x1FFFB900u + i) = 0;   /* NIS-Namen (Randomize) leer */
}

__attribute__((used, section(".head.text")))
void sn_guard(void) {
    const volatile sn_hdr_t *h = &SN_HDR;
    uint32_t len = h->len;
    int ok = h->magic == SN_MAGIC && len >= sizeof(sn_hdr_t) && len <= 0x4F964u
             && sn_crc((const volatile uint8_t *)BASE, len, (uint32_t)((uintptr_t)&h->crc - BASE)) == h->crc;
    if (ok) { SN_FLAG = 0u; sl_init(); return; }
    SN_FLAG = SN_BAD;
    sn_stock();
}

/* Generator-Methode (NIS_VT[0]): r0 gen, r1 Tripel, r2 Kandidat, r3 Original; s16/r7 wie nis_gen_entry */
__attribute__((naked, used, section(".head.text")))
void sn_gen(void) {
    __asm volatile (
        "ldr   ip, =0x1FFF9A38\n"
        "ldr   ip, [ip]\n"
        "cmp   ip, #0xA5A5A5A5\n"
        "bne   1f\n"
        "mov   r0, r3\n"                      /* beschaedigt: Wert unveraendert */
        "bx    lr\n"
        "1: b.w nis_gen_entry\n"
        ".ltorg\n");
}
#endif

int sn_mod_ok(void) { return SN_FLAG != SN_BAD; }
