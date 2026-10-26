#include "drivers/audio.h"
#include "config.h"
#include "driver/dac_cosine.h"
#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "audio";

/* Resting-state DAC channel held at 0x80 (mid-scale) between tones so that
 * enabling the amp from this state doesn't snap from rail-to-rail and pop
 * the speaker. Swapped for a cosine-wave channel during audio_play_tone.
 * NULL while a tone is playing. */
static dac_oneshot_handle_t s_oneshot = NULL;
static dac_cosine_handle_t  s_cw      = NULL;

static void rest_at_midscale(void) {
    /* Log + skip rather than ESP_ERROR_CHECK: a DAC peripheral quirk must not
     * panic the whole device over a non-critical resting tone (finding #59). */
    dac_oneshot_config_t cfg = { .chan_id = DAC_CHAN_1 };
    esp_err_t e = dac_oneshot_new_channel(&cfg, &s_oneshot);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "dac_oneshot_new_channel: %s", esp_err_to_name(e));
        s_oneshot = NULL;
        return;
    }
    e = dac_oneshot_output_voltage(s_oneshot, 0x80);
    if (e != ESP_OK) ESP_LOGW(TAG, "dac_oneshot_output_voltage: %s", esp_err_to_name(e));
}

void audio_init(void) {
    /* AUDIO_EN: drive high (=SHUTDOWN) initially. The write-then-configure
     * sequence (matches src/drivers/ssr.c) avoids a transient on the
     * output line during gpio_config. */
    gpio_set_level(PIN_AUDIO_EN, 1);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_AUDIO_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_AUDIO_EN, 1);

    /* Park DAC2 at mid-scale BEFORE enabling the amp so it sees a stable
     * DC level the instant SHUTDOWN releases. */
    rest_at_midscale();

    /* Enable the amp and leave it on for the duration of the program.
     * Idle current is ~4.4 mA — negligible. Keeping it on avoids the
     * audible click/pop the FM8002E emits at each SHUTDOWN transition,
     * which more than makes up for the small power draw. */
    gpio_set_level(PIN_AUDIO_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(AUDIO_AMP_WAKEUP_MS));

    ESP_LOGI(TAG, "audio_init: amp enabled, DAC at mid-scale");
}

void audio_play_tone(uint32_t freq_hz, uint32_t ms) {
    /* Hand DAC2 from the oneshot mid-scale holder to the cosine generator,
     * play, then hand it back. Allocation is a few register writes — cheap
     * compared to a 80-120 ms note duration. */
    if (s_oneshot) {
        esp_err_t de = dac_oneshot_del_channel(s_oneshot);
        if (de != ESP_OK) ESP_LOGW(TAG, "dac_oneshot_del_channel: %s", esp_err_to_name(de));
        s_oneshot = NULL;
    }

    dac_cosine_config_t cfg = {
        .chan_id = DAC_CHAN_1,                      /* GPIO26 */
        .freq_hz = freq_hz,
        .clk_src = DAC_COSINE_CLK_SRC_DEFAULT,
        .atten   = DAC_COSINE_ATTEN_DEFAULT,
        .phase   = DAC_COSINE_PHASE_0,
        .offset  = 0,
        .flags.force_set_freq = true,
    };
    esp_err_t err = dac_cosine_new_channel(&cfg, &s_cw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dac_cosine_new_channel failed at %u Hz: %d",
                 (unsigned)freq_hz, err);
        s_cw = NULL;
        vTaskDelay(pdMS_TO_TICKS(ms));
        rest_at_midscale();
        return;
    }
    if ((err = dac_cosine_start(s_cw)) != ESP_OK)
        ESP_LOGW(TAG, "dac_cosine_start: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(ms));
    if ((err = dac_cosine_stop(s_cw)) != ESP_OK)
        ESP_LOGW(TAG, "dac_cosine_stop: %s", esp_err_to_name(err));
    if ((err = dac_cosine_del_channel(s_cw)) != ESP_OK)
        ESP_LOGW(TAG, "dac_cosine_del_channel: %s", esp_err_to_name(err));
    s_cw = NULL;

    rest_at_midscale();
}

void audio_silence(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
