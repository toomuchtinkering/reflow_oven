#include "diag/crash_log.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

/* Bounded on purpose. The whole reason we are here is that the UI task may be
 * wedged holding this lock forever; an unbounded wait would hang the caller
 * (control_task, which is itself watchdogged at CONTROL_TASK_WDT_S) and turn a
 * recoverable reboot into a second, less informative panic. A failed take is
 * not an error to hide — it is the most useful single bit in the record. */
#define CRASH_LOG_LOCK_MS  100

#define CRASH_LOG_MAGIC    0xC0FFEE01u
#define POOL_MAGIC         0xC0FFEE02u
#define BOOT_MAGIC         0xC0FFEE03u

/* RTC_NOINIT_ATTR: not zeroed by the startup code, and retained across
 * software/panic/watchdog resets. Undefined after a cold power-on, hence the
 * magics. Each magic is written after its payload so a torn write cannot
 * validate itself. */
static RTC_NOINIT_ATTR uint32_t    s_magic;
static RTC_NOINIT_ATTR crash_log_t s_rec;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    uint32_t free, biggest, used_pct, frag_pct;
} pool_sample_t;

static RTC_NOINIT_ATTR uint32_t      s_pool_magic;
static RTC_NOINIT_ATTR pool_sample_t s_pool;

/* Monotonic across warm resets, restarted by a cold power-on — which is
 * exactly the lifetime of the record it stamps. */
static RTC_NOINIT_ATTR uint32_t s_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_boot_count;

void crash_log_boot(void) {
    if (s_boot_magic == BOOT_MAGIC) {
        s_boot_count++;
        return;
    }
    /* Cold power-on: every RTC variable above is undefined, so nothing that
     * survived is trustworthy. Clear rather than rely on the record's own
     * magic alone — see the header. */
    s_boot_count = 1;
    s_pool_magic = 0;
    crash_log_clear();
    s_boot_magic = BOOT_MAGIC;   /* written last, like every magic here */
}

uint32_t crash_log_boot_count(void) {
    return (s_boot_magic == BOOT_MAGIC) ? s_boot_count : 0u;
}

void crash_log_sample_pool(uint32_t uptime_ms) {
    if (s_pool_magic == POOL_MAGIC
        && (uint32_t)(uptime_ms - s_pool.uptime_ms) < CRASH_LOG_POOL_SAMPLE_MS) {
        return;
    }

    lv_mem_monitor_t m;
    lv_mem_monitor(&m);   /* caller holds the LVGL lock; see the header */

    uint32_t seq = (s_pool_magic == POOL_MAGIC) ? s_pool.seq + 1u : 1u;
    s_pool = (pool_sample_t){
        .seq       = seq,
        .uptime_ms = uptime_ms,
        .free      = (uint32_t)m.free_size,
        .biggest   = (uint32_t)m.free_biggest_size,
        .used_pct  = (uint32_t)m.used_pct,
        .frag_pct  = (uint32_t)m.frag_pct,
    };
    s_pool_magic = POOL_MAGIC;
}

/* Fold the rolling sample into a record being built. */
static void attach_pool_sample(crash_log_t *r, uint32_t uptime_ms) {
    if (s_pool_magic != POOL_MAGIC) return;
    r->pool_seq      = s_pool.seq;
    r->pool_age_ms   = uptime_ms - s_pool.uptime_ms;
    r->pool_free     = s_pool.free;
    r->pool_biggest  = s_pool.biggest;
    r->pool_used_pct = s_pool.used_pct;
    r->pool_frag_pct = s_pool.frag_pct;
}

static void commit(const crash_log_t *r) {
    s_rec = *r;
    s_rec.boot_count = crash_log_boot_count();  /* stamp here, not at each call site */
    s_magic = CRASH_LOG_MAGIC;   /* written last: a torn record stays invalid */
}

void crash_log_record_ui_stall(uint32_t uptime_ms, uint32_t stalled_ms) {
    crash_log_t r = {
        .reason        = CRASH_LOG_UI_STALL,
        .uptime_ms     = uptime_ms,
        .stalled_ms    = stalled_ms,
        .free_heap     = (uint32_t)esp_get_free_heap_size(),
        .min_free_heap = (uint32_t)esp_get_minimum_free_heap_size(),
    };

    if (lvgl_port_lock(CRASH_LOG_LOCK_MS)) {
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        lvgl_port_unlock();
        r.lvgl_lock_taken = 1;
        r.lv_free     = (uint32_t)m.free_size;
        r.lv_biggest  = (uint32_t)m.free_biggest_size;
        r.lv_used_pct = (uint32_t)m.used_pct;
        r.lv_frag_pct = (uint32_t)m.frag_pct;
    }

    attach_pool_sample(&r, uptime_ms);
    commit(&r);
}

void crash_log_record_lv_assert(uint32_t pc) {
    /* Commit the irreplaceable part FIRST. An assert can fire because memory
     * is already corrupt, and esp_get_free_heap_size() walks the heap — if
     * that faults, everything after it is lost. Since the magic is written
     * last by design (so a torn record cannot validate itself), a fault during
     * enrichment used to leave present:false and no evidence at all. Now the
     * reason and the PC are durable before anything that can fault runs, and
     * the extras only upgrade a record that already exists. */
    crash_log_t r = {
        .reason    = CRASH_LOG_LV_ASSERT,
        .assert_pc = pc,
    };
    commit(&r);

    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    r.uptime_ms     = uptime_ms;
    attach_pool_sample(&r, uptime_ms);
    commit(&r);

    /* Heap queries last: the most likely thing to fault, the least essential. */
    r.free_heap     = (uint32_t)esp_get_free_heap_size();
    r.min_free_heap = (uint32_t)esp_get_minimum_free_heap_size();
    commit(&r);
}

bool crash_log_read(crash_log_t *out) {
    if (s_magic != CRASH_LOG_MAGIC || s_rec.reason == CRASH_LOG_NONE) return false;
    if (out) *out = s_rec;
    return true;
}

void crash_log_clear(void) {
    s_magic = 0;
    s_rec.reason = CRASH_LOG_NONE;
}
