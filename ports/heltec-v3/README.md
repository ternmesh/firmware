# Heltec WiFi LoRa 32 V3

The first board port: an ESP32-S3 with an SX1262, built with ESP-IDF 5.5. Two boards paired with
the same passphrase send each other [secured unicast frames](https://github.com/ternmesh/spec/blob/main/draft/unicast-security.md)
over the air.

It is a bench demo, not a node. There is no routing, no airtime budget and no real first contact
yet, because the specification has none of them yet.

## Flashing

Flashing replaces whatever is on the board, Meshtastic included, along with its settings.

### Without installing anything

1. Open the latest CI run for this branch, and under **Artifacts** download `tern-heltec-v3`.
   Unzip it.
2. In Chrome or Edge, open [esptool-js](https://espressif.github.io/esptool-js/), plug in the
   board over USB, and press **Connect**.
3. Set the flash address to `0x0`, choose `tern-heltec-v3-full.bin`, and press **Program**.
4. Press the board's RST button, then open a serial terminal at 115200 baud: esptool-js has one
   under **Console**, or use the Arduino IDE's serial monitor, or `screen /dev/ttyUSB0 115200`.

Flashing the full image this way also erases the board's saved session. Pair it again, with a new
passphrase, afterwards (see below).

### With ESP-IDF

With [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/)
installed and activated:

```bash
cd ports/heltec-v3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor     # COM3 or similar on Windows
```

`idf.py menuconfig`, under **Tern demo**, changes the frequency, power, spreading factor,
bandwidth and sync word.

## Using it

Type commands into the serial terminal:

| Command | |
|---|---|
| `pair i <passphrase>` | Pair as the initiator. |
| `pair r <passphrase>` | Pair as the responder. |
| `send <text>` | Send up to 239 bytes. |
| `status` | Radio settings, role and message counts. |

Pressing **PRG** sends a ping. A board that hears a ping answers with a pong saying how strongly
it heard it, so one press checks both directions. The white LED blinks for each frame sent or
received.

To start: on one board type `pair i` followed by a passphrase, and on the other `pair r` with the
same passphrase. Then press PRG on either board.

## The rules of the stopgap pairing

The specification does not yet say how two nodes first meet; that will be EDHOC. Until then the
demo makes the session secret from the passphrase. That is safe only if a session never starts
again from the beginning, because the same passphrase always gives the same keys. So:

* **One board `i`, the other `r`.** If both take the same role, both use the same keys. The
  first board to hear the other notices and stops sending for good. Pair both again with a new
  passphrase.
* **A new passphrase for each pairing.** A board remembers the passphrases it has used, up to 32,
  and will not pair with one again.
* **After erasing a board, a passphrase neither board has used before.** Erasing the flash
  (including flashing the full image) makes the board forget its session and its list of
  passphrases. It is the one mistake the board cannot catch.

The session is saved to flash after every message, before the frame goes out, so a reset or a
power cut carries on where it left off.

The keys sit in flash unencrypted. Anyone holding the board can read them. That is acceptable
for a bench demo, and is one of the things a real node will do differently.

## Radio settings and the rules for 902–928 MHz

The defaults are 919.0 MHz, SF7, 250 kHz bandwidth, CR 4/5, a 16-symbol preamble and +2 dBm.

* **Frequency.** 919.0 MHz lies between the LoRaWAN uplink channels (902.3–914.9 MHz) and its
  downlink channels (923.3–927.5 MHz). It is also well away from Meshtastic's US LongFast default
  of 906.875 MHz.
* **Power.** +2 dBm is plenty for a bench: two boards in the same house hear each other easily.
* **Rules.** In the US, the FCC's Part 15 rules for this band set power and bandwidth limits.
  A fixed-channel, low-power bench test is the most conservative use of it. Raising the power,
  or running for long periods, is your responsibility to keep within the rules. Tern's real
  on-air settings, and how they meet each region's rules, are for the specification to set
  (MSH-28).

The sync word is `0x24`. It is neither Meshtastic's (`0x2B`) nor MeshCore's (`0x12`), so these
boards do not decode those networks' frames. They do hear the frames as energy on the channel,
as any radio would.

## How it is put together

| File | |
|---|---|
| `main/board.c` | The pins, the SPI bus, the radio's reset and BUSY line, the button and the LED. |
| `main/demo.c` | The stopgap pairing and the saved session. It has no hardware code, so `tests/demo.c` tests it on a host. |
| `main/main.c` | One loop that polls the radio, the serial port and the button. |
| `../../src/sx126x.c` | The SX1262 driver, part of the core and shared with future boards. |

The core is compiled into the app unchanged, from the repository's `src/`.
