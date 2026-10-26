#include "ota/ota_server.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>  /* malloc/free for the OTA chunk buffer */

#ifdef ESP_PLATFORM
#include "storage/nvs_store.h"
#include "storage/nvs_schema.h"   /* pid_gains_t / pid_validate for /suggest-gains */
#include "control/control_task.h"
#include "control/reflow_sm.h"
#include "control/profile_curve.h"   /* PROFILE_CURVE_MAX_SAMPLES, STAGE_COUNT_ for /profile */
#include "diag/crash_log.h"  /* post-mortem breadcrumb — no coredump partition */
#include "ota/telemetry_json.h"
#include "version.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"  /* esp_app_get_description() — build-time date/time */
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"     /* recv stall-timeout clock + one-shot reboot timer */
#include "esp_system.h"    /* esp_get_free_heap_size — /version heap readout */
#include "mbedtls/constant_time.h"
#include "lvgl.h"           /* lv_mem_monitor — LVGL pool is a separate arena */
#include "esp_lvgl_port.h"  /* lvgl_port_lock — LVGL is not thread-safe */
#endif

#ifdef ESP_PLATFORM
static const char *TAG = "ota_server";
static httpd_handle_t s_server = NULL;

/* Routes registered in ota_server_start: /, /version, /ota, /telemetry,
 * /stop, /suggest-gains, /profile. Keep OTA_MAX_URI_HANDLERS >= OTA_ROUTE_COUNT. */
#define OTA_ROUTE_COUNT        7
#define OTA_MAX_URI_HANDLERS   8

/* §1.5 invariant: the server must allow at least as many URI handlers as we
 * register, or httpd_register_uri_handler() silently fails for the overflow
 * route. This guard fails the build if a future route bumps OTA_ROUTE_COUNT
 * past the configured limit without raising OTA_MAX_URI_HANDLERS too. */
_Static_assert(OTA_ROUTE_COUNT <= OTA_MAX_URI_HANDLERS,
               "raise OTA_MAX_URI_HANDLERS to fit all registered routes");

/* One-shot reboot, scheduled off the httpd worker so the handler can return
 * and the response is flushed + the socket closed cleanly before we restart
 * (finding #11). */
static void ota_restart_cb(void *arg) { (void)arg; esp_restart(); }

/* Embedded gzipped tuning SPA — generated C array (src/web/tuning_html_gz.c by
 * tools/build_web_asset.py). Served at GET / with Content-Encoding: gzip. */
extern const unsigned char tuning_html_gz[];
extern const size_t        tuning_html_gz_len;
#endif

int ota_token_eq(const char *a, const char *b, size_t n) {
#ifdef ESP_PLATFORM
    return mbedtls_ct_memcmp(a, b, n) == 0 ? 1 : 0;
#else
    /* Hand-rolled constant-time fallback for host tests. */
    unsigned char acc = 0;
    for (size_t i = 0; i < n; i++) acc |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return acc == 0 ? 1 : 0;
#endif
}

#ifdef ESP_PLATFORM
static void ensure_token(void) {
    char t[OTA_TOKEN_HEX_LEN + 1];
    if (nvs_store_get_ota_token(t)) return;
    uint8_t raw[16];
    esp_fill_random(raw, sizeof raw);
    for (int i = 0; i < 16; i++) snprintf(t + i*2, 3, "%02x", raw[i]);
    nvs_store_set_ota_token(t);
    ESP_LOGI(TAG, "generated fresh OTA token");
}

/* Why the chip last came up. With no coredump partition (the table is full —
 * ota_1 ends exactly at 0x400000) and panic output going only to an unattached
 * UART0, this string plus the crash breadcrumb below is the entire post-mortem
 * available over WiFi. */
static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "poweron";
        case ESP_RST_SW:       return "sw";        /* incl. our UI-stall reboot */
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_WDT:      return "other_wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_EXT:      return "ext";
        default:               return "unknown";
    }
}

