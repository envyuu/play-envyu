/* Nur fuer emu_mt.py (Emulator), NICHT Teil des Images. Wird bei 0xF0000 in den freien Flash des Emulators gelegt.
 * Bildet das Minimum nach, damit der ECHTE Mixer-Code 0x15884 aus dem ECHTEN audio_callback 0xBA18 heraus laeuft:
 *   App-Objekt (0x1FFF373C zeigt darauf) mit vtable[2] = app_audio:
 *     app_audio ruft das Mixer-Update 0x15884 auf den Mixer-AudioStream bei 0x20020000 auf und wandelt danach
 *     die Master-Summen (0x20001694 / 0x20001894, int32) mit der echten Funktion 0x16F68 nach L/R (int16) –
 *     so wie es der Ausgangsknoten mit den Bloecken des Mixers taete.
 *   fx_process: Attrappe fuer Delay/Reverb (vtable[0] der Send-Effekte), schreibt ein vom Framezaehler abhaengiges Muster.
 */
#include <stdint.h>
#define MIX   ((void *)0x20020000)
#define CNT   (*(volatile uint32_t *)0x20021F00)     /* Framezaehler (vom Python-Teil gesetzt/gelesen) */

typedef void (*upd_t)(void *);
typedef void (*cvt_t)(int16_t *, const int32_t *, uint32_t);

__attribute__((section(".text.a")))
void app_audio(void *self, int16_t *l, int16_t *r, int n) {
    (void)self;
    ((upd_t)0x00015885)(MIX);
    ((cvt_t)0x00016F69)(l, (const int32_t *)0x20001694, 0);
    ((cvt_t)0x00016F69)(r, (const int32_t *)0x20001894, 0);
    CNT += (uint32_t)n;
}

__attribute__((section(".text.b")))
void fx_process(uint32_t *self, const int16_t *in, int16_t *outL, int16_t *outR, int n) {
    (void)in;
    uint32_t c = CNT, id = self[1];
    for (int k = 0; k < n; k++) {
        uint32_t f = c + (uint32_t)k;
        outL[k] = (int16_t)((int32_t)((f * 13u + id * 977u) & 0x1FFFu) - 0x1000);
        outR[k] = (int16_t)((int32_t)((f * 29u + id * 3331u + 7u) & 0x1FFFu) - 0x1000);
    }
}
