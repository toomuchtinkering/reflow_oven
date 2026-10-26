#include <unity.h>
#include <string.h>
#include <math.h>
#include "storage/nvs_schema.h"
#include "config.h"

void setUp(void) {}
void tearDown(void) {}

void test_profile_round_trip(void) {
    profile_t in; profile_defaults(&in);
    profile_v2_t blob;
    profile_to_blob(&in, &blob);
    profile_t out;
    TEST_ASSERT_TRUE(profile_from_blob(&blob, sizeof(blob), &out));
    TEST_ASSERT_EQUAL_FLOAT(in.first_heatup_rate,  out.first_heatup_rate);
    TEST_ASSERT_EQUAL_UINT16(in.soak_temp, out.soak_temp);
    TEST_ASSERT_EQUAL_UINT16(in.reflow_time, out.reflow_time);
    TEST_ASSERT_EQUAL_FLOAT(in.cool_down_rate, out.cool_down_rate);
}

void test_profile_version_mismatch_rejected(void) {
    profile_v2_t blob;
    profile_t in; profile_defaults(&in);
    profile_to_blob(&in, &blob);
    blob.version = 99;
    profile_t out;
    TEST_ASSERT_FALSE(profile_from_blob(&blob, sizeof(blob), &out));
}

void test_profile_truncated_blob_rejected(void) {
    profile_v2_t blob; profile_t in; profile_defaults(&in);
    profile_to_blob(&in, &blob);
    profile_t out;
    TEST_ASSERT_FALSE(profile_from_blob(&blob, sizeof(blob) - 1, &out));
}

void test_pid_round_trip(void) {
    pid_gains_t in; pid_defaults(&in);
    pid_v2_t blob;
    pid_to_blob(&in, &blob);
    pid_gains_t out;
    TEST_ASSERT_TRUE(pid_from_blob(&blob, sizeof(blob), &out));
    TEST_ASSERT_EQUAL_FLOAT(in.kp, out.kp);
    TEST_ASSERT_EQUAL_FLOAT(in.ki, out.ki);
    TEST_ASSERT_EQUAL_FLOAT(in.kd, out.kd);
}

void test_pid_defaults_match_config(void) {
    pid_gains_t g; pid_defaults(&g);
    TEST_ASSERT_EQUAL_FLOAT(KP_DEFAULT, g.kp);
    TEST_ASSERT_EQUAL_FLOAT(KI_DEFAULT, g.ki);
    TEST_ASSERT_EQUAL_FLOAT(KD_DEFAULT, g.kd);
}

void test_pid_validate_accepts_defaults(void) {
    pid_gains_t g; pid_defaults(&g);
    TEST_ASSERT_TRUE(pid_validate(&g));
}

void test_pid_validate_rejects_negative_kp(void) {
    pid_gains_t g = {.kp = -1.0f, .ki = KI_DEFAULT, .kd = KD_DEFAULT};
    TEST_ASSERT_FALSE(pid_validate(&g));
}

void test_pid_validate_rejects_negative_ki(void) {
    pid_gains_t g = {.kp = KP_DEFAULT, .ki = -1.0f, .kd = KD_DEFAULT};
    TEST_ASSERT_FALSE(pid_validate(&g));
}

void test_pid_validate_rejects_negative_kd(void) {
    pid_gains_t g = {.kp = KP_DEFAULT, .ki = KI_DEFAULT, .kd = -1.0f};
    TEST_ASSERT_FALSE(pid_validate(&g));
}

void test_pid_validate_rejects_kp_over_max(void) {
    pid_gains_t g = {.kp = KP_MAX + 1.0f, .ki = KI_DEFAULT, .kd = KD_DEFAULT};
    TEST_ASSERT_FALSE(pid_validate(&g));
}

void test_pid_validate_rejects_nan_and_inf(void) {
    /* NaN slips past a bare range check (NaN compares false to both bounds), so
     * pid_validate must reject non-finite gains explicitly. */
    pid_gains_t nan_kp = {.kp = NAN,      .ki = KI_DEFAULT, .kd = KD_DEFAULT};
    pid_gains_t inf_ki = {.kp = KP_DEFAULT, .ki = INFINITY, .kd = KD_DEFAULT};
    pid_gains_t inf_kd = {.kp = KP_DEFAULT, .ki = KI_DEFAULT, .kd = -INFINITY};
    TEST_ASSERT_FALSE(pid_validate(&nan_kp));
    TEST_ASSERT_FALSE(pid_validate(&inf_ki));
    TEST_ASSERT_FALSE(pid_validate(&inf_kd));
}