static esp_err_t version_handler(httpd_req_t *req) {
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    const esp_app_desc_t *desc = esp_app_get_description();  /* date/time are build-time (#57) */
    /* LVGL pool readout. esp_get_free_heap_size() below covers the ESP heap
     * only; LVGL allocates from its own fixed LV_MEM_SIZE arena (64 KB, no
     * expansion), which is otherwise unobservable from outside the device. An
     * exhausted pool trips LV_ASSERT_MALLOC, and with LV_USE_LOG off that is a
     * bare while(1) in the UI task: the panel freezes silently while this
     * server keeps answering. Exactly the failure the plan's manual step 1.3
     * exists to catch, so it is worth a permanent readout rather than a
     * one-off debug patch.
     *
     * The lock timeout is bounded and short on purpose. LVGL is not
     * thread-safe so the pool cannot be read unlocked, but a UI task wedged
     * inside that while(1) holds the lock forever — an unbounded wait here
     * would hang the HTTP task too and take away the only remaining window
     * into the device. A failed take is therefore not an error to hide: it is
     * itself the evidence that the UI task is stuck, and is reported as such. */
    char lvgl[160];
    if (lvgl_port_lock(LVGL_STAT_LOCK_MS)) {
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        lvgl_port_unlock();
        snprintf(lvgl, sizeof lvgl,
            "{\"ok\":true,\"used_pct\":%u,\"frag_pct\":%u,\"free\":%u,"
            "\"biggest\":%u,\"max_used\":%u,\"total\":%u}",
            (unsigned)m.used_pct, (unsigned)m.frag_pct, (unsigned)m.free_size,
            (unsigned)m.free_biggest_size, (unsigned)m.max_used,
            (unsigned)m.total_size);
    } else {
        snprintf(lvgl, sizeof lvgl, "{\"ok\":false,\"note\":\"ui task did not release the lock\"}");
    }
    /* Crash breadcrumb from a previous boot, if one survived. Reported rather
     * than cleared on read, so a reboot the user did not witness is still
     * visible later; it is overwritten by the next stall and lost on a cold
     * power-on (RTC memory is undefined then). "lvgl_lock" is the useful bit:
     * false means the UI task was stuck inside LVGL still holding the mutex. */
    char crash[480];
    crash_log_t cl;
    if (crash_log_read(&cl)) {
        /* "pool_*" is the rolling sample taken from inside the LVGL task
         * shortly before the fault — the pool state the crash-path probe
         * cannot obtain when the UI is wedged holding the lock. "assert_pc" is
         * only set for reason "lv_assert"; resolve it with
         *   xtensa-esp32-elf-addr2line -pfia -e firmware.elf <pc>
         * against the ELF for the exact version string reported above.
         *
         * "boots_ago" is what makes this record readable. It is deliberately
         * never cleared, so 1 means "the fault that caused the boot you are
         * talking to" and anything higher means the oven has come up cleanly
         * that many times since — a stale breadcrumb, not a live problem. */
        snprintf(crash, sizeof crash,
            "{\"present\":true,\"reason\":\"%s\",\"boot\":%u,\"boots_ago\":%u,"
            "\"uptime_ms\":%u,"
            "\"stalled_ms\":%u,\"assert_pc\":\"0x%08x\",\"lvgl_lock\":%s,"
            "\"lv_free\":%u,\"lv_biggest\":%u,\"lv_used_pct\":%u,\"lv_frag_pct\":%u,"
            "\"pool_seq\":%u,\"pool_age_ms\":%u,\"pool_free\":%u,"
            "\"pool_biggest\":%u,\"pool_used_pct\":%u,\"pool_frag_pct\":%u,"
            "\"free_heap\":%u,\"min_free_heap\":%u}",
            cl.reason == CRASH_LOG_LV_ASSERT ? "lv_assert" : "ui_stall",
            (unsigned)cl.boot_count,
            (unsigned)(crash_log_boot_count() - cl.boot_count),
            (unsigned)cl.uptime_ms, (unsigned)cl.stalled_ms,
            (unsigned)cl.assert_pc,
            cl.lvgl_lock_taken ? "true" : "false",
            (unsigned)cl.lv_free, (unsigned)cl.lv_biggest,
            (unsigned)cl.lv_used_pct, (unsigned)cl.lv_frag_pct,
            (unsigned)cl.pool_seq, (unsigned)cl.pool_age_ms,
            (unsigned)cl.pool_free, (unsigned)cl.pool_biggest,
            (unsigned)cl.pool_used_pct, (unsigned)cl.pool_frag_pct,
            (unsigned)cl.free_heap, (unsigned)cl.min_free_heap);
    } else {
        snprintf(crash, sizeof crash, "{\"present\":false}");
    }

    char body[1280];
    /* "built" is now the genuine build time (esp_app_desc), not the stale
     * CMake-configure timestamp. "max_size" is the inactive slot size so the
     * uploader doesn't duplicate the partition layout (#55). "free_heap" /
     * "min_free_heap" expose live + low-water heap over WiFi so heap regressions
     * are visible without serial (the 10 KB-starvation class of bug). */
    snprintf(body, sizeof body,
        "{\"version\":\"%s\",\"slot\":\"%s\",\"status\":\"%s\",\"built\":\"%s %s\","
        "\"max_size\":%u,\"free_heap\":%u,\"min_free_heap\":%u,\"lvgl\":%s,"
        "\"reset_reason\":\"%s\",\"boot_count\":%u,\"last_crash\":%s}",
        FIRMWARE_VERSION_STR, run ? run->label : "?",
        st == ESP_OTA_IMG_VALID ? "valid" :
        st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" : "unknown",
        desc->date, desc->time,
        (unsigned)(next ? next->size : 0),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(), lvgl,
        reset_reason_str(), (unsigned)crash_log_boot_count(), crash);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)tuning_html_gz, tuning_html_gz_len);
}

