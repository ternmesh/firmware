#!/usr/bin/env python3
"""Sends an nRF52840 board's bootloader new firmware over Bluetooth: Nordic's legacy DFU.

    python3 tools/ble_dfu.py tern-heltec-t114-us915-<version>.zip
    python3 tools/ble_dfu.py --address <the bootloader's address> <package>

A Tern node on such a board has room for one image, not two, so it takes an update the way its
bootloader does (draft/companion.md, UPDATE_BOOTLOADER): a client asks it to restart into its
bootloader, and the bootloader, which keeps Nordic's SoftDevice for itself, advertises the legacy
DFU service and takes the image over Bluetooth. `tools/companion.py update <package>` asks, and then
runs this; run alone, it finds a bootloader that is already advertising.

The package is the release's `.zip` for the board and region, as `adafruit-nrfutil dfu genpkg`
makes it (ports/nrf52/release.sh): an init packet, `.dat`, and the application, `.bin`.

The exchange, on the service's control point (written with a response, answering by notification)
and its packet characteristic (written without one):

    control  START_DFU, application          01 04
    packet   sizes: SoftDevice, bootloader, application, u32 each
             <- 10 01 01                      once the bootloader has erased the application
    control  INIT 02 00, packet the init packet, control INIT 02 01
             <- 10 02 01
    control  receipts every n packets        08 n n
    control  RECEIVE                          03
    packet   the image                        <- 11 and the bytes received, every n packets
             <- 10 03 01                      after the last
    control  VALIDATE                         04  <- 10 04 01: its CRC is the init packet's
    control  ACTIVATE_AND_RESET               05  the bootloader starts it

Everything the node saved is kept: the bootloader writes only the application's flash.

Needs bleak (`pip install bleak`).
"""

import argparse
import asyncio
import json
import struct
import sys
import time
import zipfile

SERVICE = "00001530-1212-efde-1523-785feabcd123"
CONTROL = "00001531-1212-efde-1523-785feabcd123"
PACKET = "00001532-1212-efde-1523-785feabcd123"

START, INIT, RECEIVE, VALIDATE, ACTIVATE, RECEIPTS = 0x01, 0x02, 0x03, 0x04, 0x05, 0x08
RESPONSE, RECEIPT = 0x10, 0x11
APPLICATION = 0x04
STATUS = {1: "success", 2: "invalid state", 3: "not supported", 4: "data size exceeds limit",
          5: "CRC error", 6: "operation failed"}
# A packet of the image: the least every link carries. The bootloader takes whole words only.
PACKET_BYTES = 20
# Packets between receipts: Nordic's own library holds legacy bootloaders to ten.
EVERY = 10
# A gap between packets of the image. A bootloader that is sent them faster than it writes them
# to flash stops with "operation failed", as the T1000-E's did with none; a phone's stack sends
# about one each connection event, which this is close to.
PACE_S = 0.01
# START waits for the bootloader to erase the application's flash, page by page between the
# SoftDevice's radio events: tens of seconds, and on an older bootloader minutes. Given up on too
# soon, the bootloader is left mid-session, and answers the next START "invalid state".
ERASE_S = 240
FIND_S = 30


def package(path):
    """The init packet and the application of a DFU package."""
    with zipfile.ZipFile(path) as z:
        manifest = json.loads(z.read("manifest.json"))["manifest"]
        app = manifest.get("application")
        if app is None:
            raise SystemExit(f"{path}: no application in it")
        return z.read(app["dat_file"]), z.read(app["bin_file"])


async def find(address, wait):
    from bleak import BleakScanner  # noqa: PLC0415

    if address:
        device = await BleakScanner.find_device_by_address(address, timeout=wait)
    else:
        device = await BleakScanner.find_device_by_filter(
            lambda d, adv: SERVICE in [u.lower() for u in adv.service_uuids], timeout=wait)
    if device is None:
        raise SystemExit("no bootloader advertising the DFU service was found")
    return device


async def send(device, init, image, log=print):
    from bleak import BleakClient  # noqa: PLC0415

    answers = asyncio.Queue()

    def notified(_, data):
        answers.put_nowait(bytes(data))

    async def answer(op, seconds):
        while True:
            a = await asyncio.wait_for(answers.get(), seconds)
            if a[0] == RESPONSE and a[1] == op:
                if a[2] != 1:
                    raise SystemExit(f"the bootloader refused step {op}: {STATUS.get(a[2], a[2])}")
                return
            # A receipt here is a late one from the image; anything else is not ours.

    async with BleakClient(device) as client:
        log(f"connected to {device.name or device.address}")
        await client.start_notify(CONTROL, notified)
        await client.write_gatt_char(CONTROL, bytes([START, APPLICATION]), response=True)
        await client.write_gatt_char(PACKET, struct.pack("<III", 0, 0, len(image)), response=False)
        log("erasing the old firmware...")
        erasing = time.monotonic()
        await answer(START, ERASE_S)
        log(f"erased in {time.monotonic() - erasing:.0f} s")
        await client.write_gatt_char(CONTROL, bytes([INIT, 0x00]), response=True)
        for i in range(0, len(init), PACKET_BYTES):
            await client.write_gatt_char(PACKET, init[i:i + PACKET_BYTES], response=False)
        await client.write_gatt_char(CONTROL, bytes([INIT, 0x01]), response=True)
        await answer(INIT, 10)
        await client.write_gatt_char(CONTROL, bytes([RECEIPTS, EVERY, 0]), response=True)
        await client.write_gatt_char(CONTROL, bytes([RECEIVE]), response=True)

        started, shown, packets = time.monotonic(), -1, 0
        for at in range(0, len(image), PACKET_BYTES):
            await client.write_gatt_char(PACKET, image[at:at + PACKET_BYTES], response=False)
            await asyncio.sleep(PACE_S)
            packets += 1
            if packets % EVERY == 0 and at + PACKET_BYTES < len(image):
                while True:  # the receipt is the flow control: no more until it comes
                    a = await asyncio.wait_for(answers.get(), 10)
                    if a[0] == RECEIPT:
                        break
                    if a[0] == RESPONSE and a[2] != 1:
                        raise SystemExit(f"the bootloader stopped: {STATUS.get(a[2], a[2])}")
            percent = min(at + PACKET_BYTES, len(image)) * 100 // len(image)
            if percent != shown:
                log(f"\r{percent}%", end="", flush=True)
                shown = percent
        log("")
        await answer(RECEIVE, 30)
        await client.write_gatt_char(CONTROL, bytes([VALIDATE]), response=True)
        await answer(VALIDATE, 10)
        log(f"sent {len(image)} bytes in {time.monotonic() - started:.0f} s; checked")
        try:
            await client.write_gatt_char(CONTROL, bytes([ACTIVATE]), response=True)
        except Exception:  # noqa: BLE001 - it resets as it answers, and the link may drop first
            pass
        log("the bootloader starts the new firmware")


def update(path, address=None, wait=FIND_S, log=print):
    init, image = package(path)
    log(f"{path}: {len(image)} bytes of firmware")

    async def go():
        device = await find(address, wait)
        await send(device, init, image, log)

    asyncio.run(go())


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("package", help="the release's .zip for the board and region")
    p.add_argument("--address", help="the bootloader's Bluetooth address (or, on a Mac, its UUID)")
    p.add_argument("--wait", type=float, default=FIND_S, help="seconds to look for it")
    a = p.parse_args()
    update(a.package, a.address, a.wait)


if __name__ == "__main__":
    sys.exit(main())
