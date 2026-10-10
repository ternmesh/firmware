#include "board.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "boards.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "epd.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "tern/err.h"
#include "tern/sx126x.h"
#include "tern/sx127x.h"

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
#define I2C_TIMEOUT_MS 50
#define RAILS_SETTLE_US 10000 /* a rail's coming up, many times over */

#define BUSY_TIMEOUT_US 100000 /* far longer than any command; calibration takes a few ms */

static spi_device_handle_t spi;
static struct tern_sx126x sx126x;
static struct tern_sx127x sx127x;
static i2c_master_dev_handle_t oled;
static i2c_master_dev_handle_t pmu_dev;
static struct axp pmu;
static bool pmu_open_ok, pmu_rails_on;
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

static const struct board_def *board_def(void);
static bool pmu_start(void);
static bool amp_init(void);
static void amp_off(void);

/* Vext, on: let go of if deep sleep held it off (board_off()). */
static bool vext_on(void) {
    if (B->vext == BOARD_NO_PIN) {
        return true;
    }
    gpio_config_t out = {.pin_bit_mask = bit(B->vext), .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&out) != ESP_OK) {
        return false;
    }
    gpio_hold_dis(B->vext);
    gpio_deep_sleep_hold_dis();
    set(B->vext, VEXT_ON);
    return true;
}

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

static bool is_sx127x(void) { return B->lora.chip != BOARD_SX1262; }

static int bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    /* The SX1262 raises BUSY while it works on the last command and takes no new one until it
     * drops (datasheet section 8.3.1). An SX127x has no BUSY, and takes each access at once. */
    int64_t start = esp_timer_get_time();
    while (B->lora.busy != BOARD_NO_PIN && gpio_get_level(B->lora.busy)) {
        if (esp_timer_get_time() - start > BUSY_TIMEOUT_US) {
            return TERN_EIO;
        }
    }
    spi_transaction_t t = {.length = 8 * len, .tx_buffer = tx, .rx_buffer = rx};
    return spi_device_polling_transmit(spi, &t) == ESP_OK ? TERN_OK : TERN_EIO;
}

int board_init(void) {
    gpio_config_t out = {.pin_bit_mask = bit(B->lora.reset) | bit(B->led),
                         .mode = GPIO_MODE_OUTPUT};
    gpio_config_t in = {.pin_bit_mask = bit(B->lora.busy), .mode = GPIO_MODE_INPUT};
    if (gpio_config(&out) != ESP_OK || (in.pin_bit_mask != 0 && gpio_config(&in) != ESP_OK)) {
        return TERN_EIO;
    }
    board_led(false);
    if (B->vext_always && !vext_on()) {
        return TERN_EIO;
    }
    /* A radio powered from the power management chip is powered first. */
    if (B->pmu.chip != AXP_NONE && !pmu_start()) {
        return TERN_EIO;
    }

    spi_bus_config_t bus = {.sclk_io_num = B->lora.sck,
                            .mosi_io_num = B->lora.mosi,
                            .miso_io_num = B->lora.miso,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = 300};
    /* 8 MHz: the SX1262 takes 16, an SX127x 10 (their datasheets' SPI timing). */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 8 * 1000 * 1000, .mode = 0, .spics_io_num = B->lora.nss, .queue_size = 1};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(SPI2_HOST, &dev, &spi) != ESP_OK) {
        return TERN_EIO;
    }

    /* Hold NRESET low for over 100 us, then let the chip start: 5 ms is what an SX127x asks
     * (its datasheet, page 117), and an SX1262 is ready sooner (its section 8.1). */
    gpio_set_level(B->lora.reset, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(B->lora.reset, 1);
    esp_rom_delay_us(10000);

    if (!amp_init()) {
        return TERN_EIO;
    }

    if (is_sx127x()) {
        struct tern_sx127x_bus sb = {.ctx = NULL, .transfer = bus_transfer, .now = bus_now};
        struct tern_sx127x_board wiring = {.chip = B->lora.chip == BOARD_SX1278 ? TERN_SX1278
                                                                                : TERN_SX1276,
                                           .pa_boost = B->lora.pa_boost,
                                           .tcxo = B->lora.tcxo_mv != 0};
        return tern_sx127x_init(&sx127x, &sb, &wiring);
    }
    struct tern_sx126x_bus sb = {.ctx = NULL, .transfer = bus_transfer, .now = bus_now};
    struct tern_sx126x_board wiring = {
        .tcxo_mv = B->lora.tcxo_mv, .dio2_rf_switch = B->lora.dio2_rf_switch, .dcdc = true};
    return tern_sx126x_init(&sx126x, &sb, &wiring);
}

