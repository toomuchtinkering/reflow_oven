#include "ui/screen_audio.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "audio/audio_task.h"

static lv_obj_t *s_mute_sw = NULL;

static void mute_changed_cb(lv_event_t *e) {
    (void)e;
    audio_set_muted(lv_obj_has_state(s_mute_sw, LV_STATE_CHECKED));
}

lv_obj_t *screen_audio_create(void) {
    lv_obj_t *sub = settings_make_subscreen("Audio");
    lv_obj_t *lbl = lv_label_create(sub);
    lv_label_set_text(lbl, "Mute notifications");
    lv_obj_set_pos(lbl, 8, 48);
    lv_obj_set_style_text_color(lbl, THEME_FG, 0);

    s_mute_sw = lv_switch_create(sub);
    lv_obj_set_pos(s_mute_sw, LV_HOR_RES - 80, 44);
    /* Dark-theme the switch — the default LVGL look is a near-white rounded
     * track that reads as an out-of-place box on the black UI. Off = dark
     * track; on = accent fill; knob = off-white. */
    lv_obj_set_style_bg_color(s_mute_sw, THEME_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_mute_sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mute_sw, THEME_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_mute_sw, THEME_FG, LV_PART_KNOB);
    if (audio_is_muted()) lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_mute_sw, mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sub;
}
