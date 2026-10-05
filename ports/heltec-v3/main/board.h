#ifndef HELTEC_V3_BOARD_H
#define HELTEC_V3_BOARD_H

#include <stdbool.h>

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

#endif
