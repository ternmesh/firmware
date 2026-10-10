#include "board.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>

#include "power.h"
#include "tern/err.h"
#include "tern/sx126x.h"

/* An nRF52840 board wired to an SX1262, read from its devicetree: Zephyr's board for it, and this
 * port's overlay on it in boards/, which says what Zephyr's does not or overrides it where the
 * maker's documents disagree. So a board Zephyr has is an overlay and a line in Kconfig, not code
 * here. From the devicetree:
 *
 *   lora                 the SX1262 node (semtech,sx1262): its bus, reset-gpios, busy-gpios, its
 *                        TCXO's voltage (dio3-tcxo-voltage) and whether DIO2 drives the antenna
 *                        switch (dio2-tx-enable). Its antenna-enable-gpios, if it has them, are on
 *                        while the radio is, and its rx-enable-gpios and tx-enable-gpios while it
 *                        receives and while it sends, as Zephyr's own driver drives them.
 *   sw0, led0            the button and the LED, if the board has them
 *   zephyr,display       the screen, if it has one: a 128x64 monochrome panel whose memory is in
 *                        pages (the SSD1306 or SH1106), or a colour one the picture is drawn into
 *   vbatt                the battery's divider (voltage-divider): its ADC channel, its resistors,
 *                        and the switch that connects it (power-gpios), if any
 *   tft-en, tft-led-en,  switches a board's screen, backlight or external supply hangs on, if it
 *   vext-control         has them
 */

#define LORA DT_NODELABEL(lora)
#define VBATT DT_NODELABEL(vbatt)
#define NONE                                                                                       \
    { 0 }

static const struct spi_dt_spec lora_spi =
    SPI_DT_SPEC_GET(LORA, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_CONTROLLER);
static const struct gpio_dt_spec lora_reset = GPIO_DT_SPEC_GET(LORA, reset_gpios);
static const struct gpio_dt_spec lora_busy = GPIO_DT_SPEC_GET(LORA, busy_gpios);
static const struct gpio_dt_spec lora_ant = GPIO_DT_SPEC_GET_OR(LORA, antenna_enable_gpios, NONE);
static const struct gpio_dt_spec lora_rx = GPIO_DT_SPEC_GET_OR(LORA, rx_enable_gpios, NONE);
static const struct gpio_dt_spec lora_tx = GPIO_DT_SPEC_GET_OR(LORA, tx_enable_gpios, NONE);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, NONE);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, NONE);
static const struct gpio_dt_spec tft_en = GPIO_DT_SPEC_GET_OR(DT_ALIAS(tft_en), gpios, NONE);
static const struct gpio_dt_spec tft_led_en =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(tft_led_en), gpios, NONE);
static const struct gpio_dt_spec vext = GPIO_DT_SPEC_GET_OR(DT_ALIAS(vext_control), gpios, NONE);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#if DT_HAS_CHOSEN(zephyr_display)
static const struct device *const screen = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#endif

#if DT_NODE_EXISTS(VBATT)
static const struct adc_dt_spec battery = ADC_DT_SPEC_GET(VBATT);
static const struct gpio_dt_spec battery_switch = GPIO_DT_SPEC_GET_OR(VBATT, power_gpios, NONE);
#define DIVIDER_FULL DT_PROP(VBATT, full_ohms)
#define DIVIDER_OUTPUT DT_PROP(VBATT, output_ohms)
#endif

/* The TCXO's supply from DIO3, in millivolts, from Zephyr's code for it (dt-bindings/lora/
 * sx126x.h), or 0 for a crystal. */
static const uint16_t tcxo_mv[] = {1600, 1700, 1800, 2200, 2400, 2700, 3000, 3300};
#if DT_NODE_HAS_PROP(LORA, dio3_tcxo_voltage)
#define TCXO_MV tcxo_mv[DT_PROP(LORA, dio3_tcxo_voltage)]
#else
#define TCXO_MV 0
#endif

#define BATTERY_SETTLE_US 2000
#define BATTERY_SAMPLES 16
#define BUSY_TIMEOUT_US 100000

#define POWER_MIN_DBM (-9) /* the SX1262's high-power amplifier */
#define POWER_MAX_DBM 22

