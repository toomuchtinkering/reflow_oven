#pragma once
#include "lvgl.h"

lv_obj_t *status_bar_create(lv_obj_t *parent);
void      status_bar_update(void);  /* called from 250 ms UI tick */