void test_pid_v2_round_trip_includes_ff(void) {
    pid_gains_t in = { .kp = 2.5f, .ki = 0.035f, .kd = 10.0f,
                       .k_hold = 0.21f, .k_rate = 68.0f };
    pid_v2_t blob;
    pid_to_blob(&in, &blob);
    pid_gains_t out;
    TEST_ASSERT_TRUE(pid_from_blob(&blob, sizeof(blob), &out));
    TEST_ASSERT_EQUAL_FLOAT(0.21f, out.k_hold);
    TEST_ASSERT_EQUAL_FLOAT(68.0f, out.k_rate);
}

/* A stored v1 blob (pre-OTA device state) migrates: kp/ki/kd kept, ff = defaults. */
void test_pid_v1_blob_migrates_with_ff_defaults(void) {
    pid_v1_t v1 = { .version = 1, .kp = 1.55f, .ki = 0.011f, .kd = 15.96f };
    pid_gains_t out;
    TEST_ASSERT_TRUE(pid_from_blob(&v1, sizeof(v1), &out));
    TEST_ASSERT_EQUAL_FLOAT(1.55f, out.kp);
    TEST_ASSERT_EQUAL_FLOAT(KFF_HOLD_DEFAULT, out.k_hold);
    TEST_ASSERT_EQUAL_FLOAT(KFF_RATE_DEFAULT, out.k_rate);
}

/* KI_MAX shrank 50 -> 1.0 with the identified plant; a keypad/API Ki above
 * the new cap must be rejected (it would be ~1000x past stability). */
void test_pid_validate_rejects_ki_over_new_max(void) {
    pid_gains_t g; pid_defaults(&g);
    g.ki = KI_MAX + 0.1f;
    TEST_ASSERT_FALSE(pid_validate(&g));
}

void test_pid_validate_rejects_out_of_range_ff(void) {
    pid_gains_t g; pid_defaults(&g);
    g.k_hold = KFF_HOLD_MAX + 0.5f;
    TEST_ASSERT_FALSE(pid_validate(&g));
    pid_defaults(&g); g.k_rate = -1.0f;
    TEST_ASSERT_FALSE(pid_validate(&g));
    pid_defaults(&g); g.k_rate = NAN;
    TEST_ASSERT_FALSE(pid_validate(&g));
}

/* ---- drying presets ---- */

void test_dry_presets_defaults_are_valid(void) {
    dry_presets_t p;
    dry_presets_defaults(&p);
    TEST_ASSERT_TRUE(dry_presets_validate(&p));
    TEST_ASSERT_EQUAL_UINT16(45, p.e[0].temp_c);
    TEST_ASSERT_EQUAL_UINT16(360, p.e[0].hold_min);
    TEST_ASSERT_EQUAL_UINT16(90, p.e[4].temp_c);
    /* The last index specifically: an off-by-one loop bound leaves it unset. */
    TEST_ASSERT_EQUAL_UINT16(90,  p.e[DRY_PRESET_COUNT - 1].temp_c);
    TEST_ASSERT_EQUAL_UINT16(360, p.e[DRY_PRESET_COUNT - 1].hold_min);
}

void test_dry_presets_blob_round_trip(void) {
    dry_presets_t in, out;
    dry_presets_defaults(&in);
    /* Every entry distinct, and temp_c never equal to hold_min, so a copy that
     * skips an entry or transposes two fields cannot pass. */
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        in.e[i].temp_c   = (uint16_t)(41 + i);      /* 41..46, all in range */
        in.e[i].hold_min = (uint16_t)(100 + 7 * i); /* 100..135, != temp_c */
    }

    /* Poison both buffers so an entry the code never writes reads back as a
     * known wrong value rather than as whatever the stack happened to hold. */
    dry_presets_v1_t blob;
    memset(&blob, 0xAA, sizeof blob);
    dry_presets_to_blob(&in, &blob);
    memset(&out, 0xAA, sizeof out);
    TEST_ASSERT_TRUE(dry_presets_from_blob(&blob, sizeof blob, &out));
    for (unsigned i = 0; i < DRY_PRESET_COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT16(41 + i,      out.e[i].temp_c);
        TEST_ASSERT_EQUAL_UINT16(100 + 7 * i, out.e[i].hold_min);
    }
}

void test_dry_presets_rejects_wrong_length(void) {
    dry_presets_t in, out;
    dry_presets_defaults(&in);
    dry_presets_v1_t blob;
    dry_presets_to_blob(&in, &blob);
    TEST_ASSERT_FALSE(dry_presets_from_blob(&blob, sizeof(blob) - 1, &out));
}