static struct power_sense sense;
static bool have_adc;
static struct tern_sx126x sx;

/* Sets a line the board may not have. */
static void set(const struct gpio_dt_spec *g, int value) {
    if (g->port != NULL) {
        (void)gpio_pin_set_dt(g, value);
    }
}

static bool output(const struct gpio_dt_spec *g, gpio_flags_t flags) {
    return g->port == NULL || gpio_pin_configure_dt(g, flags) == 0;
}

const char *board_title(void) { return CONFIG_TERN_BOARD_TITLE; }
int8_t board_power_min(void) { return POWER_MIN_DBM; }
int8_t board_power_max(void) { return POWER_MAX_DBM; }
bool board_power_ok(int dbm) { return dbm >= POWER_MIN_DBM && dbm <= POWER_MAX_DBM; }

tern_time board_now(void) { return (tern_time)k_ticks_to_ns_floor64(k_uptime_ticks()); }

/* --- The radio ------------------------------------------------------------------------------- */

static tern_time bus_now(void *ctx) {
    (void)ctx;
    return board_now();
}

static int bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    /* The chip raises BUSY while it works on the last command and takes no new one until it
     * drops (datasheet section 8.3.1). */
    int64_t start = k_uptime_ticks();
    while (gpio_pin_get_dt(&lora_busy) == 1) {
        if (k_ticks_to_us_floor64(k_uptime_ticks() - start) > BUSY_TIMEOUT_US) {
            return TERN_EIO;
        }
    }
    const struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
    const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    struct spi_buf rx_buf = {.buf = rx, .len = len};
    const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};
    return spi_transceive_dt(&lora_spi, &tx_set, rx != NULL ? &rx_set : NULL) == 0 ? TERN_OK
                                                                                   : TERN_EIO;
}

int board_init(void) {
    if (!spi_is_ready_dt(&lora_spi) || !gpio_is_ready_dt(&lora_reset) ||
        gpio_pin_configure_dt(&lora_reset, GPIO_OUTPUT_INACTIVE) != 0 ||
        gpio_pin_configure_dt(&lora_busy, GPIO_INPUT) != 0 || !output(&led, GPIO_OUTPUT_INACTIVE) ||
        !output(&lora_rx, GPIO_OUTPUT_INACTIVE) || !output(&lora_tx, GPIO_OUTPUT_INACTIVE) ||
        !output(&lora_ant, GPIO_OUTPUT_ACTIVE)) {
        return TERN_EIO;
    }
    /* Hold NRESET low for over 100 us (section 8.1), then let the chip start. */
    gpio_pin_set_dt(&lora_reset, 1);
    k_busy_wait(1000);
    gpio_pin_set_dt(&lora_reset, 0);
    k_busy_wait(10000);

    struct tern_sx126x_bus sb = {.ctx = NULL, .transfer = bus_transfer, .now = bus_now};
    struct tern_sx126x_board wiring = {
        .tcxo_mv = TCXO_MV, .dio2_rf_switch = DT_PROP(LORA, dio2_tx_enable), .dcdc = true};
    return tern_sx126x_init(&sx, &sb, &wiring);
}

/* A board whose antenna switch has lines of its own (rx-enable-gpios, tx-enable-gpios) is driven
 * through a radio that wraps the chip's and sets them: the one for receiving while it receives,
 * the one for sending while a frame goes, and neither otherwise, as Zephyr's driver sets them. */

static struct tern_radio chip;

static void switch_to(bool rx, bool tx) {
    set(&lora_tx, tx ? 1 : 0);
    set(&lora_rx, rx ? 1 : 0);
}

static int sw_configure(void *ctx, const struct tern_radio_config *cfg) {
    (void)ctx;
    return chip.ops->configure(chip.ctx, cfg);
}

static int sw_transmit(void *ctx, const uint8_t *frame, uint8_t len) {
    (void)ctx;
    switch_to(false, true);
    int err = chip.ops->transmit(chip.ctx, frame, len);
    if (err != TERN_OK) {
        switch_to(false, false);
    }
    return err;
}

static int sw_receive(void *ctx) {
    (void)ctx;
    switch_to(true, false);
    return chip.ops->receive(chip.ctx);
}

