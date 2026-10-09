#ifndef ESP32_BOARD_H
#define ESP32_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boards.h"
#include "tern/radio.h"
#include "tern/sx126x.h"
#include "tern/time.h"

/* The board the build was made for (Kconfig, "Board"), an ESP32 wired to an SX1262: its pins and
 * what it has fitted are its entry in boards.c. */

/* Which board this is. */
const struct board_def *board_def(void);

/* Sets up the pins and the SPI bus, resets the radio and starts its driver, and powers the board's
 * amplifier if it has one. */
int board_init(struct tern_sx126x *radio);

/* The radio for the core: the chip's own, or on a board with an amplifier one that drives it, and
 * takes the core's powers to be the antenna's (boards.h). */
struct tern_radio board_radio(struct tern_sx126x *radio);

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
 * the caller first (tern_sx126x_sleep()). Never returns. */
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
