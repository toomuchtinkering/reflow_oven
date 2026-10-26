#pragma once

/* Outcome of the boot-time OTA validation decision, cached once in app_main
 * so the LVGL UI tick and the control task never re-read OTA flash state on
 * every tick (finding #40). Written exactly once during boot, then read from
 * other tasks — the storage is atomic so the cross-task hand-off is safe
 * without an explicit barrier (finding #5). */
typedef enum {
    BOOT_VALIDATION_NONE = 0,  /* normal boot — running image is not pending-verify */
    BOOT_VALIDATION_PENDING,   /* pending-verify image + self-tests passed          */
    BOOT_VALIDATION_FAILED,    /* pending-verify image + self-tests failed          */
} boot_validation_t;

/* Set ONCE during the app_main boot path, before control_task / UI consume it. */
void boot_status_set_validation(boot_validation_t v);

/* Read from anywhere (UI tick, control task init, etc.). */
boot_validation_t boot_status_validation(void);
