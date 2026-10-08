#ifndef HELTEC_V3_BOARD_H
#define HELTEC_V3_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "tern/sx126x.h"
#include "tern/time.h"

/* The Heltec WiFi LoRa 32 V3: an ESP32-S3 wired to an SX1262 with a 1.8 V TCXO on DIO3 and the
 * antenna switch on DIO2, as Heltec's schematic shows. */

/* Sets up the pins and the SPI bus, resets the radio and starts its driver. */
int board_init(struct tern_sx126x *radio);

tern_time board_now(void);

/* The PRG button, which reads true while it is held. */
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

#endif
