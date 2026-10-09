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

/* The Heltec Mesh Node T114 V2, an nRF52840 wired to an SX1262, as Zephyr's board for it
 * (heltec_t114_v2) describes it and Heltec's schematic (MeshNode-T114_V2.1) shows: the radio on
 * SPI3 with a 1.8 V TCXO on DIO3 and its antenna switch on DIO2; a 135x240 ST7789 screen whose
 * supply (TFT_EN) and backlight (TFT_LED_EN) are each switched by a P-channel FET, on when low; the
 * user button, low when pressed; a green LED, lit when low; and the battery through 390k over 100k
 * onto AIN2, behind a switch P0.06 turns on when high. */

#define LORA DT_NODELABEL(lora)

static const struct spi_dt_spec lora_spi =
    SPI_DT_SPEC_GET(LORA, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_CONTROLLER);
static const struct gpio_dt_spec lora_reset = GPIO_DT_SPEC_GET(LORA, reset_gpios);
static const struct gpio_dt_spec lora_busy = GPIO_DT_SPEC_GET(LORA, busy_gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec tft_en = GPIO_DT_SPEC_GET(DT_ALIAS(tft_en), gpios);
static const struct gpio_dt_spec tft_led_en = GPIO_DT_SPEC_GET(DT_ALIAS(tft_led_en), gpios);
static const struct gpio_dt_spec vext = GPIO_DT_SPEC_GET(DT_ALIAS(vext_control), gpios);
static const struct gpio_dt_spec adc_ctrl = GPIO_DT_SPEC_GET(DT_ALIAS(adc_control), gpios);
static const struct device *const screen = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc));
static const struct adc_channel_cfg battery_channel =
    ADC_CHANNEL_CFG_DT(DT_CHILD(DT_NODELABEL(adc), channel_2));
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#define DIVIDER_TOP_K 390
#define DIVIDER_BOTTOM_K 100
#define BATTERY_SETTLE_US 2000
#define BATTERY_SAMPLES 16
#define BUSY_TIMEOUT_US 100000

#define POWER_MIN_DBM (-9) /* the SX1262's high-power amplifier */
#define POWER_MAX_DBM 22

static struct power_sense sense;
static bool have_adc;
static struct tern_sx126x sx;

const char *board_title(void) { return "Heltec Mesh Node T114"; }
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
        gpio_pin_configure_dt(&lora_busy, GPIO_INPUT) != 0 ||
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
        return TERN_EIO;
    }
    /* Hold NRESET low for over 100 us (section 8.1), then let the chip start. */
    gpio_pin_set_dt(&lora_reset, 1);
    k_busy_wait(1000);
    gpio_pin_set_dt(&lora_reset, 0);
    k_busy_wait(10000);

    struct tern_sx126x_bus sb = {.ctx = NULL, .transfer = bus_transfer, .now = bus_now};
    struct tern_sx126x_board wiring = {.tcxo_mv = 1800, .dio2_rf_switch = true, .dcdc = true};
    return tern_sx126x_init(&sx, &sb, &wiring);
}

struct tern_radio board_radio(void) { return tern_sx126x_radio(&sx); }

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
}

/* --- The button and the LED ------------------------------------------------------------------ */

bool board_button(void) {
    static bool ready;
    if (!ready) {
        if (gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
            return false;
        }
        ready = true;
    }
    return gpio_pin_get_dt(&button) == 1;
}

void board_led(bool on) { (void)gpio_pin_set_dt(&led, on ? 1 : 0); }

/* --- The screen ------------------------------------------------------------------------------ */

/* The node draws a 128x64 picture a page of eight rows at a time (display.h). The T114's panel is
 * 135x240, mounted on its side: the picture is turned to lie along it, at one panel pixel a
 * picture pixel, in its middle. That is small on a 1.14" panel; a layout of its own is for later
 * (docs/boards.md). */
#define PANEL_W 135
#define PANEL_H 240
#define PIC_W 128
#define PIC_H 64
#define ACROSS 240 /* the panel's long side, which the picture's width lies along */
#define DOWN 135
#define AT_X ((ACROSS - PIC_W) / 2)
#define AT_Y ((DOWN - PIC_H) / 2)
#define LIT 0xFFFFu
#define DARK 0x0000u

static uint16_t strip[PIC_W * 8]; /* one page, turned: 8 panel columns by 128 panel rows */

static int panel_write(int x, int y, int w, int h, const uint16_t *pixels) {
    struct display_buffer_descriptor d = {.buf_size = (uint32_t)(w * h * 2),
                                          .width = (uint16_t)w,
                                          .height = (uint16_t)h,
                                          .pitch = (uint16_t)w};
    return display_write(screen, (uint16_t)x, (uint16_t)y, &d, pixels);
}

