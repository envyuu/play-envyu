/* NUR FUER DEN EMULATOR-TEST (fx_rectest.bin, nicht im .ppf): Attrappe von usbrec.c (Patch 024).
 * Zustand bei 0x1FFF9F60: {st, ds, drops, num, err, n_req, last_on} */
#include <stdint.h>
#define RS ((volatile uint32_t *)0x1FFF9F60u)
int rec_request(int on) { RS[5]++; RS[6] = (uint32_t)on; RS[0] = on ? 2u : 0u; return 0; }
void rec_status(uint32_t *st, uint32_t *ds, uint32_t *drops, uint32_t *num, int32_t *err) {
    *st = RS[0]; *ds = RS[1]; *drops = RS[2]; *num = RS[3]; *err = (int32_t)RS[4];
}
