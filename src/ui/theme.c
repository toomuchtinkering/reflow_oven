#include "ui/theme.h"

void theme_apply_to_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, THEME_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, THEME_FG, 0);
}
