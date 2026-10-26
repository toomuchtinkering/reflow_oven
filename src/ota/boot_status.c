#include "ota/boot_status.h"
#include <stdatomic.h>

/* Written once by app_main, read from the LVGL + control tasks. Atomic so the
 * write is visible to the reading tasks without a separate memory barrier
 * (finding #5). */
static _Atomic boot_validation_t s_validation = BOOT_VALIDATION_NONE;

void boot_status_set_validation(boot_validation_t v) { atomic_store(&s_validation, v); }
boot_validation_t boot_status_validation(void)       { return atomic_load(&s_validation); }
