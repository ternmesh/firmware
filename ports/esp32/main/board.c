#include "board.h"

#include <string.h>

#include "sdkconfig.h"

#include "boards.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "tern/err.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#endif

/* The board the build chose, found by its name, so that the name a client is told and the pins
 * driven cannot disagree. tests/boards.c checks every name Kconfig offers is in boards.c. */
#define B board_def()

#define VEXT_ON (B->vext_high_on ? 1 : 0)

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

static uint64_t bit(int pin) { return pin == BOARD_NO_PIN ? 0 : 1ull << pin; }

static void set(int pin, int level) {
    if (pin != BOARD_NO_PIN) {
        gpio_set_level(pin, level);
    }
}

/* Keeps a pin at its level through deep sleep, where it would otherwise float. */
static void hold(int pin) {
    if (pin != BOARD_NO_PIN) {
        gpio_hold_en(pin);
    }
}

static void release(int pin) {
    if (pin != BOARD_NO_PIN) {
        gpio_hold_dis(pin);
    }
}

static bool amp_init(void);
static void amp_off(void);

static const struct board_def *board_def(void) {
    static const struct board_def *chosen;
    if (chosen == NULL) {
        chosen = board_def_named(CONFIG_TERN_BOARD_NAME);
    }
    return chosen;
}

const char *board_title(void) { return B->title; }
int8_t board_power_min(void) { return board_min_dbm(B); }
int8_t board_power_max(void) { return board_max_dbm(B); }
bool board_power_ok(int dbm) { return board_gives(B, dbm); }

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
    while (gpio_get_level(B->lora.busy)) {
        if (esp_timer_get_time() - start > BUSY_TIMEOUT_US) {
            return TERN_EIO;
        }
    }
    spi_transaction_t t = {.length = 8 * len, .tx_buffer = tx, .rx_buffer = rx};
    return spi_device_polling_transmit(spi, &t) == ESP_OK ? TERN_OK : TERN_EIO;
}

int board_init(struct tern_sx126x *radio) {
    gpio_config_t out = {.pin_bit_mask = bit(B->lora.reset) | bit(B->led),
                         .mode = GPIO_MODE_OUTPUT};
    gpio_config_t in = {.pin_bit_mask = bit(B->lora.busy), .mode = GPIO_MODE_INPUT};
    if (gpio_config(&out) != ESP_OK || gpio_config(&in) != ESP_OK) {
        return TERN_EIO;
    }
    board_led(false);

    spi_bus_config_t bus = {.sclk_io_num = B->lora.sck,
                            .mosi_io_num = B->lora.mosi,
                            .miso_io_num = B->lora.miso,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = 300};
    spi_device_interface_config_t dev = {.clock_speed_hz = 8 * 1000 * 1000, /* the chip's 16 */
                                         .mode = 0,
                                         .spics_io_num = B->lora.nss,
                                         .queue_size = 1};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(SPI2_HOST, &dev, &spi) != ESP_OK) {
        return TERN_EIO;
    }

    /* Hold NRESET low for over 100 us (section 8.1), then let the chip start. */
    gpio_set_level(B->lora.reset, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(B->lora.reset, 1);
    esp_rom_delay_us(10000);

    if (!amp_init()) {
        return TERN_EIO;
    }

    struct tern_sx126x_bus sb = {.ctx = NULL, .transfer = bus_transfer, .now = bus_now};
    struct tern_sx126x_board wiring = {
        .tcxo_mv = B->lora.tcxo_mv, .dio2_rf_switch = B->lora.dio2_rf_switch, .dcdc = true};
    return tern_sx126x_init(radio, &sb, &wiring);
}

/* --- The amplifier --------------------------------------------------------------------------- */

/* A board with an amplifier after its SX1262 (boards.h) is driven through a radio that wraps the
 * chip's: it asks the chip for less power, by the amplifier's gain, so that the core's powers are
 * the antenna's, and it raises the amplifier's transmit lines for as long as a frame is going. */

static struct tern_radio chip;
static bool amp_sending;

static bool has_amp(void) { return B->amp.power != BOARD_NO_PIN; }

static void amp_tx(bool on) {
    for (size_t i = 0; i < sizeof B->amp.tx / sizeof B->amp.tx[0]; i++) {
        set(B->amp.tx[i], on ? 1 : 0);
    }
    amp_sending = on;
}

