#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "power.h"
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
#define PIN_VEXT 36 /* powers the display (and the header's 3.3 V pin), on when low */
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RESET 21
#define PIN_ADC_CTRL 37 /* switches the battery's divider onto PIN_BATTERY (power.h) */
#define PIN_BATTERY 1   /* ADC1, channel 0 */

/* The divider, from Heltec's schematics for the V3, V3.1 and V3.2: 390k over 100k. */
#define DIVIDER_TOP_K 390
#define DIVIDER_BOTTOM_K 100
#define BATTERY_SETTLE_US 2000 /* after the switch, before reading: far longer than it needs */
#define BATTERY_SAMPLES 16

#define OLED_ADDRESS 0x3C
#define OLED_TIMEOUT_MS 50

#define BUSY_TIMEOUT_US 100000 /* far longer than any command; calibration takes a few ms */

static spi_device_handle_t spi;
static i2c_master_dev_handle_t oled;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t adc_cali;
static adc_channel_t battery_channel;
static bool have_adc, have_cali;
static struct power_sense sense;

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

/* --- The display ---------------------------------------------------------------------------- */

/* The first byte of each I2C write says what follows (SSD1306 datasheet, section 8.1.5). */
#define OLED_COMMANDS 0x00
#define OLED_DATA 0x40

static bool oled_send(const uint8_t *buf, size_t len) {
    return i2c_master_transmit(oled, buf, len, OLED_TIMEOUT_MS) == ESP_OK;
}

bool board_screen_init(void) {
    gpio_config_t out = {.pin_bit_mask = 1ull << PIN_VEXT | 1ull << PIN_OLED_RESET,
                         .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&out) != ESP_OK) {
        return false;
    }
    /* Power, then hold RES# low for more than the 3 us the datasheet asks (section 8.9). */
    gpio_set_level(PIN_VEXT, 0);
    gpio_set_level(PIN_OLED_RESET, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(PIN_OLED_RESET, 1);
    esp_rom_delay_us(10000);

    i2c_master_bus_config_t bus = {.i2c_port = -1,
                                   .sda_io_num = PIN_OLED_SDA,
                                   .scl_io_num = PIN_OLED_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true};
    i2c_master_bus_handle_t handle;
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = OLED_ADDRESS,
                               .scl_speed_hz = 400000};
    if (i2c_new_master_bus(&bus, &handle) != ESP_OK ||
        i2c_master_probe(handle, OLED_ADDRESS, OLED_TIMEOUT_MS) != ESP_OK ||
        i2c_master_bus_add_device(handle, &dev, &oled) != ESP_OK) {
        return false;
    }

    /* The datasheet's software set-up (its application note's flow), for a 128x64 panel whose
     * charge pump is on the chip. */
    /* clang-format off */
    static const uint8_t setup[] = {
        OLED_COMMANDS,
        0xAE,       /* display off while it is set up */
        0xD5, 0x80, /* clock: the reset default */
        0xA8, 0x3F, /* 64 rows */
        0xD3, 0x00, /* no vertical offset */
        0x40,       /* start at row 0 */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x02, /* page addressing: a page at a time, as board_screen_page() sends them */
#if CONFIG_TERN_SCREEN_FLIP
        0xA0, 0xC0, /* columns and rows in the order the controller numbers them */
#else
        0xA1, 0xC8, /* both reversed, the usual way round for this panel */
#endif
        0xDA, 0x12, /* the panel's rows wired alternately */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1, /* pre-charge, for the internal charge pump */
        0xDB, 0x40, /* VCOMH */
        0xA4,       /* show what is in RAM */
        0xA6,       /* light on dark */
    };
    /* clang-format on */
    static const uint8_t on[] = {OLED_COMMANDS, 0xAF};
    static const uint8_t blank[128];
    if (!oled_send(setup, sizeof setup)) {
        return false;
    }
    for (int page = 0; page < 8; page++) {
        if (!board_screen_page(page, blank)) {
            return false;
        }
    }
    return oled_send(on, sizeof on);
}

bool board_screen_page(int page, const uint8_t data[128]) {
    /* The page, then its first column, low half and high half. */
    uint8_t where[] = {OLED_COMMANDS, (uint8_t)(0xB0 | (page & 7)), 0x00, 0x10};
    uint8_t buf[1 + 128];
    buf[0] = OLED_DATA;
    memcpy(&buf[1], data, 128);
    return oled_send(where, sizeof where) && oled_send(buf, sizeof buf);
}

bool board_screen_power(bool on) {
    /* The charge pump goes on before the panel and off after it (the datasheet's application
     * note on the charge pump). */
    static const uint8_t off_seq[] = {OLED_COMMANDS, 0xAE, 0x8D, 0x10};
    static const uint8_t on_seq[] = {OLED_COMMANDS, 0x8D, 0x14, 0xAF};
    return on ? oled_send(on_seq, sizeof on_seq) : oled_send(off_seq, sizeof off_seq);
}

bool board_battery_init(void) {
    adc_unit_t unit;
    gpio_config_t ctrl = {.pin_bit_mask = 1ull << PIN_ADC_CTRL, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&ctrl) != ESP_OK ||
        adc_oneshot_io_to_channel(PIN_BATTERY, &unit, &battery_channel) != ESP_OK) {
        return false;
    }
    adc_oneshot_unit_init_cfg_t init = {.unit_id = unit};
    /* 2.5 dB reads to about 1.25 V: a full cell, 4.2 V, is 0.86 V after the divider. */
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&init, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, battery_channel, &chan) != ESP_OK) {
        return false;
    }
    adc_cali_curve_fitting_config_t cal = {.unit_id = unit,
                                           .chan = battery_channel,
                                           .atten = ADC_ATTEN_DB_2_5,
                                           .bitwidth = ADC_BITWIDTH_DEFAULT};
    have_cali = adc_cali_create_scheme_curve_fitting(&cal, &adc_cali) == ESP_OK;
    have_adc = true;
    return true;
}

/* The battery's millivolts with GPIO37 at `level`, or 0 if the ADC would not say. */
static uint16_t battery_at(int level) {
    int sum = 0;
    gpio_set_level(PIN_ADC_CTRL, level);
    esp_rom_delay_us(BATTERY_SETTLE_US);
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(adc, battery_channel, &raw) != ESP_OK) {
            return 0;
        }
        if (!have_cali || adc_cali_raw_to_voltage(adc_cali, raw, &mv) != ESP_OK) {
            mv = raw * 1250 / 4095; /* uncalibrated: the range taken as even */
        }
        sum += mv;
    }
    long at_pin = sum / BATTERY_SAMPLES;
    long mv = at_pin * (DIVIDER_TOP_K + DIVIDER_BOTTOM_K) / DIVIDER_BOTTOM_K;
    return (uint16_t)(mv > UINT16_MAX ? UINT16_MAX : mv);
}

uint16_t board_battery_mv(void) {
    if (!have_adc) {
        return 0;
    }
    uint16_t low = 0, high = 0;
    if (!sense.known || !sense.high_enables) {
        low = battery_at(0);
    }
    if (!sense.known || sense.high_enables) {
        high = battery_at(1);
    }
    uint16_t mv = power_pick(&sense, low, high);
    /* Left off until the next reading: the other way from the one that turns it on. Until that is
     * known, low, which is off on the V3.2 and costs the others a few microamps. */
    gpio_set_level(PIN_ADC_CTRL, sense.known && !sense.high_enables ? 1 : 0);
    return mv;
}
