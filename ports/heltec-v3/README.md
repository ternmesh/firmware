# Heltec WiFi LoRa 32 V3

The first board port: an ESP32-S3 with an SX1262, built with ESP-IDF 5.5. Each board has an
address. One makes [first contact](https://github.com/ternmesh/spec/blob/main/draft/first-contact.md)
with the other's, and the two then send each other
[secured unicast frames](https://github.com/ternmesh/spec/blob/main/draft/unicast-security.md)
over the air.

It is a bench demo, not a node. Boards announce themselves and learn routes to each other, but
messages do not follow those routes yet, because the specification has not drafted the frames that
do: a board talks to one other board at a time, and only if it hears it directly. There is no
airtime budget yet either.

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

Flashing the full image this way also erases the board's identity and its session. It starts
again with a new address, and the other board has to make contact with that one.

### With ESP-IDF

With [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/)
installed and activated:

```bash
cd ports/heltec-v3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor     # COM3 or similar on Windows
```

If flashing stops with "Invalid head of packet", the USB-to-serial chip is not keeping up: add
`-b 230400`.

`idf.py menuconfig`, under **Tern demo**, chooses the region, the transmit power and the
antenna's gain. The default region is US915. The image CI builds is for US915 too: for EU868,
build it yourself.

## Using it

Type commands into the serial terminal:

| Command | |
|---|---|
| `status` | This board's address, the radio settings and the session. |
| `contact <address>` | Make first contact with the board whose address that is. |
| `accept` | For two minutes, let a board other than the present peer make contact. |
| `send <text>` | Send up to 239 bytes. |
| `routes` | The boards this one hears, how well each hears the other, and the routes it has. |
| `selftest` | Run a handshake between two nodes in the board's memory, and time it. |

Pressing **PRG** sends a ping. A board that hears a ping answers with a pong saying how strongly
it heard it, so one press checks both directions. The white LED blinks for each frame sent or
received.

To start:

1. Type `status` on the second board and copy its address, sixty-four hex digits.
2. On the first board, type `contact` and that address. Four frames cross, taking two or three
   seconds, and both boards say that a session has started and with whom.
3. Press PRG on either board.

With only one board, `selftest` shows that the handshake and a message each way work on it, with
nothing sent.

## Identity and first contact

A board makes its identity the first time it starts: 32 random bytes from the ESP32's hardware
generator, saved to flash. Its address is the Ed25519 public key of that seed, and stays the same
until the flash is erased.

First contact is the specification's: an EDHOC handshake of four frames, of 45, 53, 73 and 17
bytes. Neither address goes over the air in clear, each board proves it holds the key behind its
address, and each handshake gives a new session secret, so nothing has to be remembered to stop
keys repeating.

What the specification has not settled yet, the demo decides for itself. None of this is Tern
yet:

* **Lost frames.** The board that began sends each of its two frames up to four times, a little
  over two seconds apart at the default settings, and then gives up. The other board never sends
  unasked: a frame it has already answered gets the same answer again.
* **Whom a board accepts.** A board with no session accepts whoever makes contact, and says who
  it was. One with a session accepts its own peer again, and refuses anyone else unless `accept`
  was typed in the last two minutes. A board that is refused is told nothing.
* **One peer, one handshake.** A new session replaces the old one, and while one handshake is
  under way another is not answered.

The session is saved to flash after every message: before a frame goes out, and before a
received message is shown. So a reset or a power cut carries on where it left off, never reuses
a counter, and never shows the same message twice. A handshake is not saved; a reset in the
middle of one abandons it, and `contact` starts another.

The seed and the session keys sit in flash unencrypted. Anyone holding the board can read them.
That is acceptable for a bench demo, and is one of the things a real node will do differently.

A handshake takes each board between one and one and a half seconds of arithmetic in all, on the
ESP32-S3 at 160 MHz: the core's elliptic-curve code is the portable reference, which is written to be checked
and not to be fast. The board does not listen to its console or button while it works.

## Routes

Each board announces itself by radio: every eight seconds at first, and less and less often, down
to once in eight and a half minutes, while nothing changes. From the announces it hears, a board
works out which boards it can reach and through which neighbour, as the specification's
[routing draft](https://github.com/ternmesh/spec/blob/main/draft/routing.md) describes. Two boards
on a bench find each other in under half a minute; `routes` shows it:

```
neighbours:
  482fa614  relay  up    needs -21 dBm to reach, hears us with 23 dB to spare, heard 5 s ago
routes:
  482fa614  by 482fa614  70 ms on the air
```

A board is known here by a four-byte routing id made from its address. Routing takes at most 0.5%
of a board's time on the air. A build can make a board a leaf, which announces itself and is
routed to but never through (`menuconfig`, **Relay other nodes' frames**).

Nothing in an announce is authenticated yet, and every number in the draft is the simulator's
default, not one measured on radios.

## Radio settings

The board uses the specification's [radio settings](https://github.com/ternmesh/spec/blob/main/draft/phy.md),
which are provisional: chosen from published rules and other projects' sources, and not yet
confirmed on a bench.

| Region | Frequency | Bandwidth | SF | Transmitting |
|---|---|---|---|---|
| US915 | 921.25 MHz | 500 kHz | 9 | no limit |
| EU868 | 869.475 MHz | 125 kHz | 7 | at most 10% of any hour |

Every frame has a 16-symbol preamble, coding rate 4/5 and the sync word `0x5E`. That is not
Meshtastic's (`0x2B`), MeshCore's (`0x12`) or LoRaWAN's (`0x34`), so these boards do not decode
those networks' frames, nor they these. They do hear each other as energy on the channel, as any
radio would. Two boards built before this change and after it do not hear each other either.

* **Power.** The default is +2 dBm, plenty for a bench: two boards in the same house hear each
  other easily. The radio gives up to +22 dBm. The board refuses to start if the power set, with
  the antenna's gain, is more than the region allows, which this board cannot reach with an
  ordinary antenna.
* **EU868's 10%.** The board counts the time on air of every frame it sends and refuses to send
  one that would take it past 360 s in any hour. `status` shows the count. The count is saved to
  flash, and after a restart everything in it is treated as just sent, so restarting only makes
  the wait longer.
* **The rules are yours to keep.** The profiles follow each region's rules as written (in the US,
  47 CFR 15.247's 500 kHz for a fixed channel), but nothing here is certified, and whoever
  operates a radio answers for it. Keep an antenna fitted whenever the board is powered.
* **Experiments.** `menuconfig` can also set a frequency, spreading factor, bandwidth and sync
  word that are not the region's. `status` then says so.

## How it is put together

| File | |
|---|---|
| `main/board.c` | The pins, the SPI bus, the radio's reset and BUSY line, the button and the LED. |
| `main/demo.c` | The board's identity, first contact with its retries, and the saved session. It has no hardware code, so `tests/demo.c` tests it on a host. |
| `main/main.c` | One loop that polls the radio, the serial port and the button. |
| `../../src/sx126x.c` | The SX1262 driver, part of the core and shared with future boards. |

The core is compiled into the app unchanged, from the repository's `src/`.