static int sw_standby(void *ctx) {
    (void)ctx;
    switch_to(false, false);
    return chip.ops->standby(chip.ctx);
}

static int sw_poll(void *ctx, struct tern_radio_event *ev) {
    (void)ctx;
    int n = chip.ops->poll(chip.ctx, ev);
    if (n == 1 && ev->kind == TERN_RADIO_TX_DONE) {
        switch_to(false, false); /* the chip is in standby once a frame has gone */
    }
    return n;
}

static int sw_receiving(void *ctx) {
    (void)ctx;
    return chip.ops->receiving != NULL ? chip.ops->receiving(chip.ctx) : 0;
}

static const struct tern_radio_ops sw_ops = {
    .configure = sw_configure,
    .transmit = sw_transmit,
    .receive = sw_receive,
    .standby = sw_standby,
    .poll = sw_poll,
    .receiving = sw_receiving,
};

struct tern_radio board_radio(void) {
    chip = tern_sx126x_radio(&sx);
    if (lora_rx.port == NULL && lora_tx.port == NULL) {
        return chip;
    }
    return (struct tern_radio){.ops = &sw_ops, .ctx = NULL};
}

void board_radio_counts(struct board_radio_counts *c) {
    *c = (struct board_radio_counts){.preambles = sx.counts.preambles,
                                     .headers = sx.counts.headers,
                                     .header_errors = sx.counts.header_errors,
                                     .crc_errors = sx.counts.crc_errors,
                                     .frames = sx.counts.frames};
}

void board_radio_counts_reset(void) { sx.counts = (struct tern_sx126x_counts){0}; }

void board_radio_sleep(void) {
    if (sx.bus.transfer == NULL) {
        (void)board_init();
    }
    if (sx.bus.transfer != NULL) {
        (void)tern_sx126x_sleep(&sx);
    }
    switch_to(false, false);
    set(&lora_ant, 0);
}

/* --- The button and the LED ------------------------------------------------------------------ */

bool board_button(void) {
    static bool ready;
    if (button.port == NULL) {
        return false;
    }
    if (!ready) {
        if (gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
            return false;
        }
        ready = true;
    }
    return gpio_pin_get_dt(&button) == 1;
}

void board_led(bool on) { set(&led, on ? 1 : 0); }

/* --- The screen ------------------------------------------------------------------------------ */

/* The node draws a 128x64 picture a page of eight rows at a time (display.h), a byte a column with
 * its lowest bit at the top. A monochrome panel that keeps its memory in such pages (Zephyr's
 * SCREEN_INFO_MONO_VTILED: the SSD1306 and the SH1106) takes each page as it is. A colour panel is
 * drawn into: the T114's 135x240 is mounted on its side, so the picture is turned to lie along it,
 * at one panel pixel a picture pixel, in its middle. That is small on a 1.14" panel; a layout of
 * its own is for later (docs/boards.md). */
#define PIC_W 128
#define PIC_H 64
#define LIT 0xFFFFu
#define DARK 0x0000u

#if DT_HAS_CHOSEN(zephyr_display)

static bool paged;                /* monochrome, in pages: each page goes as it is */
static int panel_w, panel_h;      /* a colour panel's size, its long side down */
static uint16_t strip[PIC_W * 8]; /* one page, turned: 8 panel columns by 128 panel rows */

static int panel_write(int x, int y, int w, int h, const void *pixels, size_t len) {
    struct display_buffer_descriptor d = {.buf_size = (uint32_t)len,
                                          .width = (uint16_t)w,
                                          .height = (uint16_t)h,
                                          .pitch = (uint16_t)w};
    return display_write(screen, (uint16_t)x, (uint16_t)y, &d, pixels);
}

