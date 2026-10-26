#pragma once
#include "lvgl.h"

/* Single source of truth for UI colors. Per spec §7. */
#define THEME_BG        lv_color_hex(0x0f1115)  /* near-black charcoal */
#define THEME_PANEL_BG  lv_color_hex(0x14171c)  /* slightly lighter charcoal */
#define THEME_BORDER    lv_color_hex(0x232833)  /* dark slate-grey */
#define THEME_FG        lv_color_hex(0xe6e6e6)  /* off-white */
#define THEME_FG_DIM    lv_color_hex(0x9aa0a6)  /* mid grey */
#define THEME_FG_FAINT  lv_color_hex(0x6a6f78)  /* darker grey */
#define THEME_ACCENT    lv_color_hex(0xff8c42)  /* warm orange */
#define THEME_STAGE     lv_color_hex(0xfbbc04)  /* amber/yellow */
#define THEME_GO        lv_color_hex(0x1f7a3f)  /* deep green */
#define THEME_STOP      lv_color_hex(0x7a1f1f)  /* deep red */
#define THEME_INFO      lv_color_hex(0x1f5f7a)  /* deep teal-blue */
#define THEME_OK        lv_color_hex(0x69b87a)  /* light green */
#define THEME_MODAL_BG  lv_color_hex(0x181b21)  /* slightly raised charcoal (modal popups) */
#define THEME_BTN_NEUTRAL lv_color_hex(0x232833)  /* neutral key/button fill — same as THEME_BORDER */

/* Dimming for a disabled button. Every button here sets its bg_color as a
 * LOCAL style at LV_STATE_DEFAULT, and a local style outranks a theme style
 * whatever the state — so the theme's disabled appearance never applies and a
 * blocked button keeps its full-strength colour, reading as broken rather than
 * blocked. opa is used instead of a disabled bg_color for two reasons:
 * lv_obj_get_style_opa_recursive() multiplies down the parent chain, so the
 * button and its child text label dim together; and it composes with HOME's
 * per-mode recolouring (GO/STOP/STAGE) rather than needing to be re-asserted
 * at each of those sites. */
#define THEME_OPA_DISABLED  LV_OPA_40

void theme_apply_to_screen(lv_obj_t *scr);
