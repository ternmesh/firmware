#include "board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "tern/err.h"

/* Heltec's pin map for the V3. */
#define PIN_NSS 8
#define PIN_SCK 9
#define PIN_MOSI 10
#define PIN_MISO 11
#define PIN_RESET 12
#define PIN_BUSY 13
#define PIN_BUTTON 0 /* PRG, low when pressed */
#define PIN_LED 35

#define BUSY_TIMEOUT_US 100000 /* far longer than any command; calibration takes a few ms */

static spi_device_handle_t spi;

tern_time board_now(void) { return (tern_time)esp_timer_get_time() * 1000; }

static tern_time bus_now(void *ctx) {
    (void)ctx;
    return board_now();
}

static int bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    /* The chip raises BUSY while it works on the last command and takes no new one until it
     * drops (datasheet section 8.3.1). */
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PIN_BUSY)) {
        if (esp_timer_get_time() - start > BUSY_TIMEOUT_US) {
            return TERN_EIO;
        }
    }
    spi_transaction_t t = {.length = 8 * len, .tx_buffer = tx, .rx_buffer = rx};
    return spi_device_polling_transmit(spi, &t) == ESP_OK ? TERN_OK : TERN_EIO;
}

int board_init(struct tern_sx126x *radio) {
    gpio_config_t out = {.pin_bit_mask = 1ull << PIN_RESET | 1ull << PIN_LED,
                         .mode = GPIO_MODE_OUTPUT};
    gpio_config_t in = {.pin_bit_mask = 1ull << PIN_BUSY, .mode = GPIO_MODE_INPUT};
    gpio_config_t button = {.pin_bit_mask = 1ull << PIN_BUTTON,
                            .mode = GPIO_MODE_INPUT,
                            .pull_up_en = GPIO_PULLUP_ENABLE};
    if (gpio_config(&out) != ESP_OK || gpio_config(&in) != ESP_OK ||
        gpio_config(&button) != ESP_OK) {
        return TERN_EIO;
    }
    board_led(false);

    spi_bus_config_t bus = {.sclk_io_num = PIN_SCK,
                            .mosi_io_num = PIN_MOSI,
                            .miso_io_num = PIN_MISO,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = 300};
    spi_device_interface_config_t dev = {.clock_speed_hz = 8 * 1000 * 1000, /* the chip's 16 */
                                         .mode = 0,
                                         .spics_io_num = PIN_NSS,
                                         .queue_size = 1};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(SPI2_HOST, &dev, &spi) != ESP_OK) {
        return TERN_EIO;
    }

    /* Hold NRESET low for over 100 us (section 8.1), then let the chip start. */
    gpio_set_level(PIN_RESET, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(PIN_RESET, 1);
    esp_rom_delay_us(10000);

    struct tern_sx126x_bus sb = {.ctx = NULL, .transfer = bus_transfer, .now = bus_now};
    struct tern_sx126x_board wiring = {.tcxo_mv = 1800, .dio2_rf_switch = true, .dcdc = true};
    return tern_sx126x_init(radio, &sb, &wiring);
}

bool board_button(void) { return gpio_get_level(PIN_BUTTON) == 0; }

void board_led(bool on) { gpio_set_level(PIN_LED, on ? 1 : 0); }
