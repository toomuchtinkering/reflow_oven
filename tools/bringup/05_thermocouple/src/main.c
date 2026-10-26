/* tools/bringup/05_thermocouple/src/main.c
 * Reads MAX31855 every 500 ms on VSPI (SPI3_HOST) and logs:
 *   - thermocouple bead temperature (compensated)
 *   - cold-junction (chip die) temperature
 *   - raw 32-bit response word
 *   - fault bits
 * Confirms P3/P4 wiring + 3.3 V supply + VSPI host.
 *
 * MAX31855 32-bit word, bit order MSB first:
 *   [31:18] signed thermocouple temperature x0.25 C  (14 bits, two's complement)
 *   [17]    reserved (always 0)
 *   [16]    fault bit (1 = fault — see [2:0])
 *   [15:4]  signed cold-junction temperature x0.0625 C (12 bits)
 *   [3]     reserved (always 0)
 *   [2]     SCV  — short to VCC
 *   [1]     SCG  — short to GND
 *   [0]     OC   — open thermocouple
 *
 * === Diagnostic test plan (in order of cheapest first) ===
 *
 *   Healthy baseline (thermocouple connected, bead in still air):
 *     - cj  ≈ room temperature (within a few °C of ambient)
 *     - temp ≈ cj
 *     - No fault bits.
 *
 *   Open-circuit test:
 *     - Disconnect the thermocouple from the screw terminals.
 *     - Expect: OC fault bit set, FAULT log line, no temp reading.
 *
 *   Test A — input short (isolate chip from wiring):
 *     - Disconnect thermocouple.
 *     - Bridge T+ to T- with a piece of copper wire / paperclip.
 *     - Expect: temp ≈ cj, ≈ ambient, no fault.
 *     - If temp is far from cj while shorted, the chip's input has an
 *       offset (replace the breakout).
 *
 *   Test B — ice-water reference (verifies the thermocouple itself):
 *     - Reconnect the thermocouple.
 *     - Dip the bead in a stirred ice/water slush.
 *     - Expect: temp converges to 0 °C within a few seconds.
 *     - If temp goes to ~+50 °C instead: leads are polarity-reversed
 *       at the screw terminal (swap T+ ↔ T-).
 *     - If temp goes to a different stable negative value: the
 *       thermocouple isn't K-type (mislabelled wire) or has a
 *       parasitic junction along its length.
 *
 *   Boiling-water reference (optional sanity-check at the upper end):
 *     - Dip the bead in boiling water at sea level (≈ 100 °C minus
 *       altitude).
 *     - Expect: temp converges to ≈ 100 °C. Different value tells you
 *       the chip's Seebeck gain is off (wire-type mismatch is the
 *       most common cause).
 */
#include "esp_log.h"
#include "driver/spi_master.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tc_bringup";

static spi_device_handle_t spi;

static void max31855_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TC_SCK,
        .miso_io_num = PIN_TC_MISO,
        .mosi_io_num = -1,           /* MAX31855 is read-only */
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,   /* 1 MHz; chip's max is 5 MHz */
        .mode = 0,
        .spics_io_num = PIN_TC_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &spi));
}

static void max31855_read(float *temp_c, float *cj_c, uint32_t *raw_word,
                          uint8_t *fault) {
    uint8_t rx[4] = {0};
    spi_transaction_t t = {.length = 32, .rxlength = 32, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));

    uint32_t word = ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
                    ((uint32_t)rx[2] << 8)  |  (uint32_t)rx[3];
    *raw_word = word;

    /* Cold-junction temperature: bits [15:4], 12-bit signed, 0.0625 C/LSB.
     * Reported regardless of fault state — useful when diagnosing a stuck
     * chip whose CJ sensor still works but whose thermocouple input lies. */
    int16_t cj_raw = (int16_t)((word >> 4) & 0xFFF);
    if (cj_raw & 0x0800) cj_raw |= 0xF000;           /* sign-extend 12-bit */
    *cj_c = cj_raw * 0.0625f;

    /* Fault bits */
    *fault = (uint8_t)(word & 0x7);                  /* OC | SCG | SCV (bits 0,1,2) */
    bool any_fault = (word & (1u << 16)) != 0;

    if (any_fault || *fault) {
        *temp_c = 0.0f / 0.0f;                       /* NaN */
        return;
    }

    int16_t raw = (int16_t)((word >> 18) & 0x3FFF);
    if (raw & 0x2000) raw |= 0xC000;                 /* sign-extend 14-bit */
    *temp_c = raw * 0.25f;
}

void app_main(void) {
    max31855_init();
    ESP_LOGI(TAG, "MAX31855 read loop. See header for the diagnostic test plan");
    ESP_LOGI(TAG, "  (open-circuit, T+/T- short, ice-water, boiling-water).");
    while (1) {
        float t = 0.0f, cj = 0.0f;
        uint32_t raw = 0;
        uint8_t fault = 0;
        max31855_read(&t, &cj, &raw, &fault);
        if (fault) {
            ESP_LOGI(TAG, "FAULT bits=0x%02x  (OC=%d SCG=%d SCV=%d)  cj=%.2f  raw=0x%08x",
                     fault, fault & 0x1, !!(fault & 0x2), !!(fault & 0x4),
                     cj, (unsigned)raw);
        } else {
            ESP_LOGI(TAG, "temp=%.2f  cj=%.2f  delta=%.2f  raw=0x%08x",
                     t, cj, t - cj, (unsigned)raw);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