static const char *mode_str(oven_mode_t m) {
    switch (m) {
        case MODE_IDLE:               return "idle";
        case MODE_RUNNING:            return "running";
        case MODE_FAULT:              return "fault";
        case MODE_PENDING_VALIDATION: return "pending_validation";
        case MODE_STEP_TEST:          return "step_test";
        case MODE_DRYING:             return "drying";
        default:                      return "unknown";
    }
}
static const char *step_end_str(uint8_t r) {
    switch (r) {
        case STEP_END_CEILING:      return "ceiling";
        case STEP_END_MAX_DURATION: return "max_duration";
        case STEP_END_MANUAL_STOP:  return "manual_stop";
        case STEP_END_FAULT:        return "fault";
        default:                    return "none";
    }
}

static esp_err_t telemetry_handler(httpd_req_t *req) {
    oven_state_t s;
    control_get_snapshot(&s);
    telemetry_view_t v = {
        .api = TELEMETRY_API_VERSION, .mode = mode_str(s.mode),
        .temp_c = s.temp_c, .setpoint_c = s.setpoint_c, .cj_c = s.cj_temp_c,
        .duty = s.duty_percent, .elapsed_ms = s.elapsed_ms, .fault = s.fault_flags,
        .run_id = s.run_id, .step_active = s.step_active, .step_duty = s.step_duty,
        .step_ceiling_c = s.step_ceiling_c, .step_max_ms = s.step_max_ms,
        .step_end = step_end_str(s.step_end_reason),
        .dry_active = s.dry_active,
        .dry_stage = dry_stage_name((dry_stage_t)s.dry_stage),
        .dry_target_c = s.dry_target_c, .dry_hold_ms = s.dry_hold_ms,
        .dry_hold_elapsed_ms = s.dry_hold_elapsed_ms,
        .dry_end = dry_end_name((dry_end_reason_t)s.dry_end_reason),
        .pending_pid_valid = s.pending_pid_valid,
        .kp = s.pending_pid.kp, .ki = s.pending_pid.ki, .kd = s.pending_pid.kd,
    };
    /* Sized for the v3 worst case: longest mode/end-reason strings, widest
     * u32 fields and negative temperatures serialise to 510 bytes + NUL.
     * The v2 shape needed 361, so the old 384-byte buffer no longer fits the
     * six drying fields (~149 bytes). telemetry_json returns -1 on truncation,
     * which falls through to the degraded body below rather than emitting
     * malformed JSON. */
    char body[640];
    if (telemetry_json(&v, body, sizeof body) < 0) {
        /* Should never happen for the bounded field set; fail safe with a
         * FULL-shape object (not a bare {api,mode}) so the SPA renders a
         * degraded reading instead of treating it as a dropped connection. */
        snprintf(body, sizeof body,
            "{\"api\":%d,\"mode\":\"unknown\",\"temp_c\":0,\"setpoint_c\":0,\"duty\":0,"
            "\"elapsed_ms\":0,\"fault\":0,\"run_id\":0,"
            "\"steptest\":{\"active\":false,\"duty\":0,\"ceiling_c\":0,\"max_ms\":0,\"end_reason\":\"none\"},"
            "\"dry_active\":false,\"dry_stage\":\"none\",\"dry_target_c\":0,"
            "\"dry_hold_ms\":0,\"dry_hold_elapsed_ms\":0,\"dry_end\":\"none\","
            "\"pending_pid\":null}", TELEMETRY_API_VERSION);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t profile_handler(httpd_req_t *req) {
    /* Heap-allocated, not static BSS: this 6.4 KB curve array is only needed
     * for the duration of a /profile request (reflow runs only), and permanent
     * BSS is precious on this memory-tight build — the static version cost
     * 6.4 KB of always-on heap headroom. Freed on every exit via `done`.
     * (Still not on the httpd stack: 6.4 KB would overflow it.) See the
     * heap-starvation root cause. */
    float *temps = malloc(PROFILE_CURVE_MAX_SAMPLES * sizeof(float));
    if (!temps) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    uint32_t phase_ms[STAGE_COUNT_];   /* small (a few entries) — fine on the stack */
    uint16_t len = PROFILE_CURVE_MAX_SAMPLES;
    control_get_curve(temps, &len, phase_ms);

    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = ESP_OK;
    pid_gains_t g; float amb;
    control_get_gains(&g, &amb);
    char head[320];
    int n = profile_head_json(head, sizeof head, TELEMETRY_API_VERSION, &g,
                              amb, PROFILE_CURVE_STEP_MS, (unsigned)len);
    if (n < 0) { free(temps); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "head"); }
    /* Bail out the moment a chunk send fails (client gone) instead of spinning
     * through ~1600 doomed sends on the single httpd worker. */
    if (httpd_resp_send_chunk(req, head, n) != ESP_OK) { rc = ESP_FAIL; goto done; }
    for (int s = 0; s < STAGE_COUNT_; s++) {
        char b[16]; int m = snprintf(b, sizeof b, "%s%u", s ? "," : "", (unsigned)phase_ms[s]);
        if (httpd_resp_send_chunk(req, b, m) != ESP_OK) { rc = ESP_FAIL; goto done; }
    }
    if (httpd_resp_send_chunk(req, "],\"temp_c\":[", HTTPD_RESP_USE_STRLEN) != ESP_OK) { rc = ESP_FAIL; goto done; }
    for (uint16_t i = 0; i < len; i++) {
        char b[16]; int m = snprintf(b, sizeof b, "%s%.1f", i ? "," : "", temps[i]);
        if (httpd_resp_send_chunk(req, b, m) != ESP_OK) { rc = ESP_FAIL; goto done; }
    }
    if (httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN) != ESP_OK) { rc = ESP_FAIL; goto done; }
    httpd_resp_send_chunk(req, NULL, 0);   /* terminate the chunked response */
done:
    free(temps);
    return rc;
}

