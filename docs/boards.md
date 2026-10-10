# Boards

Which boards Tern runs on, what adding one takes, and which come next. How the code is split
between the core and a port is [architecture.md](architecture.md); this is about the boards.

## Today

| Board | Chip | Radio | Port | Status |
|---|---|---|---|---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Runs |
| Heltec WiFi LoRa 32 V4 (V4.2, V4.3) | ESP32-S3 | SX1262, 28 dBm amplifier | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board |
| Heltec Wireless Stick Lite V3 | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board |
| Heltec Wireless Tracker (V1.0, V1.1) | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board; no screen yet (an 80x160 TFT) |
| Heltec Vision Master E290 | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board; its e-paper is driven (SSD1680) |
| Heltec Vision Master E213 | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board; no screen yet (e-paper) |
| Heltec Wireless Paper | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board; no screen yet (e-paper) |
| Heltec WiFi LoRa 32 V2, V2.1 | ESP32 | SX1276 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board |
| LilyGo T-Beam V1.0, V1.1 (AXP192) and V1.2 (AXP2101), 868/915 MHz | ESP32 | SX1276 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board; its GPS is left off |
| LilyGo LoRa32 T3 V1.6.1 (868/915 MHz) | ESP32 | SX1276 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board; no user button, so it turns off only when its battery runs down |
| LilyGo T3-S3 V1.2, V1.3 with an SX1276 (868/915 MHz) | ESP32-S3 | SX1276 | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board |
| Heltec Mesh Node T114 V2 | nRF52840 | SX1262 | [`ports/nrf52/`](../ports/nrf52/) | Built, not yet run on a board |
| RAKwireless RAK4631 (on a RAK19007 or RAK19003) | nRF52840 | SX1262 | [`ports/nrf52/`](../ports/nrf52/) | Built, not yet run on a board; no user button |
| Seeed Wio Tracker L1, L1 Lite | nRF52840 | SX1262 | [`ports/nrf52/`](../ports/nrf52/) | Built, not yet run on a board |

