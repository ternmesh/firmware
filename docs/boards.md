# Boards

Which boards Tern runs on, what adding one takes, and which come next. How the code is split
between the core and a port is [architecture.md](architecture.md); this is about the boards.

## Today

| Board | Chip | Radio | Port | Status |
|---|---|---|---|---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Runs |
| Heltec WiFi LoRa 32 V4 (V4.2, V4.3) | ESP32-S3 | SX1262, 28 dBm amplifier | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board |
| Heltec Mesh Node T114 V2 | nRF52840 | SX1262 | [`ports/nrf52/`](../ports/nrf52/) | Built, not yet run on a board |

A board is **Runs** once someone has flashed a release onto one and checked it as
[below](#bringing-a-board-up), and **Built** until then: CI builds its images, and nothing has
yet said its pins are right.

## Adding an ESP32 board with an SX1262

Everything above the pins is the same on every such board, so a new one is a row and a file:

1. **Read its maker's documents**: the pin map, and the schematic for anything the pin map does
   not say, such as which level turns a switch on. Not another mesh project's source for it, even
   where the licence would allow it ([CONTRIBUTING.md](../CONTRIBUTING.md)): its pin tables are
   that project's code. Where the documents leave a question open, say so beside the row.
2. **Add its row** to `ports/esp32/main/boards.c` (`struct board_def` in `boards.h`): the radio's
   pins and TCXO, the button, the LED, the screen, Vext, the battery's divider and switch, and an
   amplifier if it has one. Cite the documents above the row.
3. **Let a build choose it**: a `TERN_BOARD_<NAME>` entry in the **Board** choice in
   `main/Kconfig.projbuild` and its `TERN_BOARD_NAME`, and `boards/<name>.defaults` setting it
   and anything else ESP-IDF needs, such as `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` for a board
   whose USB is the chip's own. `tests/boards.c` fails until all three agree.
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
* **A power management chip** on I2C (an AXP192 or AXP2101), which switches the radio's and the
  screen's supplies and reads the battery itself.
* **Another ESP32**: the classic ESP32, the C3, C6 or S2. The port builds for the S3
  (`CONFIG_IDF_TARGET` in `sdkconfig.defaults`); another target is a `set-target` and a check
  that every pin it uses exists on that chip and can wake it.

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
`src/board.c` is the T114's today. It reads what it can from the devicetree (the radio's bus and
lines, the button, the LED, the screen), so the next board starts by moving what is the T114's
own (its screen's turn and size, its switches) behind that board's devicetree, and then is an
overlay in `boards/` and a target in `release.sh` and CI. A board Zephyr has not got is its
devicetree first, from its maker's documents, as for an ESP32 board's row.

## Every board Meshtastic and MeshCore run on

That is a hundred boards or so, but far fewer kinds of thing: a few families of chip, each a port,
and a few radios, each a driver. A board is then its pins and what it has fitted. So the work
goes a family or a radio at a time, and each one brings a crowd of boards with it:

| What | Brings | Needs | Status |
|---|---|---|---|
| ESP32-S3 + SX1262 | Heltec V3, V4, Wireless Stick Lite V3, Wireless Tracker, Vision Master; LilyGo T3-S3, T-Beam Supreme, T-Deck; Seeed XIAO ESP32S3 kit; B&Q Station G2; RAK3312 | A row each; a power chip for the T-Beam Supreme, other screens for the rest | Port exists: V3 runs, V4 built |
| nRF52840 + SX1262 | Heltec T114, Mesh Pocket; RAK4631 and the WisMesh devices on it; Seeed Wio Tracker L1, XIAO nRF52840 kit; LilyGo T-Echo; Elecrow ThinkNode | An overlay each, once `board.c` reads its board from the devicetree; e-paper for the T-Echo and others | Port exists: T114 built |
| SX1276/SX1278 driver | LilyGo T-Beam to v1.2, T3 V1.6; Heltec V2; other boards of before 2022 | A driver in `src/`, beside the SX1262's | Next radio |
| ESP32 (classic) | The SX1276 boards above, and the T-Beam v1.x with an SX1262 | The ESP32 port for another target | With the SX127x driver |
| LR1110/LR1121 driver | Seeed SenseCAP T1000-E, Wio Tracker 1110; newer boards | A driver in `src/` | After the SX127x |
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

Rows, as above: among others, the Heltec Wireless Stick Lite V3 (no screen), the LilyGo T3-S3
(its SX1262 version) and T-Beam Supreme, Seeed's XIAO ESP32S3 with the Wio-SX1262, and the B&Q
Station G2. Each waits on its maker's documents, and the T-Beam Supreme on a power management
chip.

### More nRF52840 boards

The port is there ([`ports/nrf52/`](../ports/nrf52/)), on Zephyr, with the T114. Next: `board.c`
read from the devicetree, so that a board Zephyr already has is an overlay (the RAK4631, the Wio
Tracker L1, whose 128x64 screen is the node's own size, the XIAO nRF52840 kit); a layout for the
T114's larger screen; and updates over the link, which want the two firmware slots MCUboot gives
alongside the UF2 bootloader these boards ship with.

### Other radios

* **The SX1276 and SX1278**, in the T-Beam before the Supreme, the LilyGo T3 V1.6 and the Heltec
  V2: many of the boards in drawers. A driver beside `src/sx126x.c` that implements
  `tern/radio.h`; the frames on the air are the same.
* **The LR1110 and LR1121**, in Seeed's SenseCAP T1000-E and Wio Tracker 1110 and in newer
  boards: another driver, for a chip that also carries GNSS and Wi-Fi scanning.

Each driver is tested on a host against a fake bus, as `tests/sx126x.c` tests the SX1262's.
