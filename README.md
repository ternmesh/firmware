# Tern firmware

The reference implementation of Tern: a portable C protocol core that also builds on Linux (for infrastructure nodes on single-board computers, and for host tests), plus ports for each supported board.

Part of **Tern**, a LoRa mesh protocol that treats airtime as a shared,
metered resource. The protocol is defined by the specification in
[ternmesh/spec](https://github.com/ternmesh/spec), not by this code.

**Status:** the skeleton. Nothing here implements the protocol yet, because the specification
does not define any of it yet. What exists is the part every later piece builds on:

| Module | Header | What it is |
|---|---|---|
| Time on air | `tern/lora.h` | LoRa time on air (Semtech's formula, SF7–SF12), in integer nanoseconds, matching the simulator's to the nanosecond. The airtime budget and the routing metric are both measured in it. |
| Radio seam | `tern/radio.h` | The interface each board port implements for its radio, and the checks the core makes before a port is called. Events are polled, never delivered in interrupt context. |

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
