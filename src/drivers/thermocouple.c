#include "drivers/thermocouple.h"
#include "drivers/tc_decode.h"
#include "control/reflow_sm.h"          /* FAULT_TC_* */
#include "config.h"
#include "driver/spi_master.h"

static spi_device_handle_t s_spi;

void thermocouple_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_TC_SCK, .miso_io_num = PIN_TC_MISO, .mosi_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TC_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi));
}

bool thermocouple_read(float *temp_c, float *cj_c, uint8_t *fault_flags, uint32_t *age_ms) {
    uint8_t rx[4] = {0};
    spi_transaction_t t = {.length = 32, .rxlength = 32, .rx_buffer = rx};
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err != ESP_OK) {
        *fault_flags = FAULT_TC_OPEN;
        return false;
    }
    uint32_t w = ((uint32_t)rx[0]<<24)|((uint32_t)rx[1]<<16)|((uint32_t)rx[2]<<8)|rx[3];

    tc_frame_t f;
    tc_decode(w, &f);
    if (cj_c) *cj_c = f.cj_c;   /* valid even on faulted frames */

    if (f.fault_flags) {
        *fault_flags = f.fault_flags;
        return false;
    }

    *temp_c = f.tc_c;
    *fault_flags = 0;
    *age_ms = 0;   /* control_task owns the real staleness timer */
    return true;
}
