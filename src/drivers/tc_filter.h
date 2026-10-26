/* Median-of-TC_FILTER_WINDOW_N filter over successive thermocouple reads.
 * Pure logic (host-testable). Push one sample per control tick; faulted
 * reads must not be pushed — reset instead, so pre-fault samples never
 * blend with post-fault ones. The window is seeded with the first sample
 * after reset, so push #1 passes through unchanged. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

typedef struct {
    float   buf[TC_FILTER_WINDOW_N];   /* ring of the last N samples */
    uint8_t idx;                       /* next write slot */
    bool    seeded;
} tc_filter_t;

void  tc_filter_reset(tc_filter_t *f);
float tc_filter_push(tc_filter_t *f, float sample);
