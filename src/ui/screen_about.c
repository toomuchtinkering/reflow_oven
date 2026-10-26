#include "ui/screen_about.h"
#include "ui/screen_settings.h"
#include "ui/theme.h"
#include "version.h"
#include "control/control_task.h"   /* control_begin_ota / control_end_ota */
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"  /* esp_app_get_description() — build-time date/time (#57) */
#include "esp_heap_caps.h"
#include "esp_system.h"    /* esp_restart */
#include <math.h>
#include <stdio.h>

static lv_obj_t *s_sub = NULL;
static lv_obj_t *s_temp_label = NULL;

/* Other-slot info, probed once at screen creation. The partition pointer
 * stays valid forever (the partition table is a flash-resident registry);
 * validity can only change via an OTA upload, which always ends in a reboot,
 * so a creation-time snapshot can't go stale. */
static const esp_partition_t *s_other_part = NULL;
static char s_other_version[34] = "";

static lv_obj_t *mklabel(lv_obj_t *parent, const char *txt, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, 8, y);
    lv_obj_set_style_text_color(l, THEME_FG, 0);
    return l;
}

/* Confirmed "Boot other slot": guard against a running reflow with the same
 * reservation the OTA upload uses, point the bootloader at the other slot
 * (this validates the image), then reboot. */
static void slot_switch_confirmed(void) {
    oven_state_t snap;
    control_get_snapshot(&snap);
    /* Pending validation is exactly the state this fallback is for, but the
     * idle reservation below would refuse (mode != IDLE). No reservation is
     * needed: heating is impossible while pending, and the running image was
     * never marked valid, so a plain reboot already rolls back to the other
     * slot via the bootloader (review M3). */
    if (snap.mode == MODE_PENDING_VALIDATION) {
        esp_restart();
        return;
    }
    if (snap.mode == MODE_FAULT) {
        settings_info("Fault latched", "Acknowledge the fault first.");
        return;
    }
    if (!control_begin_ota()) {
        settings_info("Oven busy", "Stop the run first.");
        return;
    }
    if (esp_ota_set_boot_partition(s_other_part) != ESP_OK) {
        control_end_ota();
        settings_info("Switch failed", "The other slot's image did not validate.");
        return;
    }
    /* Reservation intentionally not released — we reboot into the other image,
     * which resets all state (same rationale as the OTA upload path). */
    esp_restart();
}

static void slot_switch_clicked_cb(lv_event_t *e) {
    (void)e;
    char msg[128];
    snprintf(msg, sizeof msg, "Reboot into firmware %s on slot %s?",
             s_other_version, s_other_part ? s_other_part->label : "?");
    settings_confirm("Boot other slot", msg, slot_switch_confirmed, NULL);
}

lv_obj_t *screen_about_create(void) {
    lv_obj_t *sub = settings_make_subscreen("About");
    int y = 44;
    char buf[80];

    mklabel(sub, "Firmware", y); y += 16;
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(buf, sizeof buf, "  Version  %s", FIRMWARE_VERSION_STR);  mklabel(sub, buf, y); y += 16;
    snprintf(buf, sizeof buf, "  Built    %s %s", desc->date, desc->time); mklabel(sub, buf, y); y += 24;

    mklabel(sub, "OTA slot", y); y += 16;
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    snprintf(buf, sizeof buf, "  Active   %s", run ? run->label : "?"); mklabel(sub, buf, y); y += 16;
    snprintf(buf, sizeof buf, "  Status   %s",
             st == ESP_OTA_IMG_VALID ? "valid" :
             st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" : "unknown"); mklabel(sub, buf, y); y += 16;

    /* Other-slot description (may be empty if never flashed) */
    esp_app_desc_t other = {0};
    bool other_ok = next && esp_ota_get_partition_description(next, &other) == ESP_OK;
    if (other_ok) {
        s_other_part = next;
        snprintf(s_other_version, sizeof s_other_version, "%s", other.version);
        snprintf(buf, sizeof buf, "  Other    %s", other.version);
    } else {
        snprintf(buf, sizeof buf, "  Other    (empty)");
    }
    mklabel(sub, buf, y); y += 24;

    mklabel(sub, "Hardware", y); y += 16;
    mklabel(sub, "  Board    LCDwiki E32R35T", y); y += 16;
    mklabel(sub, "  Chip     ESP32-WROOM-32E", y); y += 16;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sizeof buf, "  MAC      %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    mklabel(sub, buf, y); y += 16;
    /* The About sub-screen is created once and cached, so these values are a
     * boot/first-visit snapshot. Label the heap reading accordingly rather
     * than implying it's live (#36). */
    snprintf(buf, sizeof buf, "  Free heap (at boot)  %lu KB",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT)/1024);
    mklabel(sub, buf, y); y += 16;
    /* Live electronics temperature (MAX31855 cold junction), refreshed by
     * screen_about_update while this sub-screen is visible. */
    s_temp_label = mklabel(sub, "  TC board  --", y); y += 24;

    /* Boot-other-slot fallback (spec 2026-06-10 §3): recover from a bad OTA
     * image without a working web path. Styled like the settings card rows. */
    lv_obj_t *btn = lv_button_create(sub);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_set_pos(btn, 0, y);
    lv_obj_set_style_bg_color(btn, THEME_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, THEME_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, THEME_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_t *bl = lv_label_create(btn);
    if (other_ok) {
        snprintf(buf, sizeof buf, LV_SYMBOL_REFRESH "  Boot other slot (%s)", s_other_version);
        lv_label_set_text(bl, buf);
        lv_obj_set_style_text_color(bl, THEME_FG, 0);
        lv_obj_add_event_cb(btn, slot_switch_clicked_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_label_set_text(bl, LV_SYMBOL_REFRESH "  Boot other slot (empty)");
        lv_obj_set_style_text_color(bl, THEME_FG_FAINT, 0);
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
    lv_obj_center(bl);

    s_sub = sub;
    return sub;
}

void screen_about_update(const oven_state_t *snap) {
    if (!s_sub || lv_obj_has_flag(s_sub, LV_OBJ_FLAG_HIDDEN)) return;
    /* Skip the label realloc when the displayed (0.1 °C-rounded) value is
     * unchanged — this runs at 4 Hz on a heap-tight device. */
    static float s_last_shown = -1000.0f;
    float cj = snap->cj_temp_c;
    if (isnan(cj)) return;            /* keep "--" until the first reading */
    float rounded = (float)((int)(cj * 10.0f + (cj >= 0 ? 0.5f : -0.5f))) / 10.0f;
    if (rounded == s_last_shown) return;
    s_last_shown = rounded;
    char buf[40];
    snprintf(buf, sizeof buf, "  TC board  %.1f °C", (double)rounded);
    lv_label_set_text(s_temp_label, buf);
}
