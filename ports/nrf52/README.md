# nRF52840 boards

The port for boards with Nordic's nRF52840 and an SX1262 or an LR1110, built with
[Zephyr](https://zephyrproject.org/). It runs the same node as the [ESP32 boards](../esp32/):
the same commands, screen pages, companion link and Bluetooth service, from `../node/`. Only what
is Zephyr's is here. Everything in the ESP32 port's README about using a node holds here too,
except where this one says otherwise.

| Board | Name | Status |
|---|---|---|
| Heltec Mesh Node T114 V2 | `heltec-t114` | Built; on a board, makes contact and its messages are acknowledged |
| RAKwireless RAK4631, on a RAK19007 or RAK19003 | `rak4631` | Built, not yet run on a board |
| Seeed Wio Tracker L1, and the L1 Lite | `wio-tracker-l1` | Built, not yet run on a board |
| Seeed SenseCAP T1000-E | `t1000-e` | Built; on a board, makes contact and its messages are acknowledged |

Each is Zephyr's board for it, with this port's overlay on it in `boards/`: what the board has is
read from its devicetree (`src/board.c`), so a board Zephyr has is an overlay and its names in
`Kconfig`, as [Adding an nRF52840 board](../../docs/boards.md#adding-an-nrf52840-board) says. The
T1000-E, which Zephyr has not got, is a board of this port's own, in `boards/seeed/t1000_e/`.

An nRF52840 runs for days on a cell where an ESP32 runs for hours, and most of these boards come
with a UF2 bootloader: plugged in, the board shows itself as a USB drive, and an image is copied
onto it.

## Flashing

Flashing replaces whatever is on the board, Meshtastic included, along with its settings. The
bootloader stays, so going back is copying another image on.

1. From the latest [release](https://github.com/ternmesh/firmware/releases), download
   `tern-<board>-us915-<version>.uf2` for the United States and Canada, or
   `tern-<board>-eu868-<version>.uf2` for Europe (`-au915-` for Australia, `-nz915-` for New
   Zealand, `-cl915-` for Chile, `-my919-` for Malaysia, `-th920-` for Thailand, `-ru868-` for
   Russia, `-kz863-` for Kazakhstan: the specification's
   [list](https://github.com/ternmesh/spec/blob/main/draft/phy.md#profiles) says which serves
   where), with the board's name from the table above. A
   board sends on its region's frequency as soon as it starts, so take the right one.
2. Plug the board in over USB and press its RST button twice, quickly. A drive appears: `HT-n5262`
   on the T114, `RAK4631` on the RAK4631, and one Seeed names on the Wio Tracker L1. The T1000-E
   has no RST button: hold its button while plugging the USB cable in twice, quickly, until its
   green LED stays on (Seeed's wiki; it may take a few tries), and `T1000-E` appears. A board
   already running Tern needs no button at all (below).
3. Copy the `.uf2` file onto the drive. The board writes it, restarts, and the drive goes.
4. Open a serial terminal on its USB port: `/dev/ttyACM0` or similar, at any speed (it is USB,
   not a UART). Type `status`.

The first start gives the board its identity and address. On a board that ran Meshtastic or other
firmware before, it first finds that firmware's saved files where it keeps its own, says so, wipes
them and starts again: a moment longer, once. Copying a newer release on later keeps
them, with its sessions, contacts and paired phones: the image does not reach the flash's storage.

Every CI run also keeps the same images of its commit, under **Artifacts** as `tern-<board>`.

### Without a button

A board running Tern opens its bootloader when a computer opens its USB port at 1200 baud and
closes it, as Arduino boards do (`stty -f /dev/cu.usbmodem* 1200` on a Mac, `stty -F
/dev/ttyACM0 1200` on Linux): the drive appears, and a newer `.uf2` is copied on as above.

`./flash.sh t1000-e <zephyr.hex>`, with `adafruit-nrfutil` installed, does the same over the serial
port instead, by serial DFU, with no drive: for a T1000-E whose bootloader was opened in its
serial-only mode, as MeshCore's firmware opens it.

### Building it

With a west workspace for this port (`west.yml` pins the Zephyr it builds against) and the
[Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html):

```bash
mkdir tern && cd tern
git clone https://github.com/ternmesh/firmware
west init -l --mf ports/nrf52/west.yml firmware
west update
west build -b heltec_t114_v2/nrf52840/uf2 firmware/ports/nrf52
# build/zephyr/zephyr.uf2 is the image
```

The RAK4631 is `-b rak4631/nrf52840`, the Wio Tracker L1 `-b wio_tracker_l1/nrf52840`, and the
T1000-E `-b t1000_e/nrf52840`.

`west build -t menuconfig`, under **Tern demo**, has the same settings as the ESP32 boards'
(`../node/Kconfig.tern`): region, power, antenna gain and the rest. `./release.sh <version>`
builds what a release carries, for each board an image for each region, into `release/`
(`./release.sh <version> <board>` for only that one).

## What is different from the ESP32 boards

* **The screen.** The T114's is a 1.14" colour panel, 240 by 135, and the node draws it in a
  layout of its own (`../node/colour.c`): the same pages, in smooth type and Tern's colours on a
  dark ground, with icons, the battery and a phone connected at the head of each page, and at its
  foot the page dots and what a hold of USER does. Home's figures are tiles, a message is a
  bubble with its state in colour, Nearby gives each node signal bars, and More's menu is a grid
  of six. The picture is drawn and sent a band of fifteen rows at a time, and only the bands that
  changed. If it reads upside down, `menuconfig`, **Turn the screen upside down**.
* **The screen** of the Wio Tracker L1 is a 128 by 64 OLED, the node's own size, as on the ESP32
  boards. The RAK4631 has none: its LED and the console say what it is doing.
* **The button.** The T114's USER button and the Wio Tracker L1's menu button do what PRG does on
  the Heltec ESP32 boards. RST resets the board, and twice quickly opens the bootloader. The
  RAK4631's base boards have only RST, so the RAK4631 turns off only when its battery runs down or
  a client turns it off, and RST turns it on again.
* **Off.** Held for five seconds, the button turns the board off as on the ESP32 boards: the
  screen unpowered and the chip in System OFF, drawing a few microamps, until the button is pressed.
  System OFF has no timer, so a board off for an empty battery waits idle instead, and looks at the
  battery again every half hour. A client turns it off the same way with `OFF`.
* **Updates over Bluetooth** go by the bootloader, not over the link: the application's flash has
  room for one image, not the two an update over the link needs. A client sends
  `UPDATE_BOOTLOADER` (the companion protocol's version 12), the board restarts into its
  bootloader, and the bootloader takes the release's `.zip` for the board and region over
  Bluetooth, Nordic's legacy DFU, as the boards ship; everything the board has saved is kept.
  `python3 tools/companion.py --port <port> update tern-<board>-<region>-<version>.zip` does it
  from a computer (it needs `bleak`), in five or six minutes. See [below](#updating-over-bluetooth).
* **Erasing.** The Erase page wipes the flash's storage and restarts. The time on the air is
  carried across that restart in RAM; if the bootloader clears it on the way, the region's count
  starts again.
* **Storage.** What the node saves goes in Zephyr's settings, on NVS in the 32 KB the bootloader
  leaves for it.
  `status` gives the storage's use in bytes rather than the ESP32's entries.
* **Bluetooth** is Zephyr's host and the chip's own controller, with the same service, pairing and
  frames as the ESP32 boards: a phone that pairs with one pairs with the other the same way.

## Updating over Bluetooth

A board offers it only if its bootloader is the
[OTAFIX](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX) build of the Adafruit
bootloader, which says so in its version (`INFO_UF2.TXT` on its drive: `0.9.2-OTAFIX2.3` and
the like): the node reads that from the bootloader's flash at start, and says on the console
which it found. OTAFIX erases the old firmware as the new arrives, and if a transfer breaks it
waits for another over Bluetooth. Heltec ship it on the T114. The bootloader a T1000-E came with
(Seeed's 0.9.1) took the first step and never answered it, and once reset came back in USB only:
a board with such a bootloader tells a client it cannot be updated, and is updated by copying the
`.uf2` on, as above.

To put OTAFIX on a board, copy the release's `update-<board>_bootloader-<version>_nosd.uf2` onto
its drive, as an image: it rewrites the bootloader alone, and leaves Tern and what it saved. Do
not unplug the board while it writes: a bootloader cut off half written needs a debugger to
mend.

## How it is put together

| File | |
|---|---|
| `src/board.c` | The board: the radio's SPI bus and lines, its TCXO and antenna switch, the button, the LED, the screen, the battery and the USB console, from the devicetree. |
| `src/platform.c` | What the node needs of Zephyr (`../node/platform.h`): the settings for what it saves, the chip's true generator, and the kernel. |
| `src/ble.c` | The companion link's Bluetooth LE service and pairing, over Zephyr's host. |
| `flash.sh` | Serial DFU through the bootloader after the 1200 bps touch, for a bootloader open without its drive. |
| `release.sh` | A release's images: for each board and region, the `.uf2` to copy on and the `.zip` its bootloader takes over Bluetooth. |
| `boards/seeed/t1000_e/` | The T1000-E's own Zephyr board, from Seeed's SDK and Arduino core, since Zephyr has none: its LR1110, button, LEDs, battery, UF2 bootloader's layout and USB console. |
| `dts/bindings/` | The LR1110's devicetree binding, Zephyr having one only for the LR1121. |
| `boards/*.overlay`, `boards/*.conf` | What this port changes of each Zephyr board: Tern's own SX1262 driver rather than Zephyr's, the battery's divider where the board does not give it, and on the RAK4631 the UF2 bootloader's layout, the USB console and where RAK's documents differ from Zephyr's board. |
| `Kconfig`, `prj.conf` | The node's settings, shared with the ESP32 port, and what it needs of Zephyr. |
| `west.yml` | The Zephyr it builds against, and the modules it needs. |

The radio is driven by the core's SX1262 driver (`../../src/sx126x.c`), or on the T1000-E its
LR1110 driver (`../../src/lr11xx.c`), chosen by the radio's devicetree node.
