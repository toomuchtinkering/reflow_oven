#include <unity.h>
#include "audio/audio_melodies.h"

void setUp(void)    {}
void tearDown(void) {}

static void check_melody_bounds(const audio_melody_t *m, uint32_t max_burst_ms) {
    TEST_ASSERT_GREATER_THAN(0, m->n);
    TEST_ASSERT_NOT_NULL(m->notes);
    uint32_t total = 0;
    for (size_t i = 0; i < m->n; i++) {
        const audio_note_t *n = &m->notes[i];
        /* Speaker datasheet rolls off below ~150 Hz; FM8002E + Q-8R are
         * comfortable up to a few kHz. 500-3000 Hz is well inside the
         * passband and prevents accidental 0-Hz typos. */
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16(500,  n->freq_hz);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16   (3000, n->freq_hz);
        TEST_ASSERT_GREATER_THAN(0, n->on_ms);
        /* Catch UINT16_MAX-style typos in either field. */
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(1000, n->on_ms);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(1000, n->gap_after_ms);
        total += n->on_ms + n->gap_after_ms;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(max_burst_ms, total);
}

void test_stage_transition_in_bounds(void) {
    check_melody_bounds(&MELODY_STAGE_TRANSITION, 400);
}
void test_run_complete_in_bounds(void) {
    check_melody_bounds(&MELODY_RUN_COMPLETE, 1200);
}
void test_fault_burst_in_bounds(void) {
    check_melody_bounds(&MELODY_FAULT_BURST, 1000);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stage_transition_in_bounds);
    RUN_TEST(test_run_complete_in_bounds);
    RUN_TEST(test_fault_burst_in_bounds);
    return UNITY_END();
}
