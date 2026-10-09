# Boards

Which boards Tern runs on, what adding one takes, and which come next. How the code is split
between the core and a port is [architecture.md](architecture.md); this is about the boards.

## Today

| Board | Chip | Radio | Port | Status |
|---|---|---|---|---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | SX1262 | [`ports/esp32/`](../ports/esp32/) | Runs |
| Heltec WiFi LoRa 32 V4 (V4.2, V4.3) | ESP32-S3 | SX1262, 28 dBm amplifier | [`ports/esp32/`](../ports/esp32/) | Built, not yet run on a board |

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
2. The screen shows Home, and `screen sleep` turns it off and PRG on again.
3. The battery's voltage on Home is within a tenth of a volt of a meter's on the cell.
4. Two boards make contact, and `send` reaches the other and is acknowledged.
5. On a bench page, holding PRG sends a ping, and the other board's pong says it heard it.
6. Holding PRG for five seconds turns it off, and a press starts it again with its address.
7. On a board with an amplifier, the power at the antenna, measured, is no more than the power
   set, at the least, the default and the most. Until then its figures are an estimate.

## Next

In the order that reaches the most boards for the work:

### More ESP32 boards

Rows, as above: among others, the Heltec Wireless Stick Lite V3 (no screen), the LilyGo T3-S3
(its SX1262 version) and T-Beam Supreme, Seeed's XIAO ESP32S3 with the Wio-SX1262, and the B&Q
Station G2. Each waits on its maker's documents, and the T-Beam Supreme on a power management
chip.

### nRF52840 boards

The low-power boards: the Heltec Mesh Node T114, RAKwireless's RAK4631, Seeed's Wio Tracker L1
and the XIAO nRF52840 kits, and LilyGo's T-Echo. Most have an SX1262, whose driver is already the
core's, and they run for days on a small cell where an ESP32 runs for hours.

They need a port of their own, `ports/nrf52`, and the proposal is to build it on Zephyr: it is
Apache-2.0, as this repository is, and it has the Bluetooth LE stack the companion link needs, a
flash store for the identity and sessions, and MCUboot for the two firmware slots an update over
the link writes into. The port is the same three things the ESP32's is: the board's pins, the
loop (`main.c`'s, over Zephyr's), and the companion link's Bluetooth service. Most of these boards
ship with a UF2 bootloader, which shows the board as a USB drive an image is copied onto, and the
image has to start where that bootloader expects.

### Other radios

* **The SX1276 and SX1278**, in the T-Beam before the Supreme, the LilyGo T3 V1.6 and the Heltec
  V2: many of the boards in drawers. A driver beside `src/sx126x.c` that implements
  `tern/radio.h`; the frames on the air are the same.
* **The LR1110 and LR1121**, in Seeed's SenseCAP T1000-E and Wio Tracker 1110 and in newer
  boards: another driver, for a chip that also carries GNSS and Wi-Fi scanning.

Each driver is tested on a host against a fake bus, as `tests/sx126x.c` tests the SX1262's.
