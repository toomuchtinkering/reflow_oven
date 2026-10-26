#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A one-slot crash breadcrumb held in RTC memory, which survives a software
 * reset, a panic and a watchdog reset.
 *
 * It exists because this board has no retrievable post-mortem otherwise: the
 * partition table is full (ota_1 ends exactly at 0x400000), so coredump-to-
 * flash cannot be enabled without a serial reflash, and panic output goes to
 * UART0 — which is unattached on an OTA-flashed oven running on WiFi. Without
 * a breadcrumb, switching the watchdog on would only turn a mysterious freeze
 * into a mysterious reboot. The record is read back over /version. */

/* How often the LVGL task refreshes the rolling pool sample. Rate-limited
 * rather than run every UI tick because lv_mem_monitor walks every block in
 * the pool; 1 Hz is plenty of resolution for a ratchet that moves over
 * minutes, and it bounds the staleness of the sample folded into a record. */
#define CRASH_LOG_POOL_SAMPLE_MS  1000u

typedef enum {
    CRASH_LOG_NONE      = 0,
    CRASH_LOG_UI_STALL  = 1,  /* UI heartbeat stopped; control_task rebooted us */
    CRASH_LOG_LV_ASSERT = 2,  /* an LVGL assert fired; lv_assert_trap rebooted us */
} crash_log_reason_t;

typedef struct {
    uint32_t reason;            /* crash_log_reason_t                          */
    uint32_t uptime_ms;         /* uptime when the fault was detected          */
    uint32_t stalled_ms;        /* UI_STALL only: how long the beat was frozen */
    uint32_t assert_pc;         /* LV_ASSERT only: normalised call-site PC     */

    /* Crash-path pool probe. Only meaningful for UI_STALL, and only when the
     * lock could be taken — which is the discriminating bit: false means the
     * UI task was stuck *inside* LVGL still holding the mutex. */
    uint32_t lvgl_lock_taken;
    uint32_t lv_free, lv_biggest, lv_used_pct, lv_frag_pct;

    /* Last rolling sample taken from inside the LVGL task, copied in at fault
     * time so it survives and cannot be overwritten once the UI restarts.
     * This is the pool state from shortly *before* the fault — the number the
     * crash-path probe cannot get when the lock is held. pool_seq stops
     * advancing exactly when the UI does. */
    uint32_t pool_seq, pool_age_ms;
    uint32_t pool_free, pool_biggest, pool_used_pct, pool_frag_pct;

    uint32_t free_heap, min_free_heap;

    /* Which boot wrote this record. Compared against crash_log_boot_count()
     * to tell a fresh breadcrumb from an old one: the record is deliberately
     * not cleared on read, so without this a fault from days ago looks exactly
     * like the reboot that just happened. On 2026-09-01 that cost a debugging
     * session a false alarm, disambiguated only by squinting at uptime_ms. */
    uint32_t boot_count;
} crash_log_t;

/* Refresh the rolling pool sample. MUST be called from the LVGL task (or with
 * the LVGL lock held) — it reads the allocator directly and takes no lock of
 * its own. Self-rate-limits to CRASH_LOG_POOL_SAMPLE_MS. */
void crash_log_sample_pool(uint32_t uptime_ms);

/* Fill the record and leave it in RTC memory. Neither resets — the caller
 * decides. crash_log_record_ui_stall is safe to call from another task: its
 * LVGL probe uses a bounded lock attempt and records failure rather than
 * blocking. */
void crash_log_record_ui_stall(uint32_t uptime_ms, uint32_t stalled_ms);
void crash_log_record_lv_assert(uint32_t pc);

/* Advance the boot counter. MUST be called once, early in app_main, before
 * anything can read or write a record — crash_log_read's result is only
 * interpretable relative to the count this establishes.
 *
 * It also hardens the cold-boot case. RTC memory is undefined after a power
 * cycle, so the record's magic could in principle come up valid by chance and
 * present garbage as a real fault. A cold boot is detected here (the counter's
 * own magic is absent) and clears the record outright rather than trusting a
 * single 32-bit coincidence. */
void crash_log_boot(void);

/* Boots since the counter was established; 1 on the first boot after a cold
 * power-on. Zero if crash_log_boot() has not run. */
uint32_t crash_log_boot_count(void);

/* True if a record survived the last reset. Contents are undefined after a
 * cold power-on, which the magic guards against. Compare out->boot_count with
 * crash_log_boot_count() to judge staleness: equal-minus-one means the fault
 * caused the reboot we are running from now. */
bool crash_log_read(crash_log_t *out);

void crash_log_clear(void);
