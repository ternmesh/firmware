# Architecture

How the firmware is laid out, and the rules that keep it portable. What the protocol *does* is
the specification's business ([ternmesh/spec](https://github.com/ternmesh/spec)); this is only
about the code.

## Layers of code

```
 the node               ports/node/      the loop, the companion link, the screen: every port's
 ──────────────────────────────────────────────────────────────────────────────────────────
 the platform seam      ports/node/platform.h, board.h, ble.h, implemented by each port
 ──────────────────────────────────────────────────────────────────────────────────────────
 ports                  ports/<chip>/    startup, storage, Bluetooth, each board's pins
 ──────────────────────────────────────────────────────────────────────────────────────────
 the seam               include/tern/radio.h   struct tern_radio_ops, implemented by each port
 ──────────────────────────────────────────────────────────────────────────────────────────
 protocol core          src/, include/tern/   portable C17, the same on every target
```

**The core** is everything the specification defines: framing, crypto, the airtime budget,
routing. It is one library, `libtern`, and it is the same code on a microcontroller, on a
single-board computer and in the host tests.

**The node** is what a board runs on top of the core: the loop that drives the radio, the
companion link a phone or computer uses, the screen's pages, the console. It is the same on every
chip, in `ports/node/`, and has no hardware code: the parts of it that need none are tested on a
host. It asks its platform for what it cannot do itself through three headers: `platform.h`
(storage, random numbers, firmware updates, restarting), `board.h` (the radio's bus, the button,
the screen, the battery) and `ble.h` (the companion link over Bluetooth).

**A port** is everything specific to one family of chips: how it boots, where it keeps what it
saves, its Bluetooth stack, how it talks to its radio over SPI, and each board's pins. Ports live
under `ports/`, each built with its platform's own tools (`ports/esp32/` with ESP-IDF), and
implement the node's three headers. A port uses a driver from the core for its radio
(`tern/sx126x.h`, given the board's SPI bus), or implements the radio operations in
`tern/radio.h` itself. Which boards each port runs is [boards.md](boards.md).

**The seam** between them is deliberately narrow. The core asks a port to configure the radio,
transmit a frame, listen, stand by, and report what happened. Anything it can check about those
requests, it checks itself (`src/radio.c`), so every port gets the same validation for free.

## Rules for the core

These are what let one library run everywhere. CI enforces the first three.

1. **Portable C17, warnings as errors.** It builds with GCC and Clang on Linux and macOS, and with
   `arm-none-eabi-gcc` for a Cortex-M4F (`cmake/arm-none-eabi.cmake`).
2. **Freestanding.** Nothing from libc beyond the freestanding headers (`<stdint.h>`,
   `<stdbool.h>`, `<stddef.h>` and the like), and so no `malloc`. CI fails if the
   microcontroller build calls anything outside itself and the compiler's runtime.
3. **Integer arithmetic for anything that crosses the air.** Time on air, budgets and metrics
   must give the same answer on a chip without an FPU as on a host, and the same answer as the
   simulator ([ternmesh/sim](https://github.com/ternmesh/sim)). Time is `tern_time`: signed
   64-bit nanoseconds, as in the simulator.
4. **No interrupt context.** A port's interrupt handler records what happened and returns; the
   core collects it with `tern_radio_poll()` from its own loop. That keeps the core free of locks,
   and lets the host tests drive it with no interrupts at all.
5. **No global state.** State lives in structs the caller owns, so a test, or a host process
   simulating several nodes, can run more than one instance side by side.

## Tests

Each `tests/<name>.c` is one executable and one CTest entry, using the small harness in
`tests/check.h`. Tests talk to the core the way a port would: `tests/radio.c` is a fake port that
records what it was asked to do.

When a section of the specification lands with test vectors in `ternmesh/spec/vectors/`, the core
is tested against those vectors, not against values written here.