void board_radio_counts(struct board_radio_counts *c) {
    if (is_sx127x()) {
        const struct tern_sx127x_counts *k = &sx127x.counts;
        *c = (struct board_radio_counts){k->preambles, k->headers, k->header_errors, k->crc_errors,
                                         k->frames};
    } else {
        const struct tern_sx126x_counts *k = &sx126x.counts;
        *c = (struct board_radio_counts){k->preambles, k->headers, k->header_errors, k->crc_errors,
                                         k->frames};
    }
}

void board_radio_counts_reset(void) {
    sx126x.counts = (struct tern_sx126x_counts){0};
    sx127x.counts = (struct tern_sx127x_counts){0};
}

/* Whether board_init() has given the driver its bus, which it does before it first speaks to the
 * chip. */
static bool radio_started(void) {
    return is_sx127x() ? sx127x.bus.transfer != NULL : sx126x.bus.transfer != NULL;
}

void board_radio_sleep(void) {
    if (!radio_started()) {
        (void)board_init();
    }
    if (radio_started()) {
        (void)(is_sx127x() ? tern_sx127x_sleep(&sx127x) : tern_sx126x_sleep(&sx126x));
    }
}

/* --- The amplifier --------------------------------------------------------------------------- */

/* A board with an amplifier after its radio (boards.h) is driven through a radio that wraps the
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

struct tern_radio board_radio(void) {
    chip = is_sx127x() ? tern_sx127x_radio(&sx127x) : tern_sx126x_radio(&sx126x);
    if (!has_amp()) {
        return chip;
    }
    return (struct tern_radio){.ops = &amp_ops, .ctx = NULL};
}

/* Whether a pin has a pull-up of its own: the ESP32's GPIO34 to GPIO39 have none, and a button on
 * one has its pull-up on the board. */
static bool can_pull_up(int pin) { return !(B->soc == BOARD_ESP32 && pin >= 34); }

/* Set up the first time it is read, so that PRG works before the radio has started, or when it
 * never does (main.c's halt()). */
bool board_button(void) {
    static bool ready;
    if (B->button == BOARD_NO_PIN) {
        return false;
    }
    if (!ready) {
        /* After board_off(), PRG woke the board as an RTC pin, and stays one until it is given
         * back. */
        rtc_gpio_deinit(B->button);
        gpio_config_t button = {.pin_bit_mask = bit(B->button),
                                .mode = GPIO_MODE_INPUT,
                                .pull_up_en = can_pull_up(B->button) ? GPIO_PULLUP_ENABLE
                                                                     : GPIO_PULLUP_DISABLE};
        if (gpio_config(&button) != ESP_OK) {
            return false;
        }
        ready = true;
    }
    return gpio_get_level(B->button) == 0;
}

void board_led(bool on) { set(B->led, on != B->led_low_on ? 1 : 0); }

/* --- I2C and the power management chip ------------------------------------------------------ */

/* A board's screen and its power management chip may share a bus: each pair of pins is one bus,
 * made the first time it is asked for. */
