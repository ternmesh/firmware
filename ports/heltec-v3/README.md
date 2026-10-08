# Heltec WiFi LoRa 32 V3

The first board port: an ESP32-S3 with an SX1262, built with ESP-IDF 5.5. Each board has an
address. One makes [first contact](https://github.com/ternmesh/spec/blob/main/draft/first-contact.md)
with the other's, and the two then send each other
[secured unicast frames](https://github.com/ternmesh/spec/blob/main/draft/unicast-security.md)
over the air.

It is a bench demo, not a node. Boards announce themselves and learn routes to each other, and a
message [follows them](https://github.com/ternmesh/spec/blob/main/draft/forwarding.md): it goes
to the next hop its route gives, at no more power than that hop needs, is sent again if nothing is
heard of it, and is acknowledged by the board it is for. A board built as a relay passes other
boards' frames on. A board listens first: it asks its radio before every frame, and sends none
while one is being received (`status` counts the times a frame waited). A board holds a session
with each of up to eight others. But first contact does not follow routes, so two boards must
hear each other to meet. There is no airtime budget yet either.

## Flashing

Flashing replaces whatever is on the board, Meshtastic included, along with its settings.

### Without installing anything

1. From the latest [release](https://github.com/ternmesh/firmware/releases), download the image
   for where you are: `tern-heltec-v3-us915-<version>.bin` for the United States and Canada,
   `tern-heltec-v3-eu868-<version>.bin` for Europe. A board sends on its region's frequency as
   soon as it starts, so take the right one.
2. In Chrome or Edge, open [esptool-js](https://espressif.github.io/esptool-js/), plug in the
   board over USB, and press **Connect**.
3. Set the flash address to `0x0`, choose the image, and press **Program**.
4. Press the board's RST button, then open a serial terminal at 115200 baud: esptool-js has one
   under **Console**, or use the Arduino IDE's serial monitor, or `screen /dev/ttyUSB0 115200`.

Flashing the full image this way also erases the board's identity and its session. It starts
again with a new address, and the other board has to make contact with that one.

To move a board that already runs Tern to a newer release and keep its address, sessions and
contacts, write the release's `-app.bin` image at `0x10000` instead.

Every CI run also keeps the same images of its commit, under **Artifacts** as `tern-heltec-v3`.

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
antenna's gain. The default region is US915.

`./release.sh <version>` builds what a release carries, an image for each region, into
`release/`. A tag `v<version>` makes the release from it
([release.yml](../../.github/workflows/release.yml)), with the notes in
`docs/releases/v<version>.md`.

## Using it

Type commands into the serial terminal:

| Command | |
|---|---|
| `status` | This board's address, the radio settings and its sessions. |
| `contact <address>` | Make first contact with the board whose address that is. |
| `accept` | For two minutes, let a board that is not yet a peer make contact. |
| `peers` | The boards this one has a session with, by number. |
| `to <number>` | Choose the peer `send` and the button send to. |
| `send <text>` | Send up to 128 bytes to that peer: the one last made contact with, written to or heard from, unless `to` chose another. |
| `drop <number>` | End the session with a peer and forget it. The other board is not told. |
| `routes` | The boards this one hears, how well each hears the other, and the routes it has. |
| `selftest` | Run a handshake between two nodes in the board's memory, and time it. |
| `forget` | Forget every Bluetooth client that has paired ([the companion link](#over-bluetooth)). |
| `screen sleep <seconds>` | How long the [screen](#the-screen) stays on; `0` keeps it on. `screen` alone says. |

Holding **PRG** for a second sends a ping. A board that hears a ping answers with a pong saying
how strongly it heard it, so one ping checks both directions. A short press shows the
[screen](#the-screen)'s next page. (On a board whose screen does not answer, a short press sends
the ping.) The white LED blinks for each frame sent or received.

To start:

1. Type `status` on the second board and copy its address, sixty-four hex digits.
2. On the first board, type `contact` and that address. Four frames cross, taking two or three
   seconds, and both boards say that a session has started and with whom.
3. Hold PRG on either board for a second.

With only one board, `selftest` shows that the handshake and a message each way work on it, with
nothing sent.

## The screen

The board's display shows what the console would, without a laptop: four pages, moved through by
pressing PRG.

| Page | |
|---|---|
| **Node** | The routing id, relay or leaf, the radio settings, how many boards it hears and has routes to, frames sent and heard, and time on the air: against the region's limit in EU868, in all since starting in US915. The title shows how long since it started. A `*` after the region means the build moved it off the region's settings. |
| **Neighbours** | Up to six boards it hears, those both ways first: routing id, `R` relay or `L` leaf, the link `up` or `dn`, the dBm it needs us to send at, and the dB to spare it says it hears us with (`?` until it says). |
| **Routes** | Up to six: the board, the neighbour a frame to it goes to, and the route's milliseconds on the air. |
| **Session** | The peer's first four bytes, messages sent and heard, and the last message heard. |

It is a bench screen, for whoever is developing Tern, and it will be thrown away. What a Tern node
should show the person carrying it is a different question, and
[docs/ui.md](../../docs/ui.md) is where it is being worked out.

The screen is drawn from one snapshot of the board (`main/status.h`), not from the demo's own
variables, and only the lines that change are sent to it, one at a time between turns of the
loop. If it reads upside down, `menuconfig`, **Turn the screen upside down**.

The screen turns off a minute after the last thing worth seeing: a press of PRG, a message heard,
or a [pairing](#over-bluetooth), whose passkey stays up until the pairing ends. A press while it
is off only turns it on again. `screen sleep <seconds>` changes the minute, and the board keeps
the change; `menuconfig`, **Seconds before the screen sleeps**, sets what a new board starts with.
Off, the panel keeps its picture and draws a few microamps instead of several milliamps.

## Identity and first contact

A board makes its identity the first time it starts: 32 random bytes from the ESP32's hardware
generator, saved to flash. Its address is the Ed25519 public key of that seed, and stays the same
until the flash is erased.

The generator gives true random numbers only while it has a source of entropy. Espressif's
[documentation for it](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/random.html)
names two: the chip's internal noise source, which `bootloader_random_enable()` turns on, and
the radio, while Wi-Fi or Bluetooth is on. The noise source must be off before Bluetooth starts.
So the board starts with the noise source on, makes its identity and the router's seed from it,
and turns it off as Bluetooth starts; from then on, Bluetooth is the source, and every key made
after it, a first contact's included, is made from it. If Bluetooth does not start, the noise
source goes back on.

First contact is the specification's: an EDHOC handshake of four frames, of 45, 53, 73 and 17
bytes. Neither address goes over the air in clear, each board proves it holds the key behind its
address, and each handshake gives a new session secret, so nothing has to be remembered to stop
keys repeating.

What the specification has not settled yet, the demo decides for itself. None of this is Tern
yet:

* **Lost frames.** The board that began sends each of its two frames up to four times, a little
  over two seconds apart at the default settings, and then gives up. The other board never sends
  unasked: a frame it has already answered gets the same answer again.
* **Whom a board accepts.** A board accepts an address saved as a contact, as the companion draft
  says. Beyond that, a board with no session accepts whoever makes contact, and says who it was;
  one with a session accepts a peer it has again, and refuses anyone else unless `accept` was
  typed in the last two minutes. A board that is refused is told nothing; the refusing board
  tells its clients who asked (`ASKED`), so they can offer to save it.
* **Eight peers, one handshake.** First contact with a peer the board has replaces the session
  with that peer, and leaves the others. A board with eight sessions takes no new peer until one
  is dropped: it does not choose whom to forget. While one handshake is under way another is not
  answered.
* **Listening for the answer.** Once a frame that follows a route has gone, the board sends no
  other for three times as long as it took: the radio cannot hear while it sends, and a board
  with several messages waiting would otherwise send the second over the acknowledgement of the
  first.

The session is saved to flash after every message: before a frame goes out, and before a
received message is shown or acknowledged. So a reset or a power cut carries on where it left
off, never reuses a counter, and never shows the same message twice. A message that comes again
because its acknowledgement was lost is acknowledged again, and not shown again. A handshake is not saved; a reset in the
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
on a bench find each other within a minute; `routes` shows it:

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

## On a bench

For measuring what one radio hears of another, the board has a second set of commands. `bench on`
stops routing and first contact, so that the only frames a board sends are the ones asked for,
until `bench off` or a restart.

| Command | |
|---|---|
| `bench on`, `bench off` | |
| `sync <hex>` | The sync word, in its one-byte form: `sync 2B` is Meshtastic's. |
| `power <dBm>` | The power frames are sent at, -9 to 22. |
| `freq <Hz>`, `sf <n>`, `bw <Hz>` | The channel and modulation, kept inside the region's band: another network's, to listen for it or be heard by it. |
| `beacon <count> <ms>` | Send so many 24-byte test frames, so far apart. |
| `counts`, `counts reset` | Frames sent, and what the receiver saw: preambles, headers that checked, headers and frames that did not, whole frames, and of those the test frames. |

A frame with another sync word shows as a preamble and nothing more. The simulator's
`calibration/syncword/run.py` drives two boards through every pair of sync words this way. A
board set to another network's sync word hears that network and can disturb it: use these where
none is in range, and put the board back (`sync 5E`, or restart it) afterwards.

## The companion link

The same USB port speaks the
[companion protocol](https://github.com/ternmesh/spec/blob/main/draft/companion.md), so a
program on a computer can drive the board while the console carries on. Its frames start with
the byte `0xF5`, which typed text never contains, so the board tells them from commands byte by
byte, and a terminal that has not said `HELLO` is never sent one.

`tools/companion.py`, at the top of the repository, is an example client:

```bash
pip install pyserial                       # on Linux and macOS, optional
python3 tools/companion.py --port /dev/ttyUSB0 state
python3 tools/companion.py --port /dev/ttyUSB0 contact <address> Bob
python3 tools/companion.py --port /dev/ttyUSB0 send <address> "On the ridge by six"
python3 tools/companion.py --port /dev/ttyUSB0 watch
```

`state` sets the board's clock from the computer's and prints what it holds: itself, its
contacts, the messages it has kept, the boards it hears, and its time on the air. Close the
serial monitor first: only one program can have the port.

The board cannot tell when a program closes the port: the USB bridge keeps it open on the
board's side. So a client that sends no request for a minute is taken for gone, and the board
stops sending it news until something says `HELLO` again. Without this, the next program to open
the port, a terminal included, would get the last one's frames. `watch` sends a `PING` every 20
seconds to stay connected, and if it is cut off anyway, it says `HELLO` again and catches up.

What the board offers is what the demo is:

* **Messages** are the ones sent and received since it started, up to 32; they are not saved.
  Their ids are: a message's id is greater than every one before it, across restarts too, so a
  client that asks for what is new since the last id it holds is never answered with nothing.
  Contacts, up to 16, are saved to flash.
* **A session with each of eight nodes.** A message to a node the board has no session with
  starts first contact with it. Its state says it is waiting for a session meanwhile, and "not
  delivered" if the handshake gives up, or if the board already holds eight sessions. Removing
  a contact does not end its session; `END_SESSION`, or `drop` on the console, does, and gives up
  the messages to that node not yet delivered, on the air or not. The other board is not told, and keeps its half
  until one of them makes first contact again.
* **Letting a board in.** Saving an address as a contact lets that board make first contact
  whenever it tries. One that is refused, for not being a contact or for want of room, is news
  to every client of version 1 (`ASKED`), at most once every ten seconds for each address.
* **A message is waiting, then delivered or not delivered.** It is delivered when its
  destination's acknowledgement comes back, and not delivered when the board gives it up, after
  four tries of some five seconds each. While it waits, its reason says if there is no route to
  its destination or the region's limit is holding it. It is never "sent": that means a
  neighbour was heard passing it on, which the board does not report yet, and the draft forbids
  claiming more than the node knows.
* **Four of the board's own messages** are on their way at once; the rest wait their turn.
* **Settings.** Region, role and power are saved to flash and applied by a restart, after the
  board has answered; a power or region the antenna setting does not allow is refused. The
  Bluetooth passkey applies from the next pairing.
* **Battery** is not measured yet, and is reported as unknown.

Messages sent with `send` and pings go the same way as a client's, so a client sees them too.

### Over Bluetooth

The board also offers the link over Bluetooth LE, as the draft's
[Bluetooth profile](https://github.com/ternmesh/spec/blob/main/draft/companion.md#bluetooth-le)
has it. It advertises the service as `Tern`, with nothing of its address, and takes one client
at a time, beside one on USB. Both are clients at once: each says `HELLO` and syncs for itself,
and both hear what changes.

A client must pair before it can write or hear anything: LE Secure Connections only, with a
passkey. By default the passkey is a new one for each pairing, shown on the screen; type it into
the client. `SET` 4 sets a fixed one instead, from 0 to 999999. A board with no screen and no
fixed passkey does not pair. A client that has paired keeps its bond, kept in flash, and may
connect again without a passkey. `forget` on the console forgets every bonded client.

Bluetooth is always on. What it costs a battery is not measured yet.

## How it is put together

| File | |
|---|---|
| `main/board.c` | The pins, the SPI bus, the radio's reset and BUSY line, the button, the LED, and the display (an SSD1306 on its own I2C bus). |
| `main/demo.c` | The board's identity, first contact with its retries, and the saved session. It has no hardware code, so `tests/demo.c` tests it on a host. |
| `main/status.c` | The snapshot the screen is drawn from, and its pages as lines of text. |
| `main/display.c` | The picture of the screen, its font, and which parts of it have changed. With `status.c`, tested on a host by `tests/status.c`. |
| `main/link.c` | The companion link: contacts, messages and what became of them, and the answers and news each client gets, on USB and over Bluetooth. No hardware code; tested on a host by `tests/link.c`, and with `tools/companion.py` by `tests/link_script.py`. |
| `main/ble.c` | The companion link's Bluetooth LE service, pairing and advertising, over NimBLE, which runs in its own task and reports to the loop through a queue. |
| `main/main.c` | One loop that polls the radio, the serial port, the button and the screen. |
| `../../src/sx126x.c` | The SX1262 driver, part of the core and shared with future boards. |

The core is compiled into the app unchanged, from the repository's `src/`.