/* The amplifier's lines as outputs, let go of if deep sleep held them. */
static bool amp_pins(void) {
    uint64_t pins = bit(B->amp.power) | bit(B->amp.enable);
    for (size_t i = 0; i < sizeof B->amp.tx / sizeof B->amp.tx[0]; i++) {
        pins |= bit(B->amp.tx[i]);
        release(B->amp.tx[i]);
    }
    release(B->amp.power);
    release(B->amp.enable);
    gpio_config_t out = {.pin_bit_mask = pins, .mode = GPIO_MODE_OUTPUT};
    return gpio_config(&out) == ESP_OK;
}

static bool amp_init(void) {
    if (!has_amp()) {
        return true;
    }
    if (!amp_pins()) {
        return false;
    }
    amp_tx(false);
    set(B->amp.power, 1);
    set(B->amp.enable, 1);
    esp_rom_delay_us(1000); /* the LDO's start, many times over */
    return true;
}

/* Unpowered, and held so through deep sleep, where the power line's pull-up would turn it on. The
 * board may be turning off before the radio started (an empty battery, main.c's halt()), so the
 * lines are made outputs here too. */
static void amp_off(void) {
    if (!has_amp()) {
        return;
    }
    (void)amp_pins();
    amp_tx(false);
    set(B->amp.enable, 0);
    set(B->amp.power, 0);
    for (size_t i = 0; i < sizeof B->amp.tx / sizeof B->amp.tx[0]; i++) {
        hold(B->amp.tx[i]);
    }
    hold(B->amp.enable);
    hold(B->amp.power);
}

static int amp_configure(void *ctx, const struct tern_radio_config *cfg) {
    (void)ctx;
    struct tern_radio_config at_chip = *cfg;
    at_chip.tx_power_dbm = board_chip_dbm(B, cfg->tx_power_dbm);
    return chip.ops->configure(chip.ctx, &at_chip);
}

static int amp_transmit(void *ctx, const uint8_t *frame, uint8_t len) {
    (void)ctx;
    amp_tx(true);
    int err = chip.ops->transmit(chip.ctx, frame, len);
    if (err != TERN_OK) {
        amp_tx(false);
    }
    return err;
}

static int amp_receive(void *ctx) {
    (void)ctx;
    amp_tx(false);
    return chip.ops->receive(chip.ctx);
}

static int amp_standby(void *ctx) {
    (void)ctx;
    amp_tx(false);
    return chip.ops->standby(chip.ctx);
}

static int amp_poll(void *ctx, struct tern_radio_event *ev) {
    (void)ctx;
    int n = chip.ops->poll(chip.ctx, ev);
    if (n == 1 && ev->kind == TERN_RADIO_TX_DONE && amp_sending) {
        amp_tx(false); /* the chip is in standby once a frame has gone */
    }
    return n;
}

static int amp_receiving(void *ctx) {
    (void)ctx;
    return chip.ops->receiving != NULL ? chip.ops->receiving(chip.ctx) : 0;
}

static const struct tern_radio_ops amp_ops = {
    .configure = amp_configure,
    .transmit = amp_transmit,
    .receive = amp_receive,
    .standby = amp_standby,
    .poll = amp_poll,
    .receiving = amp_receiving,
};

struct tern_radio board_radio(struct tern_sx126x *radio) {
    chip = tern_sx126x_radio(radio);
    if (!has_amp()) {
        return chip;
    }
    return (struct tern_radio){.ops = &amp_ops, .ctx = NULL};
}

/* Set up the first time it is read, so that PRG works before the radio has started, or when it
 * never does (main.c's halt()). */
bool board_button(void) {
    static bool ready;
    if (!ready) {
        /* After board_off(), PRG woke the board as an RTC pin, and stays one until it is given
         * back. */
        rtc_gpio_deinit(B->button);
        gpio_config_t button = {.pin_bit_mask = bit(B->button),
                                .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE};
        if (gpio_config(&button) != ESP_OK) {
            return false;
        }
        ready = true;
    }
    return gpio_get_level(B->button) == 0;
}

void board_led(bool on) { set(B->led, on ? 1 : 0); }

/* --- The display ---------------------------------------------------------------------------- */

/* The first byte of each I2C write says what follows (SSD1306 datasheet, section 8.1.5). */
#define OLED_COMMANDS 0x00
#define OLED_DATA 0x40

static bool oled_send(const uint8_t *buf, size_t len) {
    return i2c_master_transmit(oled, buf, len, OLED_TIMEOUT_MS) == ESP_OK;
}

