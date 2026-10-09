# nRF52840 boards

The port for boards with Nordic's nRF52840 and an SX1262, built with
[Zephyr](https://zephyrproject.org/). It runs the same node as the [ESP32 boards](../esp32/):
the same commands, screen pages, companion link and Bluetooth service, from `../node/`. Only what
is Zephyr's is here. Everything in the ESP32 port's README about using a node holds here too,
except where this one says otherwise.

| Board | Name | Status |
|---|---|---|
| Heltec Mesh Node T114 V2 | `heltec-t114` | Built, not yet run on a board |

An nRF52840 runs for days on a cell where an ESP32 runs for hours, and most of these boards come
with a UF2 bootloader: plugged in, the board shows itself as a USB drive, and an image is copied
onto it.

## Flashing

Flashing replaces whatever is on the board, Meshtastic included, along with its settings. The
bootloader stays, so going back is copying another image on.

1. From the latest [release](https://github.com/ternmesh/firmware/releases), download
   `tern-heltec-t114-us915-<version>.uf2` for the United States and Canada, or
   `tern-heltec-t114-eu868-<version>.uf2` for Europe. A board sends on its region's frequency as
   soon as it starts, so take the right one.
2. Plug the T114 in over USB and press its RST button twice, quickly. A drive named `HT-n5262`
   appears.
3. Copy the `.uf2` file onto the drive. The board writes it, restarts, and the drive goes.
4. Open a serial terminal on its USB port: `/dev/ttyACM0` or similar, at any speed (it is USB,
   not a UART). Type `status`.

The first start gives the board its identity and address. On a board that ran Meshtastic or other
firmware before, it first finds that firmware's saved files where it keeps its own, says so, wipes
them and starts again: a moment longer, once. Copying a newer release on later keeps
them, with its sessions, contacts and paired phones: the image does not reach the flash's storage.

Every CI run also keeps the same images of its commit, under **Artifacts** as `tern-heltec-t114`.

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

`west build -t menuconfig`, under **Tern demo**, has the same settings as the ESP32 boards'
(`../node/Kconfig.tern`): region, power, antenna gain and the rest. `./release.sh <version>`
builds what a release carries, an image for each region, into `release/`.

## What is different from the ESP32 boards

* **The screen.** The T114's is a 1.14" colour panel, 240 by 135. The node's pages are drawn for a
  128 by 64 screen, and are shown at that size in its middle: small, but whole. A layout of its
  own for the larger panel is to come. If it reads upside down, `menuconfig`, **Turn the screen
  upside down**.
* **The button.** The T114's USER button does what PRG does on the Heltec ESP32 boards. RST resets
  the board, and twice quickly opens the bootloader.
* **Off.** Held for five seconds, the button turns the board off as on the ESP32 boards: the
  screen unpowered and the chip in System OFF, drawing a few microamps, until the button is pressed.
  System OFF has no timer, so a board off for an empty battery waits idle instead, and looks at the
  battery again every half hour.
* **Updates over the link** are not here yet: `INFO` names no board, and the phone apps do not
  offer one. Copy the new `.uf2` on instead, which keeps everything the board has saved.
* **Erasing.** The Reset page wipes the flash's storage and restarts. The time on the air is
  carried across that restart in RAM; if the bootloader clears it on the way, the region's count
  starts again.
* **Storage.** What the node saves goes in Zephyr's settings, on NVS in the flash's last 32 KB.
  `status` gives the storage's use in bytes rather than the ESP32's entries.
* **Bluetooth** is Zephyr's host and the chip's own controller, with the same service, pairing and
  frames as the ESP32 boards: a phone that pairs with one pairs with the other the same way.

## How it is put together

| File | |
|---|---|
| `src/board.c` | The T114: the radio's SPI bus and lines, the button, the LED, the screen, the battery and the USB console, from Zephyr's devicetree for the board (`heltec_t114_v2`). |
| `src/platform.c` | What the node needs of Zephyr (`../node/platform.h`): the settings for what it saves, the chip's true generator, and the kernel. |
| `src/ble.c` | The companion link's Bluetooth LE service and pairing, over Zephyr's host. |
| `boards/heltec_t114_v2_nrf52840_uf2.overlay` | What this port changes of Zephyr's board: Tern's own SX1262 driver rather than Zephyr's, and the screen set up once it is powered. |
| `Kconfig`, `prj.conf` | The node's settings, shared with the ESP32 port, and what it needs of Zephyr. |
| `west.yml` | The Zephyr it builds against, and the modules it needs. |

The radio is driven by the core's SX1262 driver (`../../src/sx126x.c`), as on every board.