bool board_screen_init(void) {
    if (!output(&tft_en, GPIO_OUTPUT_ACTIVE) || !output(&tft_led_en, GPIO_OUTPUT_INACTIVE)) {
        return false;
    }
    if (tft_en.port != NULL) {
        k_msleep(20); /* the panel's supply settles before its controller is reset and set up */
    }
    if (device_init(screen) != 0 && !device_is_ready(screen)) {
        return false;
    }
    struct display_capabilities caps;
    display_get_capabilities(screen, &caps);
    if ((caps.supported_pixel_formats & PIXEL_FORMAT_MONO01) != 0 &&
        (caps.screen_info & SCREEN_INFO_MONO_VTILED) != 0 &&
        (caps.screen_info & SCREEN_INFO_MONO_MSB_FIRST) == 0 && caps.x_resolution >= PIC_W &&
        caps.y_resolution >= PIC_H) {
        /* A set bit lit, as the node draws it. */
        if (display_set_pixel_format(screen, PIXEL_FORMAT_MONO01) != 0) {
            return false;
        }
        paged = true;
        static const uint8_t blank[PIC_W];
        for (int page = 0; page < PIC_H / 8; page++) {
            if (!board_screen_page(page, blank)) {
                return false;
            }
        }
    } else if (caps.current_pixel_format == PIXEL_FORMAT_RGB_565 &&
               caps.x_resolution >= PIC_H + 8 && caps.y_resolution >= PIC_W) {
        panel_w = caps.x_resolution;
        panel_h = caps.y_resolution;
        /* Dark all over. */
        for (size_t i = 0; i < sizeof strip / sizeof strip[0]; i++) {
            strip[i] = DARK;
        }
        for (int y = 0; y < panel_h; y++) {
            if (panel_write(0, y, panel_w, 1, strip, (size_t)panel_w * 2) != 0) {
                return false;
            }
        }
    } else {
        return false; /* a panel the node's pages are not drawn for */
    }
    (void)display_blanking_off(screen);
    set(&tft_led_en, 1);
    return true;
}

bool board_screen_page(int page, const uint8_t data[128]) {
    if (paged) {
        return panel_write(0, page * 8, PIC_W, 8, data, PIC_W) == 0;
    }
    /* Picture row y = 8 * page + bit lies at panel column x; picture column c at panel row. One
     * way round or, with TERN_SCREEN_FLIP, the other. */
    int at_x = (panel_h - PIC_W) / 2; /* where the picture's width starts along the long side */
    int at_y = (panel_w - PIC_H) / 2;
    int y0 = at_y + 8 * page;
#if CONFIG_TERN_SCREEN_FLIP
    int x = y0;                       /* panel column of the page's first row */
    int top = panel_h - at_x - PIC_W; /* panel row of the picture's last column */
    for (int r = 0; r < PIC_W; r++) {
        uint8_t col = data[PIC_W - 1 - r];
        for (int i = 0; i < 8; i++) {
            strip[r * 8 + i] = (col >> i) & 1 ? LIT : DARK;
        }
    }
#else
    int x = panel_w - 1 - (y0 + 7);
    int top = at_x;
    for (int r = 0; r < PIC_W; r++) {
        uint8_t col = data[r];
        for (int i = 0; i < 8; i++) {
            strip[r * 8 + i] = (col >> (7 - i)) & 1 ? LIT : DARK;
        }
    }
#endif
    return panel_write(x, top, 8, PIC_W, strip, sizeof strip) == 0;
}

bool board_screen_power(bool on) {
    if (on) {
        /* On a panel without a backlight of its own, this is all that wakes it. */
        int err = display_blanking_off(screen);
        set(&tft_led_en, 1);
        return err == 0;
    }
    set(&tft_led_en, 0);
    return display_blanking_on(screen) == 0;
}

#else

bool board_screen_init(void) { return false; }
bool board_screen_page(int page, const uint8_t data[128]) { return false; }
bool board_screen_power(bool on) { return false; }

#endif

/* The panels here show each page as it arrives. */
void board_screen_poll(bool prompt) { (void)prompt; }
void board_screen_show(bool wait) { (void)wait; }

/* --- The battery ----------------------------------------------------------------------------- */

#if DT_NODE_EXISTS(VBATT)

bool board_battery_init(void) {
    if (!adc_is_ready_dt(&battery) || !output(&battery_switch, GPIO_OUTPUT_INACTIVE) ||
        adc_channel_setup_dt(&battery) != 0) {
        return false;
    }
    have_adc = true;
    return true;
}