A board is **Runs** once someone has flashed a release onto one and checked it as
[below](#bringing-a-board-up), and **Built** until then: CI builds its images, and nothing has
yet said its pins are right.

## Adding an ESP32 board

Everything above the pins is the same on every ESP32 board, an ESP32-S3 or a classic ESP32 with an
SX1262 or an SX1276, so a new one is a row and a file:

1. **Read its maker's documents**: the pin map, and the schematic for anything the pin map does
   not say, such as which level turns a switch on. Not another mesh project's source for it, even
   where the licence would allow it ([CONTRIBUTING.md](../CONTRIBUTING.md)): its pin tables are
   that project's code. Where the documents leave a question open, say so beside the row.
2. **Add its row** to `ports/esp32/main/boards.c` (`struct board_def` in `boards.h`): the radio's
   chip (`soc`), the radio (`lora.chip`) and its pins, its TCXO, and on an SX127x whether its
   antenna is on PA_BOOST; the button, the LED, the screen, Vext, the battery's divider and
   switch, an amplifier if it has one, and a power management chip (an AXP192 or AXP2101,
   `ports/node/axp.h`) with the rails it switches. Cite the documents above the row.
3. **Let a build choose it**: a `TERN_BOARD_<NAME>` entry in the **Board** choice in
   `main/Kconfig.projbuild` and its `TERN_BOARD_NAME`, and `boards/<name>.defaults` setting it
   and anything else ESP-IDF needs: `CONFIG_IDF_TARGET="esp32"` for a classic ESP32, which
   `build.sh` and `release.sh` build for (the ESP32-S3 where it does not say),
   `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` for a board whose USB is the chip's own, and
   `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` with `partitions-4mb.csv` for one with 4 MB of flash.
   `tests/boards.c` fails until all three agree, and on a pin the chip has not got, an output on
   one of the ESP32's inputs, or a button that cannot wake it.
4. **Build it** in CI: add its name to the `esp32` job's matrix in `.github/workflows/ci.yml`.
   `release.sh` builds every file in `boards/` already.
5. **List it** in the port's README and the table above, as **Built**.

The site's [flash page](https://ternmesh.org/flash) and the phone apps find an image by the
board's name, which `INFO` gives, so a new name needs nothing from them to update a board that
runs Tern. The flash page lists the boards it offers, and is told of a new one in
[ternmesh/site](https://github.com/ternmesh/site) once a release carries its images.

What a row cannot say yet, a board needs code for first:

* **A screen that is not an SSD1306 or SSD1315**: an SH1106, a colour TFT, e-paper. `display.c`
  draws 128x64 pages; another controller needs its own `board_screen_*()`, and a larger screen
  wants its own layout of the pages.
* **Another ESP32**: the C3, C6 or S2. The port builds for the S3 and the classic ESP32; another
  target is its entry in `enum board_soc`, its GPIOs in `tests/boards.c`, and its ADC's ranges in
  `board.c`. The classic ESP32 has the least static RAM of them: its builds have about 9 KB to
  spare, which `idf.py size` reports.

## Bringing a board up

On the first board of a kind, before its status is **Runs**:

1. `status` on the console names the board, and the radio started: no **Did not start**.
2. The screen shows Home, and `screen sleep` turns it off and the button on again.
3. The battery's voltage on Home is within a tenth of a volt of a meter's on the cell.
4. Two boards make contact, and `send` reaches the other and is acknowledged.
5. On a bench page, holding the button sends a ping, and the other board's pong says it heard it.
6. Holding the button for five seconds turns it off, and a press starts it again with its address.
7. On a board with an amplifier, the power at the antenna, measured, is no more than the power
   set, at the least, the default and the most. Until then its figures are an estimate.

## Adding an nRF52840 board

The port is built on Zephyr, which describes boards in devicetree, and has many of these already:
among them the RAK4631, Seeed's Wio Tracker L1, XIAO nRF52840 and Wio-WM1110 kit, and the T114.
`src/board.c` reads the board from its devicetree: the SX1262 node (`lora`: its bus and lines,
`dio3-tcxo-voltage`, `dio2-tx-enable`, and any `antenna-enable-gpios`, `rx-enable-gpios` and
`tx-enable-gpios`), `sw0` and `led0` if it has them, `zephyr,display` (a 128x64 SSD1306 or SH1106
as it is; a colour panel drawn into), and the battery's divider as a `vbatt` node
(`voltage-divider`, with `power-gpios` for its switch). So a board Zephyr has is:

1. **Its maker's documents, read** against Zephyr's board, as for an ESP32 board's row. Where they
   differ, the maker's are taken and the overlay says why (the RAK4631's antenna switch, TCXO and
   LEDs).
2. **An overlay**, `boards/<board>_<soc>.overlay`, that disables Zephyr's LoRa driver
   (`&lora { status = "disabled"; }`), adds `vbatt` if the board does not have it, and changes
   what the maker's documents say otherwise; and a `.conf` beside it for what the board needs of
   Zephyr, such as I2C for its screen, or a UF2 image and a USB console where Zephyr's board does
   not build them.
3. **Its names**, `TERN_BOARD_NAME` and `TERN_BOARD_TITLE` in `Kconfig`, for its Zephyr board.
4. **A target** in `release.sh`, and its name in the `nrf52` job's matrix in CI.

A board Zephyr has not got is its devicetree first, from its maker's documents.

## Every board Meshtastic and MeshCore run on

That is a hundred boards or so, but far fewer kinds of thing: a few families of chip, each a port,
and a few radios, each a driver. A board is then its pins and what it has fitted. So the work
goes a family or a radio at a time, and each one brings a crowd of boards with it:

| What | Brings | Needs | Status |
|---|---|---|---|
| ESP32-S3 + SX1262 or SX1276 | Heltec V3, V4, Wireless Stick Lite V3, Wireless Tracker, Vision Master, Wireless Paper; LilyGo T3-S3, T-Beam Supreme, T-Deck; Seeed XIAO ESP32S3 kit; B&Q Station G2; RAK3312 | A row each; colour and e-paper screens for the rest | Port exists: V3 runs; V4, Stick Lite V3, Tracker, Vision Master, Paper and the T3-S3 with an SX1276 built; the SH1106 and the E290's SSD1680 e-paper are driven |
| ESP32 (classic) + SX1276 | Heltec V2, V2.1, Wireless Stick; LilyGo T3 V1.6.1, LoRa32 V1.3, T-Beam to v1.2 | A row each | Port builds for it: V2, V2.1, T3 V1.6.1 and T-Beam built |
| nRF52840 + SX1262 | Heltec T114, Mesh Pocket; RAK4631 and the WisMesh devices on it; Seeed Wio Tracker L1, XIAO nRF52840 kit; LilyGo T-Echo; Elecrow ThinkNode | An overlay each; e-paper for the T-Echo and others | Port exists: T114, RAK4631 and Wio Tracker L1 built |
| SX1276/SX1278 driver | The boards above, and other boards of before 2022 | A driver in `src/`, beside the SX1262's | Done (`src/sx127x.c`); the SX1278's 433 MHz waits on a region for it |
| LR1110/LR1121 driver | Seeed SenseCAP T1000-E, Wio Tracker 1110; newer boards | A driver in `src/` | Done (`src/lr11xx.c`), LoRa only; no board uses it yet |
| ESP32-C3/C6 | Heltec HT-CT62, and boards built from modules like it | The ESP32 port for another target | Later |
| RP2040 | RAK11310, Raspberry Pi Pico with a Waveshare SX1262 | A port, on Zephyr as for the nRF52840 | Later |
| STM32WL | RAK3172, Seeed LoRa-E5 | A port; the radio is in the chip, an SX126x behind registers | Later |
| Linux | A Raspberry Pi with a LoRa HAT (SX1262 over spidev) | The core already builds on Linux; a port is the node over spidev, a socket and files | Later |
| SX1280 | 2.4 GHz boards | A driver, and a region profile for 2.4 GHz in the specification first | Waits on the specification |

Board by board, each waits on its maker's documents, as above: the clean-room rule means another
mesh project's pin tables are not a source.

## Next

In the order that reaches the most boards for the work:

### More ESP32 boards

Rows, as above. Those whose makers' documents have been read, and what each still waits on:

* **LilyGo T3-S3 with an SX1262** (V1.2 and V1.3): its pins are in LilyGo's wiki, schematics and
  `utilities.h`, which also give the SX1276's, built as `lilygo-t3s3`. The SX1262's module has a
  32 MHz TCXO on DIO3, whose voltage LilyGo's documents do not give.
* **Seeed XIAO ESP32S3 with the Wio-SX1262**: its pins are in Seeed's two schematics, but the
  module's RF_SW line (GPIO38) has no documented level, and its button (GPIO21) is the XIAO's LED's
  pin too. It waits on a row for a pin that enables the antenna switch, and on a board to try both
  levels on.
* **RAKwireless RAK3312** (on a RAK19007): an antenna switch powered from GPIO4, which waits on a
  row for it. It has no user button, which a row may now leave out, as the T3 V1.6.1's does.
* **Heltec Wireless Stick** (V2, V2.1): Heltec's datasheet and schematic disagree on its flash,
  4 MB or 8 MB, and its screen is 64x32, which `display.c` does not lay out. Its Lite (V2.1) has a
  schematic stamped as restricted, which is not used as a source.
* **LilyGo LoRa32 V1.3**: its schematic gives a button on GPIO36, which LilyGo's own pin table
  does not list, and which has no pull-up of its own in the chip: it waits on a board to see
  whether it is fitted.
* **LilyGo T-Beam with an SX1262**: LilyGo does not give its TCXO's voltage.
* **LilyGo T-Beam Supreme**: its AXP2101 is driven now (`ports/node/axp.h`), and its pins and
  rails are in LilyGo's documents, and its SH1106 screen is driven (from Sino Wealth's
  datasheet). It waits on the TCXO voltage of its HPD16A module, which neither LilyGo nor the
  module's maker publishes.
