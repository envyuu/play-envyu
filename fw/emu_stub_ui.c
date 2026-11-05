/* Nur fuer emu_ui.py (Emulator), NICHT Teil des Images. Wie emu_stub.c, zusaetzlich:
 *   fx_process schreibt seinen EINGANG nach CAP + id*0x100 (Delay/Reverb-Send pruefen),
 *   in_cb ist ein Zusatzeingang wie ihn die Firmware ueber AudioMixer_setInputCallback setzen koennte. */
#include <stdint.h>
#define MIX   ((void *)0x20020000)
#define CNT   (*(volatile uint32_t *)0x20021F00)
#define CAP   ((int16_t *)0x20021800)
#define INCB_BUF 0x20021A00u
#define INCB_CTX (*(volatile uint32_t *)0x20021F08)

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
    uint32_t c = CNT, id = self[1];
    for (int k = 0; k < n; k++) {
        CAP[id * 128u + (uint32_t)k] = in[k];
        uint32_t f = c + (uint32_t)k;
        outL[k] = (int16_t)((int32_t)((f * 13u + id * 977u) & 0x1FFFu) - 0x1000);
        outR[k] = (int16_t)((int32_t)((f * 29u + id * 3331u + 7u) & 0x1FFFu) - 0x1000);
    }
}

__attribute__((section(".text.c")))
void in_cb(int32_t *out, uint32_t ctx) {
    INCB_CTX = ctx;
    out[0] = 2;
    out[1] = (int32_t)INCB_BUF;
}
