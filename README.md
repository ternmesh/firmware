# Tern firmware

The reference implementation of Tern: a portable C protocol core that also builds on Linux (for infrastructure nodes on single-board computers, and for host tests), plus ports for each supported board.

Part of **Tern**, a LoRa mesh protocol that treats airtime as a shared,
metered resource. The protocol is defined by the specification in
[ternmesh/spec](https://github.com/ternmesh/spec), not by this code.

**Status:** early. The core implements the one part of the protocol the specification has
drafted, secured unicast frames, and passes every one of its test vectors. Nothing drives it over
a radio yet: there are no board ports, and no routing.

| Module | Header | What it is |
|---|---|---|
| Time on air | `tern/lora.h` | LoRa time on air (Semtech's formula, SF7–SF12), in integer nanoseconds, matching the simulator's to the nanosecond. The airtime budget and the routing metric are both measured in it. |
| Radio seam | `tern/radio.h` | The interface each board port implements for its radio, and the checks the core makes before a port is called. Events are polled, never delivered in interrupt context. |
| Unicast frames | `tern/unicast.h` | Secured unicast, [specification draft 0](https://github.com/ternmesh/spec/blob/main/draft/unicast-security.md): sealing a message into a frame, and recognising, authenticating and opening frames for any of a node's sessions. Keys are erased as the specification requires. About 790 bytes of RAM per session. |
| Crypto | `tern/crypto.h` | SHA-256, HMAC, HKDF-Expand, AES-128 and AES-CCM, in portable constant-time C, each tested against its standard's published vectors. A reference to check hardware AES against, not a fast one. |

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

`-DTERN_SANITIZE=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer, and
`-DTERN_WERROR=ON` makes warnings errors; CI runs both, with GCC and Clang. Format with
`clang-format` 18 before pushing.

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE).
