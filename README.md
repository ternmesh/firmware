# Tern firmware

The reference implementation of Tern: a portable C protocol core that also builds on Linux (for infrastructure nodes on single-board computers, and for host tests), plus ports for the boards it runs on ([docs/boards.md](docs/boards.md)).

Part of **Tern**, a LoRa mesh protocol that treats airtime as a shared,
metered resource. The protocol is defined by the specification in
[ternmesh/spec](https://github.com/ternmesh/spec), not by this code.

**Status:** early. The core implements the four parts of the protocol the specification has
drafted, radio settings, secured unicast frames, first contact and routes, and passes every one of
their test vectors.
The same node runs them over the air as a bench demo, on two families of chip: [ESP32
boards](ports/esp32/) (the Heltec V3, and built for the Heltec V4 and five more of Heltec's) and [nRF52840
boards](ports/nrf52/) (built for the Heltec T114). Nodes find
routes to each other, and a message follows them: it is sent to the next hop, sent again if
nothing is heard of it, and acknowledged by the node it is for.

| Module | Header | What it is |
|---|---|---|
| Time on air | `tern/lora.h` | LoRa time on air (Semtech's formula, SF7–SF12), in integer nanoseconds, matching the simulator's to the nanosecond. The airtime budget and the routing metric are both measured in it. |
| Radio settings | `tern/region.h` | The sync word and settings every frame uses, and a profile for each region, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/phy.md): US915 and EU868, provisional until a bench confirms them. Checked against the specification's table. |
| Transmit limit | `tern/duty.h` | The account of time on air that holds a node to a region's limit on transmitting, such as 10% of any hour. Not the airtime budget, which is not specified yet. |
| Radio seam | `tern/radio.h` | The interface each board port implements for its radio, and the checks the core makes before a port is called. Events are polled, never delivered in interrupt context. |
| Addresses | `tern/address.h` | A node's identity and address, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/first-contact.md#addresses): an Ed25519 key pair from a seed, checking an address (prime-order subgroup included), converting it to the X25519 key the handshake uses, and Ed25519 signatures, made and checked, for presence cards. |
| First contact | `tern/contact.h` | The EDHOC handshake that gives two nodes a unicast session, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/first-contact.md): both roles, four frames that follow routes, contact tags, and erasure as the specification requires. About 3.6 KB of stack at its deepest. |
| Routes | `tern/route.h` | How a node learns which neighbour to hand a frame to, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/routing.md): announces, links judged by signal strength each way, loop-free route selection (Babel's feasibility condition), requests, Trickle and a cap on routing's share of the air. The simulator's candidate 3, as far as choosing routes; every parameter provisional. A node that restarts says so and waits before it routes through others, which narrows but does not close the one way a loop can form. A full table of neighbours keeps every link that is up, and otherwise the nearest. Not yet: broadcast and authentication. |
| Frames that follow routes | `tern/forward.h` | How a message is carried along those routes, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/forwarding.md): each frame names its destination and the neighbour it is for now, a node hears its neighbour pass a frame on and sends it again if it does not, then tries another route, and a message's source sends it again until its destination's acknowledgement comes back. First contact's frames go the same way, each kept by the node that began the handshake until the next answers it. Measured in the simulator beside Meshtastic and MeshCore (that draft's rationale). |
| Frames for every node | `tern/flood.h` | The flood, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/flooding.md): a frame that names no node, known by a hash of its bytes, passed on once by each relay that has not heard another pass it on first, a few relays deep. Two token buckets bound what a node spends on its own floods and on others'. No board uses it yet. |
| Groups | `tern/group.h` | A group's frames, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/groups.md): a shared secret, a random nonce and a tag only members can work out, so nothing in clear names the group or the writer; a count from each writer, sealed, so that a frame recorded and sent again is not read again; receiving for every group a node holds; the invite that hands a group over a session; and the join code, a link a QR code holds, that hands one over off the air. A member can write as any other, and there is no forward secrecy: the header says so first. No board uses it yet. |
| Positions | `tern/position.h` | Where a node is, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/positions.md): a cell of a grid the whole world shares, as coarse as its user chooses, the bytes it goes in, what a node keeps of one received, and when one is due. Integers throughout. The Heltec V3 shares one with the contacts and groups a client chooses. |
| Presence cards | `tern/card.h` | Who is about, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/cards.md): a node's address and a name its user chose, signed with Ed25519 and flooded two hops, only while the user has cards on; checking one received, and holding the newest from each address for a day. Every node holds the cards it hears; a board sends its own about every two hours. |
| Listening first | `tern/listen.h` | When a node may start to send, from the same draft: not while its radio is receiving a frame. It keeps what a radio has said - a preamble, a header, a frame's end - and answers whether it is receiving; the SX126x driver feeds it and a port asks before each frame (`tern_radio_receiving()`). |
| Sharing an address | `tern/share.h` | How an address leaves the air, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/sharing.md): its text form, the `ternmesh.org` link a QR code holds, with the address in base32, reading either back as a person gives it, and the twelve-digit short code two people compare. Its base32 is a join code's too. |
| Unicast frames | `tern/unicast.h` | Secured unicast, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/unicast-security.md): sealing a message into a frame, and recognising, authenticating and opening frames for any of a node's sessions; and the acknowledgement its destination answers with, which only the two ends can make or check. Keys are erased as the specification requires. About 790 bytes of RAM per session. |
| SX1262 driver | `tern/sx126x.h` | The radio seam for Semtech's SX1262, written from its datasheet, errata included. Portable, so every SX1262 board shares it. |
| Crypto | `tern/crypto.h` | SHA-256, SHA-512, HMAC, HKDF-Expand, AES-128, AES-CCM and X25519, in portable constant-time C, each tested against its standard's published vectors. A reference to check hardware against, not a fast one. |

