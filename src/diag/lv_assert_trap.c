#include "diag/lv_assert_trap.h"
#include "diag/crash_log.h"

#include "esp_system.h"
#include <stdint.h>

/* noinline so __builtin_return_address(0) is genuinely the LVGL call site and
 * not something the optimiser folded away. */
void __attribute__((noinline)) lv_assert_trap(void) {
    uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);

    /* Xtensa windowed ABI: the top two bits of a return address hold the
     * register-window increment, not address bits. Clear them and restore the
     * code-region bits, the same normalisation esp_cpu_process_stack_pc does,
     * so the value resolves. Instruction code lives at 0x4xxxxxxx on this
     * part, both IRAM and flash-mapped IROM.
     *
     * Resolve with:
     *   xtensa-esp32-elf-addr2line -pfia -e .pio/build/esp32dev-ota/firmware.elf <pc>
     * The address is the instruction *after* the call, so it can land on the
     * following line; check the line above if the answer looks off by one. */
    pc = (pc & 0x3FFFFFFFu) | 0x40000000u;

    /* Deliberately does not walk the LVGL pool here. This runs inside the LVGL
     * task with the lock held, so a live lv_mem_monitor() would be tempting —
     * but an assert can fire while the allocator is mid-mutation, and walking a
     * half-updated TLSF pool could hang or fault, replacing a clean record with
     * a bare panic. The rolling sample folded into the record by crash_log is
     * at most CRASH_LOG_POOL_SAMPLE_MS old, which is ample for a ratchet that
     * moves over minutes. */
    crash_log_record_lv_assert(pc);
    esp_restart();
}