/* Shared token check: returns ESP_OK if the X-OTA-Token header matches the
 * stored token; otherwise sends a 401 and returns ESP_FAIL. Mirrors the
 * inline check in ota_handler so /stop and /suggest-gains reuse it. */
static esp_err_t require_token(httpd_req_t *req) {
    char tok[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing token");
        return ESP_FAIL;
    }
    char expected[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (!nvs_store_get_ota_token(expected) ||
        !ota_token_eq(tok, expected, OTA_TOKEN_HEX_LEN)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t stop_handler(httpd_req_t *req) {
    if (require_token(req) != ESP_OK) return ESP_OK;  /* 401 already sent */
    cmd_msg_t m = { .type = CMD_STOP };
    httpd_resp_set_type(req, "application/json");
    if (!control_post_cmd(&m)) {
        /* Queue momentarily full (e.g. a multi-second sensor-fault stall): don't
         * claim success for a STOP that was dropped — this is safety-relevant. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "{\"error\":\"queue full, retry\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, "{\"status\":\"stopping\"}", HTTPD_RESP_USE_STRLEN);
}

/* Extract a float field like "kp": 1.23 from a small JSON buffer. Returns
 * true and sets *out on success. Tolerates spaces; not a general parser —
 * the body is device-generated by our own SPA and bounded. */
static bool json_get_float(const char *buf, const char *key, float *out) {
    const char *p = strstr(buf, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    return sscanf(p + 1, " %f", out) == 1;
}

static esp_err_t suggest_gains_handler(httpd_req_t *req) {
    if (require_token(req) != ESP_OK) return ESP_OK;   /* 401 already sent */
    if (req->content_len <= 0 || req->content_len > 191) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body length");
    }
    char buf[192] = {0};
    /* Read the whole body: httpd_req_recv returns only what arrived in one
     * socket read, which can be < content_len if the body is fragmented across
     * TCP segments. Loop until we have it all (mirrors ota_handler's recv loop). */
    int received = 0;
    /* Stall-bounded like ota_handler: without it, a client that stops sending
     * mid-body wedges the single httpd worker forever — taking /telemetry,
     * /stop and /ota down with it (review L1). */
    const int64_t stall_limit_us = (int64_t)OTA_RECV_TIMEOUT_TOTAL_MS * 1000;
    int64_t last_progress_us = esp_timer_get_time();
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - last_progress_us > stall_limit_us)
                return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
            continue;
        }
        if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        received += r;
        last_progress_us = esp_timer_get_time();
    }
    buf[received] = '\0';

    pid_gains_t g;
    if (!json_get_float(buf, "\"kp\"", &g.kp) ||
        !json_get_float(buf, "\"ki\"", &g.ki) ||
        !json_get_float(buf, "\"kd\"", &g.kd) ||
        !json_get_float(buf, "\"k_hold\"", &g.k_hold) ||
        !json_get_float(buf, "\"k_rate\"", &g.k_rate)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "missing kp/ki/kd/k_hold/k_rate");
    }
    if (!pid_validate(&g)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gains out of range");
    }
    cmd_msg_t m = { .type = CMD_SUGGEST_PID, .payload.pid = g };
    httpd_resp_set_type(req, "application/json");
    if (!control_post_cmd(&m)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "{\"error\":\"queue full, retry\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, "{\"status\":\"pending_confirm\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_handler(httpd_req_t *req) {
    /* Idle gate is enforced via control_begin_ota() below (a reservation in the
     * control queue), not an unsynchronised control_get_mode() read here — see
     * findings #13. Auth + header checks run first so an unauthenticated or
     * malformed request can't reserve the oven. */

    /* Content-Length check (400 if missing) */
    if (req->content_len <= 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or zero Content-Length");

    /* Content-Type: must be exactly "application/octet-stream", optionally
     * followed by a separator (';' params or whitespace) — not just a prefix,
     * so "application/octet-stream-evil" is rejected (finding #10). */
    char ctype[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof ctype) == ESP_OK) {
        static const char base[] = "application/octet-stream";
        size_t blen = sizeof(base) - 1;
        char after = ctype[blen];
        if (strncmp(ctype, base, blen) != 0 ||
            (after != '\0' && after != ' ' && after != ';')) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "Content-Type must be application/octet-stream");
        }
    }

    /* Auth */
    char tok[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing token");
    char expected[OTA_TOKEN_HEX_LEN + 1] = {0};
    if (!nvs_store_get_ota_token(expected) ||
        !ota_token_eq(tok, expected, OTA_TOKEN_HEX_LEN))
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || part == esp_ota_get_running_partition())
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "no inactive OTA slot to write");   /* #15: clear error, not a cryptic generic 500 */
    /* Compare as size_t, BEFORE narrowing to int. content_len comes straight
     * from the client's header; narrowed first, anything above INT_MAX turned
     * negative, slipped past this check and reached esp_ota_begin as
     * (size_t)-1 == OTA_SIZE_UNKNOWN, which erases the whole inactive slot
     * (the previous firmware) before the empty transfer failed. Token-gated,
     * so a nuisance rather than an exploit, but the intent is "reject anything
     * larger than the slot" and this now says exactly that
     * (pre-publication review, low item 3). */
    if (req->content_len > part->size)
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "firmware larger than OTA slot");
    int total = (int)req->content_len;   /* <= part->size (0x1F0000), so it fits */

    /* Reserve the (idle) oven for this update. Decided on the control task,
     * serialised with START handling, so a reflow can't start between this
     * check and the write — and it replaces the unsynchronised
     * control_get_mode() read (finding #13). */
    if (!control_begin_ota()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"oven not idle\"}", HTTPD_RESP_USE_STRLEN);  /* #12 */
    }

    esp_ota_handle_t op = 0;
    if (esp_ota_begin(part, total, &op) != ESP_OK) {
        control_end_ota();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");
    }

    /* Heap-allocated chunk buffer, freed on every exit below. NOT on the httpd
     * worker stack (a 4 KB frame overflows it — finding #7) and NOT permanent
     * BSS (that 4 KB matters on this memory-tight device — it pushed the boot
     * heap over the edge). Only alive during an upload. */
    char *buf = malloc(OTA_UPLOAD_CHUNK_SIZE);
    if (!buf) {
        esp_ota_abort(op); control_end_ota();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int received = 0;
    /* Stall timeout: abort only after a full window with NO progress. The
     * previous code measured time since the *start* of the transfer on the
     * first timeout, so a long but healthy upload was killed by a single late
     * timeout (finding #8). Reset the reference on every successful chunk. */
    const int64_t stall_limit_us = (int64_t)OTA_RECV_TIMEOUT_TOTAL_MS * 1000;
    int64_t last_progress_us = esp_timer_get_time();
    while (received < total) {
        int r = httpd_req_recv(req, buf, OTA_UPLOAD_CHUNK_SIZE);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - last_progress_us > stall_limit_us) {
                free(buf); esp_ota_abort(op); control_end_ota();
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv timeout");
            }
            continue;
        }
        if (r <= 0) { free(buf); esp_ota_abort(op); control_end_ota(); return ESP_FAIL; }
        if (esp_ota_write(op, buf, r) != ESP_OK) {
            free(buf); esp_ota_abort(op); control_end_ota();
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
        }
        received += r;
        last_progress_us = esp_timer_get_time();  /* progress resets the stall timer */
    }
    free(buf);
    if (esp_ota_end(op) != ESP_OK) {
        control_end_ota();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        control_end_ota();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot");
    }
    /* Success: the reservation is intentionally NOT released — the device
     * reboots into the new image below, which resets all state anyway. */

    /* Report what was WRITTEN, not what is running (#26). This used to format
     * FIRMWARE_VERSION_STR — a compile-time constant of the *running* image —
     * under a field named "new_version", so flashing d728738 answered
     * "new_version":"a6c19c7", which reads exactly like an upload that
     * silently did nothing. The new image's version is only legible through
     * its esp_app_desc, so read it back off the slot just written.
     *
     * That descriptor is only trustworthy because tools/pio_gen_version.py now
     * writes version.txt (PROJECT_VER). Images built before that carry a
     * frozen string from whenever CMake last reconfigured, so an unreadable or
     * absent descriptor reports "?" rather than a plausible-looking lie. */
    esp_app_desc_t wrote;
    const char *wrote_ver =
        (esp_ota_get_partition_description(part, &wrote) == ESP_OK) ? wrote.version : "?";
    char resp[160];
    snprintf(resp, sizeof resp,
             "{\"status\":\"rebooting\",\"slot\":\"%s\","
             "\"previous_version\":\"%s\",\"new_version\":\"%.32s\"}",
             part->label, FIRMWARE_VERSION_STR, wrote_ver);
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN) != ESP_OK)  /* #14: log, but reboot regardless */
        ESP_LOGW(TAG, "final OTA response send failed; rebooting anyway");

    /* Reboot from a one-shot timer (not inline) so this handler returns,
     * httpd flushes the JSON and closes the socket, and only then do we
     * restart — otherwise the client sees a connection reset (finding #11). */
    const esp_timer_create_args_t targs = { .callback = ota_restart_cb, .name = "ota_restart" };
    esp_timer_handle_t th = NULL;
    if (esp_timer_create(&targs, &th) == ESP_OK) {
        esp_timer_start_once(th, 1000 * 1000);  /* 1 s grace for the TCP flush */
    } else {
        /* Can't schedule the deferred reboot (OOM). The new image is already
         * set bootable, so reboot inline rather than leave it unactivated —
         * the client may miss the flushed response, an acceptable rare fallback. */
        ESP_LOGE(TAG, "restart timer create failed — rebooting now");
        esp_restart();
    }
    return ESP_OK;
}

