#include <unity.h>
#include <string.h>
#include <math.h>
#include "ota/telemetry_json.h"

void setUp(void) {}
void tearDown(void) {}

void test_idle_shape_has_all_fields_and_null_pending(void) {
    telemetry_view_t v = {
        .api = 1, .mode = "idle", .temp_c = 24.5f, .setpoint_c = 0.0f,
        .duty = 0, .elapsed_ms = 0, .fault = 0, .run_id = 0,
        .step_active = false, .step_duty = 0, .step_ceiling_c = 0.0f,
        .step_max_ms = 0, .step_end = "none", .pending_pid_valid = false,
    };
    char buf[384];
    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"api\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"idle\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"run_id\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"steptest\":{"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"end_reason\":\"none\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pending_pid\":null}"));
}

void test_step_test_active_with_pending_pid(void) {
    telemetry_view_t v = {
        .api = 1, .mode = "step_test", .temp_c = 150.0f, .setpoint_c = 0.0f,
        .duty = 50, .elapsed_ms = 42000, .fault = 0, .run_id = 3,
        .step_active = true, .step_duty = 50, .step_ceiling_c = 180.0f,
        .step_max_ms = 600000, .step_end = "none", .pending_pid_valid = true,
        .kp = 8.40f, .ki = 0.19f, .kd = 46.0f,
    };
    char buf[384];
    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"step_test\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"run_id\":3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pending_pid\":{\"kp\":8.400"));
    /* well-formed: ends with the closing brace of the object */
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

void test_cj_temp_field_present(void) {
    /* Electronics (cold-junction) temperature rides along so a browser can
     * log how hot the electronics bay gets across a full reflow cycle. */
    telemetry_view_t v = {
        .api = 1, .mode = "running", .temp_c = 160.0f, .cj_c = 55.25f,
        .step_end = "none",
    };
    char buf[384];
    TEST_ASSERT_GREATER_THAN(0, telemetry_json(&v, buf, sizeof buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cj_c\":55.25"));
}

void test_cj_temp_nan_serialises_as_null(void) {
    /* cj_c is NAN until the first successful SPI read; "nan" is not valid
     * JSON and would break the SPA's response parsing. */
    telemetry_view_t v = {
        .api = 1, .mode = "idle", .cj_c = NAN, .step_end = "none",
    };
    char buf[384];
    TEST_ASSERT_GREATER_THAN(0, telemetry_json(&v, buf, sizeof buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cj_c\":null"));
    TEST_ASSERT_NULL(strstr(buf, "nan"));
}

void test_truncation_returns_negative(void) {
    telemetry_view_t v = { .api = 1, .mode = "idle", .step_end = "none" };
    char tiny[16];
    TEST_ASSERT_EQUAL_INT(-1, telemetry_json(&v, tiny, sizeof tiny));
}

/* /profile head (spec §5): api, all five gains with exact field names,
 * ambient_ref_c, and the open phase_end_ms bracket for the chunked tail. */
void test_profile_head_carries_gains_and_ambient(void) {
    pid_gains_t g = { .kp = 0.44f, .ki = 0.0068f, .kd = 4.75f,
                      .k_hold = 0.19f, .k_rate = 71.0f };
    char buf[320];
    int w = profile_head_json(buf, sizeof buf, 2, &g, 30.5f, 1000, 1600);
    TEST_ASSERT_GREATER_THAN(0, w);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"api\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"kp\":0.4400"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ki\":0.00680"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"kd\":4.750"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"k_hold\":0.1900"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"k_rate\":71.00"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ambient_ref_c\":30.50"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"step_ms\":1000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"len\":1600"));
    /* Head must end with the open bracket the handler streams into. */
    TEST_ASSERT_EQUAL_STRING("\"phase_end_ms\":[", buf + w - strlen("\"phase_end_ms\":["));
}

void test_profile_head_truncation_returns_negative(void) {
    pid_gains_t g = { .kp = 1, .ki = 1, .kd = 1, .k_hold = 1, .k_rate = 1 };
    char tiny[24];
    TEST_ASSERT_EQUAL_INT(-1, profile_head_json(tiny, sizeof tiny, 2, &g, 25.0f, 1000, 10));
}

/* v3: the drying block rides alongside the steptest block so an external
 * logger can see a dry in progress without a second endpoint. */
void test_telemetry_includes_drying_fields(void) {
    char buf[768];
    telemetry_view_t v = {0};
    v.api = 3;
    v.mode = "drying";
    v.temp_c = 78.5f; v.setpoint_c = 80.0f; v.cj_c = 41.0f;
    v.duty = 12; v.elapsed_ms = 900000; v.run_id = 7;
    v.dry_active = true;
    v.dry_stage = "hold";
    v.dry_target_c = 80.0f;
    v.dry_hold_ms = 21600000u;
    v.dry_hold_elapsed_ms = 600000u;
    v.dry_end = "none";

    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"drying\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_stage\":\"hold\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_target_c\":80.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_hold_ms\":21600000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_hold_elapsed_ms\":600000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_end\":\"none\""));
    /* still one well-formed object */
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

void test_telemetry_drying_inactive_shape(void) {
    char buf[768];
    telemetry_view_t v = {0};
    v.api = 3; v.mode = "idle";
    v.dry_active = false; v.dry_stage = "none"; v.dry_end = "none";
    int n = telemetry_json(&v, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dry_active\":false"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idle_shape_has_all_fields_and_null_pending);
    RUN_TEST(test_step_test_active_with_pending_pid);
    RUN_TEST(test_cj_temp_field_present);
    RUN_TEST(test_cj_temp_nan_serialises_as_null);
    RUN_TEST(test_truncation_returns_negative);
    RUN_TEST(test_profile_head_carries_gains_and_ambient);
    RUN_TEST(test_profile_head_truncation_returns_negative);
    RUN_TEST(test_telemetry_includes_drying_fields);
    RUN_TEST(test_telemetry_drying_inactive_shape);
    return UNITY_END();
}