* [docs/architecture.md](docs/architecture.md) — the core, the ports, and the seam between them
* [docs/ui.md](docs/ui.md) — what a node shows the person carrying it, and what it will
* [docs/boards.md](docs/boards.md) — adding a board, and which boards come next
* [CONTRIBUTING.md](CONTRIBUTING.md) — DCO sign-off and the clean-room rule
* [Governance](https://github.com/ternmesh/spec/blob/main/GOVERNANCE.md)

## Building

Linux or macOS, with CMake 3.20 or later and a C17 compiler (on Windows, use WSL).

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

To check that the core still builds for a microcontroller (a Cortex-M4F, the nRF52840's class),
with `arm-none-eabi-gcc` installed:

```bash
cmake -S . -B build-arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DBUILD_TESTING=OFF
cmake --build build-arm
```

The simulator ([ternmesh/sim](https://github.com/ternmesh/sim)) links this core and runs it as
its `core` routing, so a change to the routing can be run over a mesh of hundreds of nodes before
it reaches a board. With a checkout of it beside this one:

```bash
cmake -S ../sim -B ../sim/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTSIM_FIRMWARE_DIR=$PWD
cmake --build ../sim/build
ctest --test-dir ../sim/build -R core
```

CI runs the same.

`-DTERN_SANITIZE=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer, and
`-DTERN_WERROR=ON` makes warnings errors; CI runs both, with GCC and Clang. Format with
`clang-format` 18 before pushing.

## Boards

Every board runs the same bench demo: first contact, then messages that follow routes and are
acknowledged, with a screen that says, in plain words, what it hears, what came in and how much of
the air it may use. CI builds an image of each that you can flash from a browser.

| Board | Port | |
|---|---|---|
| Heltec WiFi LoRa 32 V3 (ESP32-S3, SX1262) | [`ports/esp32/`](ports/esp32/) | Runs. |
| Heltec WiFi LoRa 32 V4 (ESP32-S3, SX1262, 28 dBm amplifier) | [`ports/esp32/`](ports/esp32/) | Built from Heltec's schematics; not yet run on a board. |
| Heltec Wireless Stick Lite V3, Wireless Tracker, Vision Master E290 and E213, Wireless Paper (ESP32-S3, SX1262) | [`ports/esp32/`](ports/esp32/) | Built from Heltec's schematics; not yet run on a board. Their screens are not driven yet. |
| Heltec WiFi LoRa 32 V2 and V2.1, LilyGo LoRa32 T3 V1.6.1 and T-Beam V1.0 to V1.2 (ESP32, SX1276); LilyGo T3-S3 (ESP32-S3, SX1276) | [`ports/esp32/`](ports/esp32/) | Built from their makers' schematics; not yet run on a board. |
| Heltec Mesh Node T114 V2 (nRF52840, SX1262) | [`ports/nrf52/`](ports/nrf52/) | Built on Zephyr; not yet run on a board. |
| RAKwireless RAK4631, Seeed Wio Tracker L1 (nRF52840, SX1262) | [`ports/nrf52/`](ports/nrf52/) | Built on Zephyr from their makers' documents; not yet run on a board. |

A board on a family the firmware has is a row in a table or an overlay; [docs/boards.md](docs/boards.md)
says how to add one, and the plan for every board Meshtastic and MeshCore run on.

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE).