/* The battery's millivolts with the switch's line at `level` (its level on the pin, whatever its
 * flags say), or 0 if the ADC would not say. */
static uint16_t battery_at(int level) {
    int16_t raw;
    struct adc_sequence seq = {.buffer = &raw, .buffer_size = sizeof raw};
    if (adc_sequence_init_dt(&battery, &seq) != 0) {
        return 0;
    }
    if (battery_switch.port != NULL) {
        (void)gpio_pin_set_raw(battery_switch.port, battery_switch.pin, level);
    }
    k_busy_wait(BATTERY_SETTLE_US);
    int32_t sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        if (adc_read_dt(&battery, &seq) != 0) {
            return 0;
        }
        int32_t mv = raw < 0 ? 0 : raw;
        if (adc_raw_to_millivolts_dt(&battery, &mv) != 0) {
            return 0;
        }
        sum += mv;
    }
    int64_t mv = (int64_t)(sum / BATTERY_SAMPLES) * DIVIDER_FULL / DIVIDER_OUTPUT;
    return (uint16_t)(mv > UINT16_MAX ? UINT16_MAX : mv);
}

/* As on the Heltec ESP32 boards, the switch's sense is learnt rather than assumed (power.h): the
 * devicetree's flags say which level turns it on, and the board finds so itself. */
uint16_t board_battery_mv(void) {
    if (!have_adc) {
        return 0;
    }
    if (battery_switch.port == NULL) {
        uint16_t mv = battery_at(0); /* a divider always connected */
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
    (void)gpio_pin_set_raw(battery_switch.port, battery_switch.pin,
                           sense.known && !sense.high_enables ? 1 : 0);
    return mv;
}

static void battery_off(void) {
    if (battery_switch.port != NULL) {
        (void)gpio_pin_set_raw(battery_switch.port, battery_switch.pin,
                               sense.known && !sense.high_enables ? 1 : 0);
    }
}

#else

bool board_battery_init(void) { return false; }
uint16_t board_battery_mv(void) { return 0; }
static void battery_off(void) {}

#endif

/* --- Turning off ----------------------------------------------------------------------------- */

/* A start the timer gave, told apart from a press or power coming on by a word in RAM that a
 * restart keeps and power coming on does not. */
static __noinit uint32_t woke_by;
#define WOKE_BY_TIMER 0x54494d45u /* "TIME" */

bool board_woke_by_timer(void) {
    bool timer = woke_by == WOKE_BY_TIMER;
    woke_by = 0;
    return timer;
}

/* Off: the screen and its backlight unpowered, the LED and Vext off, and the chip in System OFF,
 * drawing a few microamps, once the button has been let go; a press starts it again from the
 * top, or on a board without one, RESET. System OFF has no timer to wake it, so a board asked to
 * wake after a while sleeps in System ON instead, idle, and restarts when the time is up or the
 * button is pressed. */
void board_off(uint32_t wake_after_s) {
    board_led(false);
    set(&tft_led_en, 0);
    (void)output(&tft_en, GPIO_OUTPUT_INACTIVE);
    (void)output(&vext, GPIO_OUTPUT_INACTIVE);
    battery_off();
    while (board_button()) {
        k_msleep(10);
    }
    k_msleep(50); /* the contacts settle */
    if (wake_after_s != 0) {
        for (uint32_t ms = 0; ms < wake_after_s * 1000u; ms += 100) {
            if (board_button()) {
                sys_reboot(SYS_REBOOT_COLD);
            }
            k_msleep(100);
        }
        woke_by = WOKE_BY_TIMER;
        sys_reboot(SYS_REBOOT_WARM);
    }
    if (button.port != NULL) {
        (void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_LEVEL_ACTIVE);
    }
    sys_poweroff();
}

/* --- The console ----------------------------------------------------------------------------- */

/* USB CDC ACM, which the board makes its console: printf() goes there too. */
bool board_console_init(void) { return device_is_ready(console); }

bool board_console_read(uint8_t *c) { return uart_poll_in(console, c) == 0; }

void board_console_write(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(console, buf[i]);
    }
}

void board_console_flush(uint32_t ms) { k_msleep(ms < 100 ? ms : 100); }