bool board_screen_init(void) {
    if (gpio_pin_configure_dt(&tft_en, GPIO_OUTPUT_ACTIVE) != 0 ||
        gpio_pin_configure_dt(&tft_led_en, GPIO_OUTPUT_INACTIVE) != 0) {
        return false;
    }
    k_msleep(20); /* the panel's supply settles before its controller is reset and set up */
    if (device_init(screen) != 0 && !device_is_ready(screen)) {
        return false;
    }
    /* Dark all over, then the backlight on. */
    for (size_t i = 0; i < sizeof strip / sizeof strip[0]; i++) {
        strip[i] = DARK;
    }
    for (int y = 0; y < PANEL_H; y++) {
        if (panel_write(0, y, PANEL_W, 1, strip) != 0) {
            return false;
        }
    }
    (void)display_blanking_off(screen);
    (void)gpio_pin_set_dt(&tft_led_en, 1);
    return true;
}

bool board_screen_page(int page, const uint8_t data[128]) {
    /* Picture row y = 8 * page + bit lies at panel column x; picture column c at panel row. One
     * way round or, with TERN_SCREEN_FLIP, the other. */
    int y0 = AT_Y + 8 * page;
#if CONFIG_TERN_SCREEN_FLIP
    int x = y0;                       /* panel column of the page's first row */
    int top = PANEL_H - AT_X - PIC_W; /* panel row of the picture's last column */
    for (int r = 0; r < PIC_W; r++) {
        uint8_t col = data[PIC_W - 1 - r];
        for (int i = 0; i < 8; i++) {
            strip[r * 8 + i] = (col >> i) & 1 ? LIT : DARK;
        }
    }
#else
    int x = PANEL_W - 1 - (y0 + 7);
    int top = AT_X;
    for (int r = 0; r < PIC_W; r++) {
        uint8_t col = data[r];
        for (int i = 0; i < 8; i++) {
            strip[r * 8 + i] = (col >> (7 - i)) & 1 ? LIT : DARK;
        }
    }
#endif
    return panel_write(x, top, 8, PIC_W, strip) == 0;
}

bool board_screen_power(bool on) {
    if (on) {
        (void)display_blanking_off(screen);
        return gpio_pin_set_dt(&tft_led_en, 1) == 0;
    }
    (void)gpio_pin_set_dt(&tft_led_en, 0);
    return display_blanking_on(screen) == 0;
}

/* --- The battery ----------------------------------------------------------------------------- */

bool board_battery_init(void) {
    if (!device_is_ready(adc) || gpio_pin_configure_dt(&adc_ctrl, GPIO_OUTPUT_INACTIVE) != 0 ||
        adc_channel_setup(adc, &battery_channel) != 0) {
        return false;
    }
    have_adc = true;
    return true;
}

/* The battery's millivolts with the switch at `level`, or 0 if the ADC would not say. */
static uint16_t battery_at(int level) {
    int16_t raw;
    struct adc_sequence seq = {.channels = BIT(battery_channel.channel_id),
                               .buffer = &raw,
                               .buffer_size = sizeof raw,
                               .resolution = 12};
    (void)gpio_pin_set_dt(&adc_ctrl, level);
    k_busy_wait(BATTERY_SETTLE_US);
    int32_t sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        if (adc_read(adc, &seq) != 0) {
            return 0;
        }
        int32_t mv = raw < 0 ? 0 : raw;
        if (adc_raw_to_millivolts(adc_ref_internal(adc), battery_channel.gain, 12, &mv) != 0) {
            return 0;
        }
        sum += mv;
    }
    int32_t mv = sum / BATTERY_SAMPLES * (DIVIDER_TOP_K + DIVIDER_BOTTOM_K) / DIVIDER_BOTTOM_K;
    return (uint16_t)(mv > UINT16_MAX ? UINT16_MAX : mv);
}

/* As on the Heltec ESP32 boards, the switch's sense is learnt rather than assumed (power.h): high
 * turns it on, by the schematic, and the board finds so itself. */
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
    (void)gpio_pin_set_dt(&adc_ctrl, sense.known && !sense.high_enables ? 1 : 0);
    return mv;
}

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
 * top. System OFF has no timer to wake it, so a board asked to wake after a while sleeps in System
 * ON instead, idle, and restarts when the time is up or the button is pressed. */
void board_off(uint32_t wake_after_s) {
    board_led(false);
    (void)gpio_pin_set_dt(&tft_led_en, 0);
    (void)gpio_pin_configure_dt(&tft_en, GPIO_OUTPUT_INACTIVE);
    (void)gpio_pin_configure_dt(&vext, GPIO_OUTPUT_INACTIVE);
    (void)gpio_pin_set_dt(&adc_ctrl, 0);
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
    (void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_LEVEL_ACTIVE);
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
