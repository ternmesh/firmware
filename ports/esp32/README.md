# ESP32 boards

The port for boards with an ESP32 and an SX1262, built with ESP-IDF 5.5. Each board has an
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
with each of up to eight others. First contact follows routes too, so two boards can meet
wherever one could send the other a message: through a relay, with neither hearing the other.
There is no airtime budget yet.

## Boards

One port runs them all: what differs between boards is a row in `main/boards.c` (pins, how the
radio is wired, the screen, the battery, an amplifier) and a file in `boards/` (what ESP-IDF
needs to know, such as which USB the console is on). An image is built for one board and does not
run on another. Everything below holds for each, unless it says otherwise.

| Board | Name | Status | |
|---|---|---|---|
| Heltec WiFi LoRa 32 V3, V3.1, V3.2 | `heltec-v3` | Runs | The first board. USB through a CP2102: the port is `/dev/ttyUSB0` or similar. |
| Heltec WiFi LoRa 32 V4 (V4.2, V4.3) | `heltec-v4` | Built, not yet run on a board | Pin for pin a V3, with an amplifier after the radio for up to 28 dBm, and USB from the ESP32-S3 itself: the port is `/dev/ttyACM0` or similar. See [the V4](#the-heltec-v4). |

[Adding a board](../../docs/boards.md) says what a new one takes, and which come next.

### The Heltec V4

The V4 is read from Heltec's datasheet and its schematics for the V4.2 and V4.3. What is not a V3
about it:

* **The amplifier.** After the SX1262 comes a 17 dB attenuator and a front-end amplifier: a GC1109
  on the V4.2, a KCT8103L on the V4.3. The board powers it (GPIO7) and enables it (GPIO2) at start,
  turns it off with the board, and raises its transmit line for as long as a frame is going: GPIO46
  on the V4.2 and GPIO5 on the V4.3, both at once, since on each the other goes only to the header.
  One image runs on either. Do not wire anything to GPIO5 or GPIO46.
* **Power.** Every power here, the build's, `power` and a client's, is what goes into the antenna,
  from +4 to +28 dBm; a V4 starts at +4, the least it gives. The radio is asked for 13 dB less than
  the power set: the most the V4.2's amplifier adds after its attenuator, by the GC1109's
  datasheet (the V4.3's KCT8103L has none published). The power at the antenna is that or less,
  never more, until someone measures it with a meter. The region's limit is checked
  against the power set, as on the V3; with a 3 dBi antenna, EU868's limit stops at +26 dBm.
* **USB.** The V4 has no USB-to-serial chip. Its console and the companion link are the ESP32-S3's
  own USB Serial/JTAG, which shows as `/dev/ttyACM0` (`/dev/cu.usbmodem…` on a Mac). It needs no
  driver, and flashing needs no button: esptool resets it into its bootloader itself.

Its 16 MB flash is laid out as the V3's 8 MB is, so the addresses below are the same for both.

## Flashing

Flashing replaces whatever is on the board, Meshtastic included, along with its settings.

### Without installing anything

1. From the latest [release](https://github.com/ternmesh/firmware/releases), download the image
   for your board and where you are: `tern-<board>-us915-<version>.bin` for the United States
   and Canada, `tern-<board>-eu868-<version>.bin` for Europe, `<board>` being its name in
   [the table](#boards), such as `heltec-v3`. A board sends on its region's frequency as soon as
   it starts, so take the right one.
2. In Chrome or Edge, open [esptool-js](https://espressif.github.io/esptool-js/), plug in the
   board over USB, and press **Connect**.
3. Set the flash address to `0x0`, choose the image, and press **Program**.
4. Press the board's RST button, then open a serial terminal at 115200 baud: esptool-js has one
   under **Console**, or use the Arduino IDE's serial monitor, or `screen /dev/ttyUSB0 115200`
   (`/dev/ttyACM0` on a V4).

Flashing the full image this way also erases the board's identity and its session. It starts
again with a new address, and the other board has to make contact with that one.

To move a board that already runs Tern to a newer release and keep its address, sessions,
contacts and paired phones, write two of the release's images instead, in the same **Program**:
`-boot.bin` at `0x0` and `-update.bin` at `0xF000`. They are the full image with the part where
the board keeps those cut out. The [flash page](https://ternmesh.org/flash) does this when you
choose to update. Releases before 0.2.0 had one slot for the firmware and no `-update.bin`; a
board on one of those is moved to the [two slots](#updating-over-the-link) this way, once, and
from then on a phone can update it.

The release's `-app.bin` is the firmware alone, which a phone sends over the link. Do not write
it at `0x10000` as the first releases said: there is no firmware there any more.

Every CI run also keeps the same images of its commit, under **Artifacts** as `tern-<board>`.

### With ESP-IDF

With [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/)
installed and activated:

```bash
cd ports/esp32
./build.sh heltec-v4                                     # or heltec-v3: a file in boards/
idf.py -B build-heltec-v4 -p /dev/ttyACM0 flash monitor  # COM3 or similar on Windows
```

A plain `idf.py build` builds for the V3, as before there were other boards.

If flashing a V3 stops with "Invalid head of packet", the USB-to-serial chip is not keeping up:
add `-b 230400`.

`idf.py -B build-<board> menuconfig`, under **Tern demo**, chooses the region, the transmit power
and the antenna's gain. The default region is US915.

`./release.sh <version>` builds what a release carries, for every board an image for each region,
into `release/` (`./release.sh <version> <board>` for only that one). A tag `v<version>` makes the release from it
([release.yml](../../.github/workflows/release.yml)), with the notes in
`docs/releases/v<version>.md`.

## Using it

Type commands into the serial terminal:

| Command | |
|---|---|
| `status` | This board's address, the radio settings and its sessions. |
| `contact <address>` | Make first contact with the board whose address that is: its sixty-four digits, in either case, spaces allowed, or its link, `https://ternmesh.org/a/…`, as a phone reads it off the **Share** page. |
| `accept` | For two minutes, let a board that is not yet a peer make contact. |
| `peers` | The boards this one has a session with, by number. |
| `to <number>` | Choose the peer `send` and the button send to. |
| `send <text>` | Send up to 128 bytes to that peer: the one last made contact with, written to or heard from, unless `to` chose another. |
| `drop <number>` | End the session with a peer and forget it. The other board is not told. |
| `groups` | The [groups](#groups) this board holds, by number, and the invites it has had, by id. |
| `group new <name>` | Make a group. |
| `group invite <number>` | Invite the peer `send` sends to, to that group. |
| `group join <id>` | Take the group an invite was to. |
| `group send <number> <text>` | Write up to 128 bytes to a group. |
| `group leave <number>` | Leave a group. The others are not told. |
| `routes` | The boards this one hears, how well each hears the other, and the routes it has. |
| `selftest` | Run a handshake between two nodes in the board's memory, and time it. |
| `forget` | Forget every Bluetooth client that has paired ([the companion link](#over-bluetooth)). |
| `screen sleep <seconds>` | How long the [screen](#the-screen) stays on; `0` keeps it on. `screen` alone says. |
| `screen bench on`, `screen bench off` | Add the [bench pages](#the-bench-pages) to the screen, after the others, or take them away. The board keeps the choice. |

A short press of **PRG** shows the [screen](#the-screen)'s next page, and holding it for a second
acts on the page shown. On a [bench page](#the-bench-pages), holding it sends a ping: a board
that hears a ping answers with a pong saying how strongly it heard it, so one ping checks both
directions. (On a board whose screen does not answer, a short press sends the ping.) The white
LED blinks for each frame sent or received, and while a message is unread it blinks briefly
every four seconds, until the message is read on the screen or on a client (**Blink the LED while
a message is unread** in `idf.py menuconfig` turns that off).

Holding PRG for five seconds turns the board off. From the second second the screen counts
down, and letting go before the end leaves it on. Off, the board is in deep sleep with its
display and radio unpowered, drawing tens of microamps, and a press of PRG starts it again, as
at power on. Its identity, sessions, contacts, groups, settings and messages are in flash, and
are there when it starts.

To start:

1. Type `status` on the second board and copy its address, sixty-four hex digits.
2. On the first board, type `contact` and that address. Four frames cross, taking two or three
   seconds, and both boards say that a session has started and with whom.
3. Type `send hello` on either board: the other wakes its screen and shows it.

With only one board, `selftest` shows that the handshake and a message each way work on it, with
nothing sent.

## The screen

The board's display shows what someone carrying it needs at a glance, in their words rather than
the protocol's: eight pages, moved through by pressing PRG. It is the first version of the screen
[docs/ui.md](../../docs/ui.md) describes.

| Page | |
|---|---|
| **Home** | Bluetooth's rune beside **Tern** while a client is connected over Bluetooth; the battery's charge, once the board has read it, and the region; in large letters the one thing most worth knowing: how many new messages, or else how many nodes it hears, or that it is still listening for one. Below, what that does not say: how many nodes it hears and can reach by routes, who the last new message is from or how many of its own await delivery, and how much of the region's limit on the air is left. |
| **Messages** | One message at a time, newest first: who it is from or to (a group's by the group's name, and below it who wrote it: the contact whose address gives the routing id it claims, or else that id), how long ago (once a client has set the board's clock), and its text, wrapped. A message this board sent says what became of it: waiting, and why (no route yet, making contact, the region's limit, the radio busy), then delivered or not delivered. Hold PRG for the one before. |
| **Nearby** | The nodes it hears directly, most recently heard first, seven at a time: each by a contact's name if one has its address, or else its routing id, with how well its last announce was heard, in dB of SNR, and how long ago. Hold PRG for the next seven. |
| **Air** | The region's limit on time on the air, as a bar: what is counted against it, of how much, over what span, and when the next frame may go. In a region with no limit, how long it has sent for. |
| **Share** | Its address as a QR code holding its link, `HTTPS://TERNMESH.ORG/A/` and the address in base32, which a phone's camera opens as a web page showing the address and its short code; and beside it, its short code. |
| **This node** | Its short code, twelve digits two people compare to check a phone has the right node; its address, sixty-four hex digits in groups of eight, to read out or copy; relay or leaf, the region and the power; and the firmware's version. |
| **Phones** | How many phones have paired over Bluetooth and are remembered, and whether one is connected. Hold PRG, and hold it again within ten seconds, to forget them all, as `forget` does: each must pair again. |
| **Reset** | Erases the board for a new owner or a fresh start. Hold PRG, and hold it again within ten seconds: the board restarts, erases its flash's storage whole (its identity, sessions, contacts, groups, messages, settings and the phones' bonds), and starts as a new node, with a new address and the build's settings. The time it has spent on the air is kept, so the region's limit still counts it. |

When it starts, the board shows its name and firmware version at once, then its region and
short code as it reads them (and **New address made** on the start that gave it its address), for two and a half seconds after it is on the air, or until PRG is
pressed. If it cannot start, it says so on the screen rather than only on the console: **Did not
start**, what went wrong, what to do about it, and the version. The reasons are saved data this
firmware cannot read (erase the flash and flash it again; the board gets a new address), an
identity that could not be saved, a power more than the region allows into the antenna, no
random numbers, and a radio that did not answer, with the driver's error number. The screen then
sleeps as it otherwise would, and PRG lights it again; holding PRG turns the board off, and so
does an empty battery, as when it runs.

A message that arrives turns the screen on and shows it. It counts as read, here and on every
connected [client](#the-companion-link), once PRG is pressed while it is shown, or when a
client says so; until then Home counts it as new. Names are the ones a client saved for its
contacts; a node with none is shown by the first eight digits of its address. Characters the
font cannot draw, such as accents and emoji, show as `?`.

While a [Bluetooth client pairs](#over-bluetooth), the screen shows the passkey to type into it,
over whatever page was shown.

The screen turns off a minute after the last thing worth seeing: a press of PRG, a message heard,
or a [pairing](#over-bluetooth), whose passkey stays up until the pairing ends. A press while it
is off only turns it on again, at Home. `screen sleep <seconds>` changes the minute, and the board
keeps the change; `menuconfig`, **Seconds before the screen sleeps**, sets what a new board starts
with. Off, the panel keeps its picture and draws a few microamps instead of several milliamps.

The pages are drawn from one snapshot of the board (`main/ui.h`), not from the demo's own
variables, and only the parts of the picture that change are sent to it, a strip of eight rows
at a time between turns of the loop. If it reads upside down, `menuconfig`, **Turn the screen
upside down**.

The QR code is dark on light, as every scanner reads it: on this screen, a lit block with the
dark modules left unlit, two pixels a module, with as wide a margin as the screen leaves (three
pixels above and below, six to each side). Two decoders, ZXing's and OpenCV's, read it off the
screen's picture, blurred as a camera would see it.

The battery is read every thirty seconds, from Heltec's divider on GPIO1. Its charge is an estimate
from the voltage, good to ten percent or so, and reads high while the battery charges; with no
battery fitted, the board shows none. GPIO37 turns the divider on, low on a V3 or V3.1 and high
on a V3.2, and the board does not need telling which it is: it reads both ways the first time, and
the way that is off reads nothing. Checked against Heltec's schematics, not yet against a meter on
each revision.

At 10% or less, Home says **Low** before the charge. Below 3.3 V on two readings in a row the
board turns itself off, saying **Battery empty**, rather than run the cell down until it browns
out; it wakes every half hour to look again and starts once the battery is back above 3.45 V, or
at a press of PRG. It will not start below 3.3 V.

The board has no wire from its charger to the chip, so Home's **Chg** is inferred from the
voltage: plugging a charger in lifts it at once by 30 mV or more, and unplugging drops it as
much (`main/power.h`). It is a guess. A full battery plugged in takes no current and shows no
change, and it can take two readings, a minute, to notice.

The link and the short code are the specification's
([draft/sharing.md](https://github.com/ternmesh/spec/blob/main/draft/sharing.md)), so a phone app
shows the same code for the address it scanned. `status` on the console prints both. A phone
with no Tern app opens the link as a page on `ternmesh.org`, which reads the address out of the
link: so opening it tells the site which address was looked at. The site keeps no logs, but the
QR code is a way to hand your address to anyone who scans it, the site included. The short
code catches a mistake, and someone passing off a node of their own as yours without much effort;
the full address, in the QR code, is the strong check.

### The bench pages

For whoever is developing Tern, `screen bench on` adds four more pages after **This node**, with
the routing ids, signal margins and frame counts the console shows (`menuconfig`, **Show the
bench screen's pages**, sets what a new board starts with). On these pages, holding PRG sends a
ping.

| Page | |
|---|---|
| **Node** | The routing id, relay or leaf, the radio settings, how many boards it hears and has routes to, frames sent and heard, and time on the air: against the region's limit in EU868, in all since starting in US915. The title shows how long since it started. A `*` after the region means the build moved it off the region's settings. |
| **Neighbours** | Up to six boards it hears, those both ways first: routing id, `R` relay or `L` leaf, the link `up` or `dn`, the dBm it needs us to send at, and the dB to spare it says it hears us with (`?` until it says). |
| **Routes** | Up to six: the board, the neighbour a frame to it goes to, and the route's milliseconds on the air. |
| **Session** | The peer's first four bytes, messages sent and heard, and the last message heard. |

They are drawn from their own snapshot (`main/status.h`).

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

First contact is the specification's: an EDHOC handshake of four frames, of 56, 60, 80 and 24
bytes. Neither address goes over the air in clear, each board proves it holds the key behind its
address, and each handshake gives a new session secret, so nothing has to be remembered to stop
keys repeating. The frames go by routes, as messages do, so they do carry both boards' routing
ids, and relays pass them on.

Lost frames are the specification's as well. The board that began keeps each of its two frames
as it keeps a message: it waits five seconds and a little for the answer, sends the frame again
up to three times, and then gives up. The other board never sends unasked: a frame it has
already answered gets the same answer again, and it keeps a handshake for a minute after it last
heard anything of it. Between two boards on a bench every frame is sent once, and a handshake
takes under two seconds.

What the specification has not settled yet, the demo decides for itself. None of this is Tern
yet:

* **Whom a board accepts.** A board accepts an address saved as a contact, as the companion draft
  says. Beyond that, a board with no session accepts whoever makes contact, and says who it was;
  one with a session accepts a peer it has again, and refuses anyone else unless `accept` was
  typed in the last two minutes. A board that is refused is told nothing; the refusing board
  tells its clients who asked (`ASKED`), so they can offer to save it.
* **Eight peers, one handshake.** First contact with a peer the board has replaces the session
  with that peer, and leaves the others. A board with eight sessions takes no new peer until one
  is dropped: it does not choose whom to forget. While one handshake is under way another is not
  answered, unless it says it comes from the board that began the one under way: that board has
  started again.
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

* **Power.** The default is +2 dBm (+4 on a V4), plenty for a bench: two boards in the same house
  hear each other easily. The V3 gives up to +22 dBm, the V4 [up to +28](#the-heltec-v4). The board refuses
  to start if the power set, with the antenna's gain, is more than the region allows, which a V3
  cannot reach with an ordinary antenna and a V4 can.
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

## Groups

A group is the boards that hold one secret, as the draft's
[Groups](https://github.com/ternmesh/spec/blob/main/draft/groups.md) has it. A message to a
group is one frame, [flooded](https://github.com/ternmesh/spec/blob/main/draft/flooding.md): every
relay that hears it sends it once more, unless it hears another do so first, and every board
that holds the secret reads it. From the console:

1. `group new Hut` on one board makes a group, and `groups` lists it as number 1.
2. `group invite 1` sends its secret to the peer `send` sends to, over their session.
3. On that board, `groups` shows the invite and its id, and `group join <id>` takes the group.
4. `group send 1 <text>` on either writes to it.

What to know before trusting one with anything:

* **Any member can write as any other.** A group's frames say who wrote them, and every member
  holds the key that says so. The console prints the routing id a frame gave and nothing more.
* **Whoever gets the secret reads everything**, past frames they recorded included, and nobody
  can be put out of a group. The others start a new one.
* **Nothing says a message arrived.** A group message is waiting, then sent once it has gone on
  the air. No board answers it.
* **A board writes only so much.** Its own group messages take at most 0.5% of its time, about
  two dozen short ones at once and one every 25 seconds after; it tells a client when one waits
  for that. It spends up to 3% of its time passing on other boards'.
* **A busy board passes fewer on.** Once its radio has spent more than a fifth of the last half
  minute to minute sending or receiving, it drops some of the group frames it would pass on,
  more of them the busier it is, and its own never. It counts what it sent and what it received
  whole: a frame lost part-way is not counted, so it takes itself for less busy than it is.
* **A message recorded and sent again is not read again**, with four exceptions. Each board
  numbers the group frames it writes, and a board keeps, for each of up to sixteen writers in a
  group, the highest number it has read and which of the 31 below it. It keeps them in flash,
  written at most once a minute. So an old frame sent again by someone who recorded it, which
  needs no secret, is dropped. It is still read as new:
  * by a board that never received it and has read nothing later from its writer;
  * by a board that lost power within a minute of first reading it;
  * by a board so full of sessions, groups and messages that flash had no room to spare for
    the numbers, after it restarts;
  * in a group where more than sixteen boards write, when its writer is the one the board has
    gone longest without hearing.
* **Boards on 0.2.0-alpha.1 or earlier and boards on this cannot read each other's group
  messages.** The frame changed to carry the number, and each takes the other's for a frame
  that fails its check. Update every board in a group together. Groups, their names and saved
  messages are kept.
* **One member can stop another being read.** A group's frames say who wrote them and prove
  nothing, so a member who writes under another's id with a high number has the rest drop that
  board's own frames from then on.

## Positions

A client can give the board its position and have it shared, as
[positions](https://github.com/ternmesh/spec/blob/main/draft/positions.md) says, with the contacts
and groups the user chooses, at the precision the user chooses: `SET_POSITION`, then `SHARE` or
`SHARE_GROUP` (companion protocol version 5). The board has no receiver of its own, so it shares
nothing until a client has given it a position. It works out the cell, decides when one is due,
and sends it to a contact as a message for its node, sealed and acknowledged as a message is. What it receives from a
contact is told to every client as a `POSITION`, and from a group member as a
`GROUP_POSITION`.

* **To a contact, only over a session.** A position never makes first contact: to a contact the
  board shares no session with, nothing goes.
* **Seldom.** One goes when the board has moved to another cell, no sooner than the interval the
  user chose (a minute at least), and again after an hour if it has not. One at a time to each
  contact: the next waits until the last is acknowledged or given up. A fix more than an hour old
  is not sent. Messages go first.
* **A cell's edge.** Once a cell has gone, the board takes itself to be in it until it is more
  than a quarter of a cell outside it.
* **Turning sharing off** sends the contact or group a stopped position, once, if a position had
  gone. A node that receives one forgets the last: one that keeps it anyway cannot be stopped.
* **Not across a restart.** Sharing is not saved: after a restart the board shares with nobody
  until a client turns it on again, and a client's next sync says so. Received positions are not
  saved either, and are forgotten a day after they came.
* **To groups**, by `SHARE_GROUP`, in a group frame with its `node` flag set, flooded as a
  group's message is: no more often than every five minutes, and only while the board's
  allowance for its own floods would still hold a full frame of words after it. Nothing answers
  it, and a position from a group member is what that member claimed: any member could have
  written it. The board keeps the last from each of up to eight members of each group.
* **Leaving a group** sends no stopped position: the group's keys go with it.

## The companion link

The same USB port speaks the
[companion protocol](https://github.com/ternmesh/spec/blob/main/draft/companion.md), so a
program on a computer can drive the board while the console carries on. Its frames start with
the byte `0xF5`, which typed text never contains, so the board tells them from commands byte by
byte, and a terminal that has not said `HELLO` is never sent one.

`tools/companion.py`, at the top of the repository, is an example client. It speaks version 1
of the protocol, from before groups, so the board tells it of none. A client that says version
2 is told of groups, their messages and invites, and can make, join and write to them. Its
`update` command says version 4, and [updates the firmware](#updating-over-the-link).

```bash
pip install pyserial                       # on Linux and macOS, optional
python3 tools/companion.py --port /dev/ttyUSB0 state
python3 tools/companion.py --port /dev/ttyUSB0 contact <address> Bob
python3 tools/companion.py --port /dev/ttyUSB0 send <address> "On the ridge by six"
python3 tools/companion.py --port /dev/ttyUSB0 watch
python3 tools/companion.py --port /dev/ttyUSB0 update tern-heltec-v3-eu868-<version>-app.bin
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

* **Messages** are the last 32 sent and received, saved to flash as they come, so a board with
  no client attached holds what arrived until one asks, through a restart or a flat battery. A
  message that was waiting when the board restarted and had not yet been handed over to be sent
  goes once it starts. One to an address that had been handed over is shown as not delivered:
  the board no longer listens for its acknowledgement and has given it up, though it may have
  arrived. An invite not yet joined is kept, with the group's secret, like any other message.
  **Not all 32 are always saved.** Messages share 24 KB of flash with everything else the board
  saves, and have about a quarter of it, so that they are never why a session or a contact
  cannot be saved: room for 26 messages of up to 23 bytes, 20 of 60 bytes, or 16 at the
  full 128. The newest are the ones saved: when there is no room for another, the oldest
  leave flash for it, and are held in memory until a restart; `status` says how many are saved
  and how many are not. More
  wants a part of the flash for messages alone, which the layout leaves room for past its two
  slots for the firmware (`partitions.csv`).
  Nothing in flash is encrypted: whoever holds the board can read them, as they can its keys.
  A message's id is greater than every one before it, across restarts too, so a client that asks
  for what is new since the last id it holds is never answered with nothing. A `SEND` repeated
  with the same `ref` after a restart is sent twice: the board does not save which it has had.
  Contacts, up to 16, are saved to flash.
* **A session with each of eight nodes.** A message to a node the board has no session with
  starts first contact with it. Its state says it is waiting for a session meanwhile, and "not
  delivered" if the handshake gives up, or if the board already holds eight sessions. Removing
  a contact does not end its session; `END_SESSION`, or `drop` on the console, does, and gives up
  the messages to that node not yet delivered, on the air or not. The other board is not told, and keeps its half
  until one of them makes first contact again.
* **Groups**, up to four, with their secrets, saved to flash: see [Groups](#groups).
* **Letting a board in.** Saving an address as a contact lets that board make first contact
  whenever it tries. One that is refused, for not being a contact or for want of room, is news
  to every client of version 1 or later (`ASKED`), at most once every ten seconds for each
  address.
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
* **Battery**: its voltage, and the charge it is taken for, as [the screen](#the-screen) shows
  them; unknown when none is fitted. Whether it is charging is not known: the charger tells only
  its LED.

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
connect again without a passkey. `forget` on the console, or the screen's **Phones** page,
forgets every bonded client.

Bluetooth is always on. What it costs a battery is not measured yet.

### Updating over the link

A client can give the board new firmware over the link, as the draft's
[updates](https://github.com/ternmesh/spec/blob/main/draft/companion.md#updating-the-firmware)
have it: the Tern phone apps over Bluetooth, or `tools/companion.py update` over USB. The image
is the release's `-app.bin` for the board's region, about 700 KB, which takes a few minutes over
Bluetooth; the screen shows how much has arrived. Everything the board saves stays: its
address, sessions, contacts, groups, messages, settings and paired phones. A board whose region
was the build's keeps the one it is on, whichever region's image it is sent.

The flash holds two slots for the firmware (`partitions.csv`), and the image is written into the
one not running as it arrives, while the board goes on as before, on the air. A link that drops
goes on from where it stopped. Once the image is whole, its SHA-256 the one the client gave, and
ESP-IDF finds it a Tern image for this chip, the board restarts into it. If the new firmware
cannot start, as **Did not start** would say, or the board restarts before it is on the air, the
bootloader goes back to the firmware it ran before.

`INFO` names the board, `heltec-v3` or `heltec-v4`, and the firmware's release, so a client can
find the image.
A board flashed with a release from before 0.2.0 has one slot and cannot be updated this way; it
names no board, and is moved to two slots [over USB](#without-installing-anything) once.

Anyone who has paired can update the board, as they can change its region. The image is not
signed: the digest says it arrived whole, not who made it.

## How it is put together

| File | |
|---|---|
| `main/boards.c` | Each board's pins and what it has fitted, from its maker's documents, and how much less to ask of the radio on a board with an amplifier. No hardware code; tested on a host by `tests/boards.c`. |
| `main/board.c` | The chosen board's hardware: the SPI bus, the radio's reset and BUSY line, an amplifier, the button, the LED, the display (an SSD1306 or SSD1315 on its own I2C bus), the battery and the console (UART or USB Serial/JTAG). |
| `main/demo.c` | The board's identity, first contact, and the saved sessions. It has no hardware code, so `tests/demo.c` tests it on a host, and `tests/relay.c` with the router and the forwarder: boards that make a session through a relay. |
| `main/ui.c` | The screen's pages, drawn from a snapshot of the node in the user's words. Tested on a host by `tests/ui.c`, which also writes each page it checks as a picture: `build/test_ui <directory>`. |
| `main/qr.c` | The QR code: version 3, level L, alphanumeric, written from ISO/IEC 18004. `tests/qr.c` checks every mask against another encoder's symbols. |
| `main/power.c` | Which of the battery's readings to believe, and the charge a voltage is taken for. Tested on a host by `tests/power.c`; `board.c` does the reading. |
| `main/status.c` | The bench pages, and the snapshot they are drawn from, as lines of text. Tested on a host by `tests/status.c`. |
| `main/display.c` | The picture of the screen, its font in two sizes, the bar, and which parts of it have changed. |
| `main/link.c` | The companion link: contacts, messages and what became of them, positions and whom they are shared with, updates, and the answers and news each client gets, on USB and over Bluetooth. No hardware code; tested on a host by `tests/link.c`, and with `tools/companion.py` by `tests/link_script.py`. |
| `main/ble.c` | The companion link's Bluetooth LE service, pairing and advertising, over NimBLE, which runs in its own task and reports to the loop through a queue. |
| `main/main.c` | One loop that polls the radio, the serial port, the button and the screen. |
| `../../src/sx126x.c` | The SX1262 driver, part of the core and shared with every board. |

The core is compiled into the app unchanged, from the repository's `src/`.