bool board_screen_init(void) {
    if (B->screen.sda == BOARD_NO_PIN) {
        return false;
    }
    gpio_config_t out = {.pin_bit_mask = bit(B->vext) | bit(B->screen.reset),
                         .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&out) != ESP_OK) {
        return false;
    }
    /* Power, then hold RES# low for more than the 3 us the datasheet asks (section 8.9). Vext was
     * held off through deep sleep if the board turned itself off (board_off()). */
    if (B->vext != BOARD_NO_PIN) {
        gpio_hold_dis(B->vext);
    }
    gpio_deep_sleep_hold_dis();
    set(B->vext, VEXT_ON);
    set(B->screen.reset, 0);
    esp_rom_delay_us(20000);
    set(B->screen.reset, 1);
    esp_rom_delay_us(10000);

    i2c_master_bus_config_t bus = {.i2c_port = -1,
                                   .sda_io_num = B->screen.sda,
                                   .scl_io_num = B->screen.scl,
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
    gpio_config_t ctrl = {.pin_bit_mask = bit(B->battery.enable), .mode = GPIO_MODE_OUTPUT};
    if (B->battery.sense == BOARD_NO_PIN ||
        (B->battery.enable != BOARD_NO_PIN && gpio_config(&ctrl) != ESP_OK) ||
        adc_oneshot_io_to_channel(B->battery.sense, &unit, &battery_channel) != ESP_OK) {
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

/* The battery's millivolts with its switch at `level`, or 0 if the ADC would not say. */
static uint16_t battery_at(int level) {
    int sum = 0;
    set(B->battery.enable, level);
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
    long mv = at_pin * (B->battery.top_k + B->battery.bottom_k) / B->battery.bottom_k;
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
    set(B->battery.enable, sense.known && !sense.high_enables ? 1 : 0);
    return mv;
}

/* --- Turning off ----------------------------------------------------------------------------- */

bool board_woke_by_timer(void) { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }

/* Deep sleep: only the RTC domain stays up, to watch PRG (an RTC pin, GPIO0 on the Heltecs) and
 * the timer. The display's power is held off through it, since Vext (GPIO36) is not an RTC pin and
 * would otherwise float, and so is an amplifier's. PRG must be let go first, or the press that
 * turned the board off would wake it again. Waking is a restart: app_main() runs from the top. */
void board_off(uint32_t wake_after_s) {
    set(B->led, 0);
    set(B->vext, !VEXT_ON);
    hold(B->vext);
    amp_off();
    gpio_deep_sleep_hold_en();
    while (board_button()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50)); /* the contacts settle */
    rtc_gpio_pullup_en(B->button);
    rtc_gpio_pulldown_dis(B->button);
    esp_sleep_enable_ext0_wakeup(B->button, 0);
    if (wake_after_s != 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_after_s * 1000000u);
    }
    esp_deep_sleep_start();
}

/* --- The console ----------------------------------------------------------------------------- */

/* UART0, through the board's USB-to-serial chip, or the ESP32-S3's own USB Serial/JTAG port on a
 * board without one; the build's console says which (sdkconfig). printf() goes the same way. */

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

bool board_console_init(void) {
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = 512, .tx_buffer_size = 512};
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        return false;
    }
    /* printf() through the driver too, or the two would fight over the port. */
    usb_serial_jtag_vfs_use_driver();
    return true;
}

bool board_console_read(uint8_t *c) { return usb_serial_jtag_read_bytes(c, 1, 0) == 1; }

void board_console_write(const uint8_t *buf, size_t len) {
    /* With no computer listening the port does not drain, so nothing waits on it. */
    if (usb_serial_jtag_is_connected()) {
        (void)usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(50));
    }
}

void board_console_flush(uint32_t ms) {
    if (usb_serial_jtag_is_connected()) {
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(ms));
    }
}

#else

#define CONSOLE UART_NUM_0

bool board_console_init(void) { return uart_driver_install(CONSOLE, 512, 0, 0, NULL, 0) == ESP_OK; }

bool board_console_read(uint8_t *c) { return uart_read_bytes(CONSOLE, c, 1, 0) == 1; }

void board_console_write(const uint8_t *buf, size_t len) {
    (void)uart_write_bytes(CONSOLE, buf, len);
}

void board_console_flush(uint32_t ms) { (void)uart_wait_tx_done(CONSOLE, pdMS_TO_TICKS(ms)); }

#endif