void test_dry_presets_rejects_wrong_version(void) {
    dry_presets_t in, out;
    dry_presets_defaults(&in);
    dry_presets_v1_t blob;
    dry_presets_to_blob(&in, &blob);
    blob.version = 99;
    TEST_ASSERT_FALSE(dry_presets_from_blob(&blob, sizeof blob, &out));
}

void test_dry_presets_validate_rejects_out_of_range(void) {
    dry_presets_t p;

    dry_presets_defaults(&p);
    p.e[1].temp_c = (uint16_t)(DRY_TEMP_MAX_C + 1);
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[1].temp_c = (uint16_t)(DRY_TEMP_MIN_C - 1);
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[3].hold_min = 0;
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[3].hold_min = (uint16_t)(DRY_DURATION_MAX_MS / 60000u) + 1;
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    /* The last entry must be checked too: an off-by-one loop bound would let
     * an out-of-range target through as a thermal setpoint on load. */
    dry_presets_defaults(&p);
    p.e[DRY_PRESET_COUNT - 1].temp_c = (uint16_t)(DRY_TEMP_MAX_C + 1);
    TEST_ASSERT_FALSE(dry_presets_validate(&p));

    dry_presets_defaults(&p);
    p.e[DRY_PRESET_COUNT - 1].hold_min = 0;
    TEST_ASSERT_FALSE(dry_presets_validate(&p));
}

/* The bounds are inclusive: sm_start_dry clamps into [DRY_TEMP_MIN_C,
 * DRY_TEMP_MAX_C] and [DRY_HOLD_MIN_MS, DRY_DURATION_MAX_MS], so a preset
 * saved at a bound must survive validation. If it did not, saving a preset at
 * the maximum would revert to defaults on every reboot, looking like flash
 * corruption. Testing only from the reject side leaves < vs <= free. */
void test_dry_presets_validate_accepts_bounds(void) {
    dry_presets_t p;
    dry_presets_defaults(&p);
    p.e[0].temp_c   = (uint16_t)DRY_TEMP_MIN_C;
    p.e[0].hold_min = 1;                     /* DRY_HOLD_MIN_MS = 1 min */
    p.e[DRY_PRESET_COUNT - 1].temp_c   = (uint16_t)DRY_TEMP_MAX_C;
    p.e[DRY_PRESET_COUNT - 1].hold_min = (uint16_t)(DRY_DURATION_MAX_MS / 60000u);
    TEST_ASSERT_TRUE(dry_presets_validate(&p));
}

void test_dry_preset_names_are_stable(void) {
    TEST_ASSERT_EQUAL_STRING("PLA", dry_preset_name(0));
    TEST_ASSERT_EQUAL_STRING("PC",  dry_preset_name(DRY_PRESET_COUNT - 1));
    TEST_ASSERT_EQUAL_STRING("?",   dry_preset_name(DRY_PRESET_COUNT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_profile_round_trip);
    RUN_TEST(test_profile_version_mismatch_rejected);
    RUN_TEST(test_profile_truncated_blob_rejected);
    RUN_TEST(test_pid_round_trip);
    RUN_TEST(test_pid_defaults_match_config);
    RUN_TEST(test_pid_validate_accepts_defaults);
    RUN_TEST(test_pid_validate_rejects_negative_kp);
    RUN_TEST(test_pid_validate_rejects_negative_ki);
    RUN_TEST(test_pid_validate_rejects_negative_kd);
    RUN_TEST(test_pid_validate_rejects_kp_over_max);
    RUN_TEST(test_pid_validate_rejects_nan_and_inf);
    RUN_TEST(test_pid_v2_round_trip_includes_ff);
    RUN_TEST(test_pid_v1_blob_migrates_with_ff_defaults);
    RUN_TEST(test_pid_validate_rejects_ki_over_new_max);
    RUN_TEST(test_pid_validate_rejects_out_of_range_ff);
    RUN_TEST(test_dry_presets_defaults_are_valid);
    RUN_TEST(test_dry_presets_blob_round_trip);
    RUN_TEST(test_dry_presets_rejects_wrong_length);
    RUN_TEST(test_dry_presets_rejects_wrong_version);
    RUN_TEST(test_dry_presets_validate_rejects_out_of_range);
    RUN_TEST(test_dry_presets_validate_accepts_bounds);
    RUN_TEST(test_dry_preset_names_are_stable);
    return UNITY_END();
}
