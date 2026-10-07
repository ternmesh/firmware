# Tern firmware

The reference implementation of Tern: a portable C protocol core that also builds on Linux (for infrastructure nodes on single-board computers, and for host tests), plus ports for each supported board.

Part of **Tern**, a LoRa mesh protocol that treats airtime as a shared,
metered resource. The protocol is defined by the specification in
[ternmesh/spec](https://github.com/ternmesh/spec), not by this code.

**Status:** early. The core implements the four parts of the protocol the specification has
drafted, radio settings, secured unicast frames, first contact and routes, and passes every one of
their test vectors.
One board runs them over the air: the [Heltec V3](ports/heltec-v3/), as a two-board bench demo.
Nodes find routes to each other, but nothing is sent along them yet: the specification has not
drafted the frames that follow routes.

| Module | Header | What it is |
|---|---|---|
| Time on air | `tern/lora.h` | LoRa time on air (Semtech's formula, SF7–SF12), in integer nanoseconds, matching the simulator's to the nanosecond. The airtime budget and the routing metric are both measured in it. |
| Radio settings | `tern/region.h` | The sync word and settings every frame uses, and a profile for each region, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/phy.md): US915 and EU868, provisional until a bench confirms them. Checked against the specification's table. |
| Transmit limit | `tern/duty.h` | The account of time on air that holds a node to a region's limit on transmitting, such as 10% of any hour. Not the airtime budget, which is not specified yet. |
| Radio seam | `tern/radio.h` | The interface each board port implements for its radio, and the checks the core makes before a port is called. Events are polled, never delivered in interrupt context. |
| Addresses | `tern/address.h` | A node's identity and address, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/first-contact.md#addresses): an Ed25519 key pair from a seed, checking an address (prime-order subgroup included), and converting it to the X25519 key the handshake uses. |
| First contact | `tern/contact.h` | The EDHOC handshake that gives two nodes a unicast session, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/first-contact.md): both roles, four frames, contact tags, and erasure as the specification requires. About 3.6 KB of stack at its deepest. |
| Routes | `tern/route.h` | How a node learns which neighbour to hand a frame to, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/routing.md): announces, links judged by signal strength each way, loop-free route selection (Babel's feasibility condition), requests, Trickle and a cap on routing's share of the air. The simulator's candidate 3, as far as choosing routes; every parameter provisional. A node that restarts says so and waits before it routes through others, which narrows but does not close the one way a loop can form. A full table of neighbours keeps every link that is up, and otherwise the nearest. Not yet: broadcast and authentication. |
| Frames that follow routes | `tern/forward.h` | How a message is carried along those routes, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/forwarding.md): each frame names its destination and the neighbour it is for now, a node hears its neighbour pass a frame on and sends it again if it does not, then tries another route, and a message's source sends it again until its destination's acknowledgement comes back. Measured in the simulator beside Meshtastic and MeshCore (that draft's rationale). Not yet: the secured unicast frame under this head, so the Heltec port does not use it. |
| Unicast frames | `tern/unicast.h` | Secured unicast, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/unicast-security.md): sealing a message into a frame, and recognising, authenticating and opening frames for any of a node's sessions. Keys are erased as the specification requires. About 790 bytes of RAM per session. |
| SX1262 driver | `tern/sx126x.h` | The radio seam for Semtech's SX1262, written from its datasheet, errata included. Portable, so every SX1262 board shares it. |
| Crypto | `tern/crypto.h` | SHA-256, SHA-512, HMAC, HKDF-Expand, AES-128, AES-CCM and X25519, in portable constant-time C, each tested against its standard's published vectors. A reference to check hardware against, not a fast one. |

* [docs/architecture.md](docs/architecture.md) — the core, the ports, and the seam between them
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

| Board | Directory | |
|---|---|---|
| Heltec WiFi LoRa 32 V3 (ESP32-S3, SX1262) | [`ports/heltec-v3/`](ports/heltec-v3/) | A two-board bench demo: first contact, then unicast. CI builds an image you can flash from a browser. |

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE).