* **B&Q Station G2**: B&Q's page gives its TCXO, 1.8 V, its amplifier and its power, but none of
  its pins, and links no schematic. It waits on those.

And the screens these boards have, which the port does not drive yet: the Wireless Tracker's
80x160 TFT, and the Vision Master E213's and Wireless Paper's e-paper. Each runs without one today.
The E290's SSD1680 is driven: Solomon Systech publishes its datasheet and DKE its panel's. The
E213 and the Wireless Paper changed panels between revisions, to a JD79656 and an SSD1682 whose
datasheets are not published, and the Wireless Paper's first, an SSD1680, cannot be told from the
SSD1682 by its chip ID; they wait on those datasheets, or on boards of each revision.

### More nRF52840 boards

The port is there ([`ports/nrf52/`](../ports/nrf52/)), on Zephyr, with the T114, the RAK4631
and the Wio Tracker L1, each an overlay. Next:

* **Seeed XIAO nRF52840 with the Wio-SX1262**: Zephyr's `xiao_ble` has no radio, so its overlay
  adds one, from Seeed's schematics, which come in two revisions with the header in a different
  order: it waits on knowing which one is sold.
* **LilyGo T-Echo**, **Heltec Mesh Pocket**, **Elecrow ThinkNode**: e-paper, or boards Zephyr has
  not got.
* A layout for the T114's larger screen, and updates over the link, which want the two firmware
  slots MCUboot gives alongside the UF2 bootloader these boards ship with.

### Other radios

* **The LR1110 and LR1121** are driven (`src/lr11xx.c`, from Semtech's LR1121 user manual and
  its SWDR001 driver), for LoRa; their GNSS and Wi-Fi scanners are not. The boards that carry
  them wait on their makers' documents agreeing:
  * **Seeed SenseCAP T1000-E**: its pins are in Seeed's SDK for it and its Arduino core, which
    agree. Its TCXO's voltage does not: Seeed's wiki says 1.6 V, its SDK 3.0 V, its Arduino core
    1.8 V. Nor does its antenna switch: the SDK drives DIO8 with DIO5 and DIO6, the Arduino core
    does not. Seeed publishes no schematic, and warns that flashing the wrong firmware can brick
    it. It waits on Seeed, or on a board to settle both on.
  * **Seeed Wio Tracker 1110**: two Seeed sources give its TCXO as 1.8 V and 3.0 V, and none says
    how its battery is read.

Each driver is tested on a host against a fake bus, as `tests/sx126x.c` tests the SX1262's.
