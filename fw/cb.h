#pragma once
#include <stdint.h>
typedef struct {
    int32_t mode;      /* 0 Normal, 1 Red-Green, 2 Blue-Yellow (Menuewert, EnumMenuItem schreibt int32) */
    int32_t saved;     /* Stand der Datei */
    int32_t seen;      /* zuletzt gesehen (Speicherverzoegerung) */
} cb_t;
void cb_map(uint32_t mode, uint32_t *r, uint32_t *g, uint32_t *b);
void cb_pixel(void *obj, uint32_t i, uint32_t r, uint32_t g, uint32_t b);
