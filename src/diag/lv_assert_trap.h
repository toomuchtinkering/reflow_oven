#pragma once

/* LVGL assert trap.
 *
 * Pulled in by LVGL's lv_assert.h through CONFIG_LV_ASSERT_HANDLER_INCLUDE.
 * That include sits *after* lv_conf_internal.h, which has already defined
 * LV_ASSERT_HANDLER to its default bare `while(1);` — hence the #undef here.
 * Redefining from a header rather than a -D flag is deliberate: the handler
 * has to expand to a complete statement, semicolon included, and `;` starts a
 * comment in platformio.ini.
 *
 * Why it exists: LV_USE_ASSERT_MALLOC and LV_USE_ASSERT_NULL are both on while
 * LV_USE_LOG is off, so every LVGL assert spins forever inside the LVGL task —
 * still holding the LVGL lock — and prints nothing. The UI supervisor already
 * catches that and reboots the oven, but it cannot say *which* assert fired.
 * The trap records the caller's return address, which addr2line maps back to
 * the exact LVGL file and line. */

#ifdef __cplusplus
extern "C" {
#endif

void lv_assert_trap(void);

#undef  LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER lv_assert_trap();

#ifdef __cplusplus
}
#endif
