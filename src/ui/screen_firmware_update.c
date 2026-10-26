#include "ui/screen_firmware_update.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "storage/nvs_store.h"
#include "config.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_token_lbl = NULL;

static void refresh_token(void) {
    char t[OTA_TOKEN_HEX_LEN + 1];
    if (nvs_store_get_ota_token(t)) lv_label_set_text(s_token_lbl, t);
    else                            lv_label_set_text(s_token_lbl, "(no token)");
}

static void do_regen(void) {
    uint8_t raw[16];
    esp_fill_random(raw, sizeof raw);
    char t[OTA_TOKEN_HEX_LEN + 1];
    for (int i = 0; i < 16; i++) snprintf(t + i*2, 3, "%02x", raw[i]);
    nvs_store_set_ota_token(t);
    refresh_token();
}

static void regen_clicked(lv_event_t *e) {
    (void)e;
    /* Spec §9.3 mandates a confirmation: the old token stops working
     * immediately and upload.conf must be updated (#34). */
    settings_confirm("Regenerate token?",
                     "The current token will stop working. Update upload.conf afterwards.",
                     do_regen, NULL);
}

lv_obj_t *screen_firmware_update_create(void) {
    lv_obj_t *sub = settings_make_subscreen("Firmware update");
    lv_obj_t *info = lv_label_create(sub);
    lv_label_set_text(info,
        "Upload via:\n"
        "  http://reflow.local/ota\n"
        "with X-OTA-Token header.");
    lv_obj_set_pos(info, 8, 44);
    lv_obj_set_style_text_color(info, THEME_FG, 0);

    lv_obj_t *tlbl = lv_label_create(sub);
    lv_label_set_text(tlbl, "OTA token:");
    lv_obj_set_pos(tlbl, 8, 110);
    lv_obj_set_style_text_color(tlbl, THEME_FG, 0);

    s_token_lbl = lv_label_create(sub);
    lv_obj_set_pos(s_token_lbl, 8, 126);
    lv_obj_set_width(s_token_lbl, LV_HOR_RES - 16);
    lv_label_set_long_mode(s_token_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_token_lbl, THEME_ACCENT, 0);
    refresh_token();

    lv_obj_t *regen = lv_button_create(sub);
    lv_obj_set_size(regen, 140, 32); lv_obj_set_pos(regen, 8, 190);
    lv_obj_set_style_bg_color(regen, THEME_STOP, 0);
    lv_obj_add_event_cb(regen, regen_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(regen); lv_label_set_text(rl, "Regenerate"); lv_obj_center(rl);

    return sub;
}
