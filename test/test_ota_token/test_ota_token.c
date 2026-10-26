#include "unity.h"
#include <string.h>

/* Forward decl from src/ota/ota_server.c, exposed for tests. */
int ota_token_eq(const char *a, const char *b, size_t n);

/* NOTE (finding #51): these are functional-correctness checks only. The
 * constant-time property — equal work regardless of where the first byte
 * differs — is provided by mbedtls_ct_memcmp on target and by the
 * no-short-circuit accumulator loop in the host fallback (ota_server.c); a
 * portable unit test can't assert timing-invariance reliably without a noisy
 * statistical harness. The first-byte vs last-byte mismatch cases below at
 * least exercise both extremes so an accidental switch to a short-circuiting
 * strcmp would still be caught functionally. */
void test_eq_match(void)        { TEST_ASSERT_EQUAL(1, ota_token_eq("abc123", "abc123", 6)); }
void test_eq_mismatch(void)     { TEST_ASSERT_EQUAL(0, ota_token_eq("abc123", "abc124", 6)); }
void test_eq_diff_at_start(void){ TEST_ASSERT_EQUAL(0, ota_token_eq("xbc123", "abc123", 6)); }
void test_eq_diff_at_end(void)  { TEST_ASSERT_EQUAL(0, ota_token_eq("abc12X", "abc123", 6)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_eq_match);
    RUN_TEST(test_eq_mismatch);
    RUN_TEST(test_eq_diff_at_start);
    RUN_TEST(test_eq_diff_at_end);
    return UNITY_END();
}