static i2c_master_bus_handle_t i2c_bus(int sda, int scl) {
    static struct {
        int sda, scl;
        i2c_master_bus_handle_t handle;
    } buses[2];
    static size_t made;
    for (size_t i = 0; i < made; i++) {
        if (buses[i].sda == sda && buses[i].scl == scl) {
            return buses[i].handle;
        }
    }
    i2c_master_bus_config_t cfg = {.i2c_port = -1,
                                   .sda_io_num = sda,
                                   .scl_io_num = scl,
                                   .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true};
    i2c_master_bus_handle_t handle;
    if (made == sizeof buses / sizeof buses[0] || i2c_new_master_bus(&cfg, &handle) != ESP_OK) {
        return NULL;
    }
    buses[made].sda = sda;
    buses[made].scl = scl;
    buses[made].handle = handle;
    made++;
    return handle;
}

static bool pmu_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len) {
    (void)ctx;
    return i2c_master_transmit_receive(pmu_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool pmu_write(void *ctx, uint8_t reg, uint8_t value) {
    (void)ctx;
    uint8_t buf[] = {reg, value};
    return i2c_master_transmit(pmu_dev, buf, sizeof buf, I2C_TIMEOUT_MS) == ESP_OK;
}

/* The chip, spoken to: the first time it is asked for. */
static bool pmu_open(void) {
    if (!pmu_open_ok && B->pmu.chip != AXP_NONE) {
        i2c_master_bus_handle_t bus = i2c_bus(B->pmu.sda, B->pmu.scl);
        i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = AXP_ADDRESS,
                                   .scl_speed_hz = 400000};
        struct axp_bus ab = {.ctx = NULL, .read = pmu_read, .write = pmu_write};
        pmu_open_ok =
            bus != NULL &&
            (pmu_dev != NULL || i2c_master_bus_add_device(bus, &dev, &pmu_dev) == ESP_OK) &&
            axp_init(&pmu, &ab, B->pmu.chip);
    }
    return pmu_open_ok;
}

/* Its rails as the board lists them, the radio's and the screen's among them: by whichever of the
 * radio, the screen and the battery starts first. */
static bool pmu_start(void) {
    if (pmu_rails_on) {
        return true;
    }
    if (!pmu_open()) {
        return false;
    }
    for (size_t i = 0; i < sizeof B->pmu.rails / sizeof B->pmu.rails[0]; i++) {
        const struct board_rail *r = &B->pmu.rails[i];
        if (r->rail != AXP_RAIL_NONE && !axp_set_rail(&pmu, r->rail, r->mv)) {
            return false;
        }
    }
    esp_rom_delay_us(RAILS_SETTLE_US);
    pmu_rails_on = true;
    return true;
}

/* Every rail the board lists, off, for a board turning itself off. */
static void pmu_off(void) {
    if (!pmu_open()) {
        return;
    }
    for (size_t i = 0; i < sizeof B->pmu.rails / sizeof B->pmu.rails[0]; i++) {
        if (B->pmu.rails[i].rail != AXP_RAIL_NONE) {
            (void)axp_set_rail(&pmu, B->pmu.rails[i].rail, 0);
        }
    }
    pmu_rails_on = false;
}

/* --- The display ---------------------------------------------------------------------------- */

/* The first byte of each I2C write says what follows: the SSD1306's datasheet, section 8.1.5,
 * and the SH1106's, its I2C interface, agree. */
#define OLED_COMMANDS 0x00
#define OLED_DATA 0x40

/* The board's controller, unless the build says an SH1106 is fitted to its header in place of
 * the SSD1306 the board is listed with. */
static enum board_screen_chip oled_chip(void) {
#if CONFIG_TERN_SCREEN_SH1106
    if (B->screen.chip == BOARD_SSD1306) {
        return BOARD_SH1106;
    }
#endif
    return B->screen.chip;
}

static bool oled_send(const uint8_t *buf, size_t len) {
    return i2c_master_transmit(oled, buf, len, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool epd_init(void);
static bool epd_page(int page, const uint8_t data[128]);
static bool epd_power(bool on);

bool board_screen_init(void) {
    if (!board_has_screen(B)) {
        return false;
    }
    if (board_epaper(B)) {
        return epd_init();
    }
    if (B->pmu.chip != AXP_NONE && !pmu_start()) {
        return false; /* the screen may be on one of its rails */
    }
    gpio_config_t out = {.pin_bit_mask = bit(B->screen.reset), .mode = GPIO_MODE_OUTPUT};
    if (!vext_on() || (out.pin_bit_mask != 0 && gpio_config(&out) != ESP_OK)) {
        return false;
    }
    /* Power, then hold RES# low for more than the 3 us the datasheet asks (section 8.9). A panel
     * without a RES# pin resets itself as its supply comes up. */
    set(B->screen.reset, 0);
    esp_rom_delay_us(20000);
    set(B->screen.reset, 1);
    esp_rom_delay_us(10000);

    i2c_master_bus_handle_t handle = i2c_bus(B->screen.sda, B->screen.scl);
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = OLED_ADDRESS,
                               .scl_speed_hz = 400000};
    if (handle == NULL || i2c_master_probe(handle, OLED_ADDRESS, I2C_TIMEOUT_MS) != ESP_OK ||
        i2c_master_bus_add_device(handle, &dev, &oled) != ESP_OK) {
        return false;
    }

    /* The SSD1306 datasheet's software set-up (its application note's flow), for a 128x64 panel
     * whose charge pump is on the chip. */
    /* clang-format off */
    static const uint8_t ssd1306_setup[] = {
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
    /* The SH1106's (Sino Wealth's datasheet, V2.6): the same panel, set up only with the commands
     * it lists, at their reset values where the SSD1306's were tuned for that chip. It has no
     * addressing modes, only a page at a time, and its DC-DC is set while the display is off. */
    static const uint8_t sh1106_setup[] = {
        OLED_COMMANDS,
        0xAE,       /* display off while it is set up */
        0xD5, 0x50, /* clock: the reset default */
        0xA8, 0x3F, /* 64 rows */
        0xD3, 0x00, /* no vertical offset */
        0x40,       /* start at row 0 */
        0xAD, 0x8B, /* DC-DC on with the display */
#if CONFIG_TERN_SCREEN_FLIP
        0xA0, 0xC0,
#else
        0xA1, 0xC8,
#endif
        0xDA, 0x12, /* the panel's rows wired alternately */
        0x81, 0x80, /* contrast: the reset default */
        0xD9, 0x22, /* discharge and pre-charge: the reset defaults */
        0xDB, 0x35, /* VCOM deselect: the reset default */
        0xA4,       /* show what is in RAM */
        0xA6,       /* light on dark */
    };
    /* clang-format on */
    static const uint8_t on[] = {OLED_COMMANDS, 0xAF};
    static const uint8_t blank[128];
    bool sh1106 = oled_chip() == BOARD_SH1106;
    if (!(sh1106 ? oled_send(sh1106_setup, sizeof sh1106_setup)
                 : oled_send(ssd1306_setup, sizeof ssd1306_setup))) {
        return false;
    }
    for (int page = 0; page < 8; page++) {
        if (!board_screen_page(page, blank)) {
            return false;
        }
    }
    if (!oled_send(on, sizeof on)) {
        return false;
    }
    if (sh1106) {
        esp_rom_delay_us(100000); /* its DC-DC settles before the first frame (its power-on flow) */
    }
    return true;
}

/* The SH1106 has 132 columns of RAM to the panel's 128. Its datasheet does not say which the panel
 * is bonded to; a panel centred on them starts at column 2, whichever way the columns run. */
#define SH1106_FIRST_COLUMN 2

bool board_screen_page(int page, const uint8_t data[128]) {
    if (board_epaper(B)) {
        return epd_page(page, data);
    }
    /* The page, then its first column, low half and high half. */
    uint8_t column = oled_chip() == BOARD_SH1106 ? SH1106_FIRST_COLUMN : 0;
    uint8_t where[] = {OLED_COMMANDS, (uint8_t)(0xB0 | (page & 7)), (uint8_t)(column & 0x0F),
                       (uint8_t)(0x10 | (column >> 4))};
    uint8_t buf[1 + 128];
    buf[0] = OLED_DATA;
    memcpy(&buf[1], data, 128);
    return oled_send(where, sizeof where) && oled_send(buf, sizeof buf);
}

bool board_screen_power(bool on) {
    if (board_epaper(B)) {
        return epd_power(on);
    }
    /* The SSD1306's charge pump goes on before the panel and off after it (the datasheet's
     * application note on the charge pump). The SH1106's DC-DC follows the display by itself, set
     * up as it is: off stops it and on starts it. */
    static const uint8_t off_seq[] = {OLED_COMMANDS, 0xAE, 0x8D, 0x10};
    static const uint8_t on_seq[] = {OLED_COMMANDS, 0x8D, 0x14, 0xAF};
    static const uint8_t sh1106_off[] = {OLED_COMMANDS, 0xAE};
    static const uint8_t sh1106_on[] = {OLED_COMMANDS, 0xAF};
    if (oled_chip() == BOARD_SH1106) {
        return on ? oled_send(sh1106_on, sizeof sh1106_on)
                  : oled_send(sh1106_off, sizeof sh1106_off);
    }
    return on ? oled_send(on_seq, sizeof on_seq) : oled_send(off_seq, sizeof off_seq);
}

/* --- The e-paper panel ----------------------------------------------------------------------- */

/* An SSD1680 (Solomon Systech's datasheet, Rev 1.4) on a DKE DEPG0290BNS800F6 (its specification),
 * on SPI: a byte is a command while DC is low and data while it is high. It keeps its picture with
 * no power at all, and redraws it whole, which takes seconds and flashes the panel; so the picture
 * the node sends a page at a time is gathered here, and drawn by board_screen_poll() once it has
 * stopped arriving: soon after something the user did, otherwise no more often than
 * CONFIG_TERN_EPAPER_REFRESH_S. A refresh is started and left to run, the panel holding BUSY high
 * until it is done; it must not be interrupted (section 8.1, 0x20). */

#define EPD_SPI_HZ 4000000              /* it takes 20 MHz (DKE, page 14) */
#define EPD_POWER_US 10000              /* VCI on, then 10 ms (SSD1680, section 9.1) */
#define EPD_RESET_US 10000              /* RES# low and high, far more than DKE's 200 us each */
#define EPD_RESET_TIMEOUT_US 100000     /* the software reset, a few ms */
#define EPD_REFRESH_TIMEOUT_US 10000000 /* a refresh, about 4 s at 25 C (DKE, page 9) */
#define EPD_SETTLE_NS 200000000LL       /* a picture still arriving a page a turn */
#define EPD_RETRY_NS 10000000000LL      /* after a draw that failed: its reset can take 100 ms */

#if CONFIG_TERN_SCREEN_FLIP
#define EPD_FLIP true
#else
#define EPD_FLIP false
#endif

static spi_device_handle_t epd_spi;
/* On the heap, and only on a board with e-paper: a classic ESP32's static memory is short. */
#define EPD_FRAME (EPD_LINES * EPD_LINE_BYTES)
static struct display *epd_pic;
static uint8_t *epd_frame;
static bool epd_open;       /* its pins and bus are set up */
static bool epd_awake;      /* reset and set up, not in deep sleep */
static bool epd_changed;    /* the picture differs from what the panel shows */
static bool epd_refreshing; /* a refresh has been started and BUSY not yet seen low */
static bool epd_dark;       /* the node has turned the screen off */
static tern_time epd_page_at, epd_drawn_at;
static tern_time epd_retry_at; /* after a draw that failed, not before this */

static bool epd_busy(void) { return gpio_get_level(B->screen.busy) == 1; }

static bool epd_wait(int64_t timeout_us) {
    int64_t start = esp_timer_get_time();
    while (epd_busy()) {
        if (esp_timer_get_time() - start > timeout_us) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

static bool epd_send(bool data, const uint8_t *buf, size_t len) {
    set(B->screen.dc, data ? 1 : 0);
    spi_transaction_t t = {.length = 8 * len, .tx_buffer = buf};
    return len == 0 || spi_device_polling_transmit(epd_spi, &t) == ESP_OK;
}

static bool epd_cmd(uint8_t c, const uint8_t *args, size_t len) {
    return epd_send(false, &c, 1) && epd_send(true, args, len);
}

#define EPD(c, ...)                                                                                \
    epd_cmd((c), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

/* Power, a hardware reset, then the software one, which also brings it out of deep sleep, and the
 * temperature sensor on the chip, which the waveforms are chosen by (section 9.1). The rest, the
 * gates, the RAM's window and the order it is filled in, are the module's own once reset (DKE,
 * pages 15 to 17): a window of bytes 1 to 16 across, lines 295 down to 0. */
static bool epd_wake(void) {
    if (!vext_on()) {
        return false;
    }
    esp_rom_delay_us(EPD_POWER_US);
    set(B->screen.reset, 0);
    esp_rom_delay_us(EPD_RESET_US);
    set(B->screen.reset, 1);
    esp_rom_delay_us(EPD_RESET_US);
    if (!epd_wait(EPD_RESET_TIMEOUT_US) || !epd_cmd(0x12, NULL, 0) ||
        !epd_wait(EPD_RESET_TIMEOUT_US) || !EPD(0x18, 0x80)) {
        return false;
    }
    epd_awake = true;
    return true;
}

/* Fills the panel's RAM from its first byte and line, and starts a full refresh: the temperature
 * read, the waveform loaded from OTP, the panel driven (0x22 0xF7, section 8.1). */
static bool epd_draw_now(void) {
    if (!epd_awake && !epd_wake()) {
        return false;
    }
    for (int n = 0; n < EPD_LINES; n++) {
        epd_line(epd_pic, n, EPD_FLIP, &epd_frame[n * EPD_LINE_BYTES]);
    }
    if (!EPD(0x4E, 0x01) || !EPD(0x4F, 0x27, 0x01) || !epd_cmd(0x24, epd_frame, EPD_FRAME) ||
        !EPD(0x22, 0xF7) || !epd_cmd(0x20, NULL, 0)) {
        epd_awake = false; /* reset again before the next */
        return false;
    }
    epd_changed = false;
    epd_refreshing = true;
    epd_drawn_at = board_now();
    return true;
}

/* A draw that fails is tried again no sooner than EPD_RETRY_NS, so a panel that has stopped
 * answering costs the loop a reset now and then rather than every turn. */
static bool epd_draw(void) {
    if (board_now() < epd_retry_at) {
        return false;
    }
    if (epd_draw_now()) {
        return true;
    }
    epd_retry_at = board_now() + EPD_RETRY_NS;
    return false;
}

/* Deep sleep, mode 1, keeping its RAM; only a hardware reset brings it out (section 8.1, 0x10). */
static void epd_sleep(void) {
    if (epd_awake && EPD(0x10, 0x01)) {
        epd_awake = false;
    }
}

static bool epd_init(void) {
    gpio_config_t out = {.pin_bit_mask = bit(B->screen.dc) | bit(B->screen.reset),
                         .mode = GPIO_MODE_OUTPUT};
    gpio_config_t in = {.pin_bit_mask = bit(B->screen.busy), .mode = GPIO_MODE_INPUT};
    spi_bus_config_t bus = {.sclk_io_num = B->screen.sck,
                            .mosi_io_num = B->screen.mosi,
                            .miso_io_num = -1,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = EPD_FRAME};
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ, .mode = 0, .spics_io_num = B->screen.cs, .queue_size = 1};
    epd_pic = calloc(1, sizeof *epd_pic);
    epd_frame = heap_caps_malloc(EPD_FRAME, MALLOC_CAP_DMA); /* sent by DMA as it is */
    if (epd_pic == NULL || epd_frame == NULL || gpio_config(&out) != ESP_OK ||
        gpio_config(&in) != ESP_OK ||
        spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(SPI3_HOST, &dev, &epd_spi) != ESP_OK) {
        return false;
    }
    epd_open = true;
    display_init(epd_pic);
    epd_changed = true; /* whatever it kept from before is not this */
    return epd_wake();
}

static bool epd_page(int page, const uint8_t data[128]) {
    if (memcmp(epd_pic->px[page & 7], data, DISPLAY_WIDTH) != 0) {
        memcpy(epd_pic->px[page & 7], data, DISPLAY_WIDTH);
        epd_changed = true;
    }
    epd_page_at = board_now();
    return true;
}

/* Off, it keeps showing its picture and is left asleep; on, it is drawn again only if the picture
 * has changed meanwhile. */
static bool epd_power(bool on) {
    epd_dark = !on;
    return true;
}

/* A refresh that has held BUSY far longer than one takes: the panel is reset before the next, and
 * the picture is drawn again, so a fault costs a picture rather than the screen. */
static bool epd_stuck(void) {
    if (board_now() - epd_drawn_at < (tern_time)EPD_REFRESH_TIMEOUT_US * 1000) {
        return false;
    }
    epd_awake = false;
    epd_changed = true;
    return true;
}

void board_screen_show(bool wait) {
    if (!board_epaper(B) || !epd_open || epd_dark) {
        return;
    }
    if (epd_refreshing && epd_busy()) {
        if (!wait && !epd_stuck()) {
            return; /* drawn when the next flush or poll finds it done */
        }
        if (wait && !epd_wait(EPD_REFRESH_TIMEOUT_US)) {
            epd_stuck();
        }
    }
    epd_refreshing = false;
    if (epd_changed && epd_draw() && wait) {
        (void)epd_wait(EPD_REFRESH_TIMEOUT_US);
        epd_refreshing = false;
    }
}

void board_screen_poll(bool prompt) {
    if (!board_epaper(B) || !epd_open) {
        return;
    }
    if (epd_refreshing) {
        if (epd_busy() && !epd_stuck()) {
            return;
        }
        epd_refreshing = false;
    }
    if (epd_dark) {
        epd_sleep();
        return;
    }
    tern_time now = board_now();
    if (!epd_changed || now - epd_page_at < EPD_SETTLE_NS) {
        return;
    }
    if (!prompt && epd_drawn_at != 0 &&
        now - epd_drawn_at < (tern_time)CONFIG_TERN_EPAPER_REFRESH_S * 1000000000LL) {
        return;
    }
    (void)epd_draw();
}

/* Before the board turns off: the last picture drawn and the panel asleep, since its supply is
 * about to go and a refresh must not be cut short. */
static void epd_finish(void) {
    if (!board_epaper(B) || !epd_open) {
        return;
    }
    if ((epd_refreshing && !epd_wait(EPD_REFRESH_TIMEOUT_US)) ||
        (epd_changed && (!epd_draw() || !epd_wait(EPD_REFRESH_TIMEOUT_US)))) {
        return;
    }
    epd_refreshing = false;
    epd_sleep();
}

/* The ADC's ranges, each the most it reads at that attenuation (ESP-IDF's ADC oneshot guide, for
 * each chip), the least first: a battery is read in the narrowest that holds a full cell through
 * the board's divider, with a tenth to spare. 390k over 100k, the Heltecs', puts 4.2 V at 0.86 V;
 * an even divider, at 2.1 V. */
static const struct {
    adc_atten_t atten;
    uint16_t most_mv;
} adc_ranges[] = {
    {ADC_ATTEN_DB_2_5, 1250},
    {ADC_ATTEN_DB_6, 1750},
#if CONFIG_IDF_TARGET_ESP32
    {ADC_ATTEN_DB_12, 2450},
#else
    {ADC_ATTEN_DB_12, 3100},
#endif
};

static adc_atten_t battery_atten;
static uint16_t battery_range_mv;

bool board_battery_init(void) {
    if (B->pmu.chip != AXP_NONE) {
        return pmu_start(); /* the chip measures it */
    }
    adc_unit_t unit;
    gpio_config_t ctrl = {.pin_bit_mask = bit(B->battery.enable), .mode = GPIO_MODE_OUTPUT};
    if (B->battery.sense == BOARD_NO_PIN ||
        (B->battery.enable != BOARD_NO_PIN && gpio_config(&ctrl) != ESP_OK) ||
        adc_oneshot_io_to_channel(B->battery.sense, &unit, &battery_channel) != ESP_OK ||
        (B->vext_always && !vext_on())) {
        return false;
    }
    size_t r = 0;
    while (r + 1 < sizeof adc_ranges / sizeof adc_ranges[0] &&
           board_battery_pin_mv(B) * 11 / 10 > adc_ranges[r].most_mv) {
        r++;
    }
    battery_atten = adc_ranges[r].atten;
    battery_range_mv = adc_ranges[r].most_mv;
    adc_oneshot_unit_init_cfg_t init = {.unit_id = unit};
    adc_oneshot_chan_cfg_t chan = {.atten = battery_atten, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&init, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, battery_channel, &chan) != ESP_OK) {
        return false;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {.unit_id = unit,
                                           .chan = battery_channel,
                                           .atten = battery_atten,
                                           .bitwidth = ADC_BITWIDTH_DEFAULT};
    have_cali = adc_cali_create_scheme_curve_fitting(&cal, &adc_cali) == ESP_OK;
#else
    /* The classic ESP32 fits a line, through what its eFuses hold, or failing those its nominal
     * 1.1 V reference. */
    adc_cali_line_fitting_config_t cal = {.unit_id = unit,
                                          .atten = battery_atten,
                                          .bitwidth = ADC_BITWIDTH_DEFAULT,
                                          .default_vref = 1100};
    have_cali = adc_cali_create_scheme_line_fitting(&cal, &adc_cali) == ESP_OK;
#endif
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
            mv = raw * battery_range_mv / 4095; /* uncalibrated: the range taken as even */
        }
        sum += mv;
    }
    long at_pin = sum / BATTERY_SAMPLES;
    long mv = at_pin * (B->battery.top_k + B->battery.bottom_k) / B->battery.bottom_k;
    return (uint16_t)(mv > UINT16_MAX ? UINT16_MAX : mv);
}

uint16_t board_battery_mv(void) {
    if (B->pmu.chip != AXP_NONE) {
        uint16_t mv;
        bool charging;
        return pmu_rails_on && axp_battery(&pmu, &mv, &charging) && mv >= POWER_NONE_MV ? mv : 0;
    }
    if (!have_adc) {
        return 0;
    }
    if (B->battery.enable == BOARD_NO_PIN) {
        /* A divider always connected, or on Vext with everything else. */
        uint16_t mv = battery_at(0);
        return mv < POWER_NONE_MV ? 0 : mv;
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
 * would otherwise float, and so is an amplifier's; a power management chip's rails are turned off.
 * PRG must be let go first, or the press that turned the board off would wake it again. Waking is
 * a restart: app_main() runs from the top. */
void board_off(uint32_t wake_after_s) {
    epd_finish();
    board_led(false);
    set(B->vext, !VEXT_ON);
    hold(B->vext);
    amp_off();
    pmu_off();
    gpio_deep_sleep_hold_en();
    while (board_button()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50)); /* the contacts settle */
    if (B->button != BOARD_NO_PIN) {
        if (can_pull_up(B->button)) {
            rtc_gpio_pullup_en(B->button);
            rtc_gpio_pulldown_dis(B->button);
        }
        esp_sleep_enable_ext0_wakeup(B->button, 0);
    }
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