void ota_server_start(void) {
    if (s_server) return;
    ensure_token();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = OTA_MAX_URI_HANDLERS;
    cfg.stack_size = 8192;  /* headroom for the esp_ota_write call tree (finding #7) */
    /* When all max_open_sockets (7) are taken, evict the least-recently-used
     * connection instead of refusing the new one. Without it a few idle
     * keep-alive connections (a misbehaving client, forgotten browser tabs)
     * lock POST /stop out until they close. The touchscreen STOP never depends
     * on this; it only keeps the network STOP reachable. An in-flight handler
     * such as an OTA upload cannot be evicted by it: new connections are
     * accepted by this same single httpd task, which is busy inside that
     * handler. No heap cost (pre-publication review, low item 2). */
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    httpd_uri_t root    = { .uri="/",        .method=HTTP_GET,  .handler=root_handler };
    httpd_uri_t version = { .uri="/version", .method=HTTP_GET,  .handler=version_handler };
    httpd_uri_t ota     = { .uri="/ota",     .method=HTTP_POST, .handler=ota_handler };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &version);
    httpd_register_uri_handler(s_server, &ota);
    httpd_uri_t telemetry = { .uri="/telemetry", .method=HTTP_GET, .handler=telemetry_handler };
    httpd_register_uri_handler(s_server, &telemetry);
    httpd_uri_t stop = { .uri="/stop", .method=HTTP_POST, .handler=stop_handler };
    httpd_register_uri_handler(s_server, &stop);
    httpd_uri_t suggest = { .uri="/suggest-gains", .method=HTTP_POST, .handler=suggest_gains_handler };
    httpd_register_uri_handler(s_server, &suggest);
    httpd_uri_t profile = { .uri="/profile", .method=HTTP_GET, .handler=profile_handler };
    httpd_register_uri_handler(s_server, &profile);
    ESP_LOGI(TAG, "OTA server up on port 80");
}

bool ota_server_is_running(void) { return s_server != NULL; }
#endif /* ESP_PLATFORM */
