#ifndef NODE_BOARD_H
#define NODE_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tern/radio.h"
#include "tern/time.h"

/* The board the node runs on: what each port implements for each board it supports, a LoRa radio
 * (an SX1262, or on an ESP32 an SX1276 or SX1278) and whatever the board has besides (on an ESP32,
 * ports/esp32/main/board.c, its pins from boards.c). Where a board has no screen, LED or battery,
 * those say so and the node goes on without. */

/* The board's name, as its maker sells it. */
const char *board_title(void);

/* The power the board puts into its antenna, in dBm: the least and the most it gives, and whether
 * it gives dbm. Every power the node asks of the radio is the antenna's. */
int8_t board_power_min(void);
int8_t board_power_max(void);
bool board_power_ok(int dbm);

/* Sets up the pins and the SPI bus, resets the radio and starts its driver, and powers the board's
 * amplifier if it has one. */
int board_init(void);

/* The radio for the core: the chip's own, or on a board with an amplifier one that drives it, and
 * takes the core's powers to be the antenna's (boards.h). */
struct tern_radio board_radio(void);

/* What the radio's receiver has seen since it started, or since board_radio_counts_reset(): the
 * driver's own counts (tern/sx126x.h, tern/sx127x.h), which are the same five. */
struct board_radio_counts {
    uint32_t preambles, headers, header_errors, crc_errors, frames;
};
void board_radio_counts(struct board_radio_counts *c);
void board_radio_counts_reset(void);

/* Puts the radio to sleep, for a board that is turning itself off: started first if board_init()
 * was never called, since from power on it would sit in standby. A radio that does not answer is
 * left as it is. */
void board_radio_sleep(void);

tern_time board_now(void);

/* The board's button (PRG on the Heltecs), which reads true while it is held. */
bool board_button(void);

void board_led(bool on);

/* The 128x64 OLED display, an SSD1306 on its own I2C bus. Powers it, resets it and sets it up
 * blank; false if it does not answer, and the board then runs without it. */
bool board_screen_init(void);

/* Sends one page of the picture: eight rows of 128 columns, a byte a column, the lowest bit at
 * the top (display.h). About 3 ms at the bus's 400 kHz. */
bool board_screen_page(int page, const uint8_t data[128]);

/* Turns the panel and its charge pump off, or on again. Off, it draws a few microamps and keeps
 * its picture, so it comes back showing what it showed. */
bool board_screen_power(bool on);

/* The battery's measuring circuit (power.h): false if the ADC would not start, and the board
 * then reports no battery. */
bool board_battery_init(void);

/* The battery's voltage in millivolts, or 0 for none fitted. Takes about 4 ms the first time, while
 * it learns which way the board's switch turns, and 2 ms after. */
uint16_t board_battery_mv(void);

/* Turns the board off: the display unpowered, the chip in deep sleep, drawing tens of microamps,
 * once PRG has been let go. A press of PRG turns it on again, and so does `wake_after_s` passing,
 * if it is not 0. Either way it starts from the top, as at power on. The radio is put to sleep by
 * the caller first (board_radio_sleep()). Never returns. */
__attribute__((noreturn)) void board_off(uint32_t wake_after_s);

/* Whether this start is the timer board_off() was given running out, rather than a press or
 * power coming on. */
bool board_woke_by_timer(void);

/* The serial console, over USB: UART0 or USB Serial/JTAG, as the build's console is. */
bool board_console_init(void);
bool board_console_read(uint8_t *c); /* one byte if one has come, without waiting */
void board_console_write(const uint8_t *buf, size_t len);
void board_console_flush(uint32_t ms); /* waits up to ms for what was written to leave */

#endif
