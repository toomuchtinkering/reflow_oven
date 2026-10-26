#include "drivers/tc_filter.h"

_Static_assert(TC_FILTER_WINDOW_N == 3, "median below is hand-rolled for N=3");

void tc_filter_reset(tc_filter_t *f) {
    f->idx = 0;
    f->seeded = false;
}

float tc_filter_push(tc_filter_t *f, float sample) {
    if (!f->seeded) {
        for (uint8_t i = 0; i < TC_FILTER_WINDOW_N; i++) f->buf[i] = sample;
        f->seeded = true;
    } else {
        f->buf[f->idx] = sample;
        f->idx = (uint8_t)((f->idx + 1) % TC_FILTER_WINDOW_N);
    }
    float a = f->buf[0], b = f->buf[1], c = f->buf[2];
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return b;
}
