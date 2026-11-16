#!/bin/sh
# Puts an image on a board over USB, with no button, by serial DFU through the bootloader it ships
# with (Adafruit's): the 1200 bps touch restarts the board into its bootloader, from Tern or from
# the firmware it came with, and the image goes over the same serial port. For a bootloader open
# without its drive, as MeshCore's firmware opens the T1000-E's; otherwise a .uf2 is copied on.
#
#   ./flash.sh t1000-e build/zephyr/zephyr.hex [port]
#
# With adafruit-nrfutil installed (pip install adafruit-nrfutil). The port is the board's USB serial
# port, found if there is only one. A board running firmware that does not answer the touch is put
# in its bootloader by hand first (README.md).
set -eu

board=${1:?usage: flash.sh <board> <zephyr.hex> [port]}
hex=${2:?usage: flash.sh <board> <zephyr.hex> [port]}
port=${3:-}

# The SoftDevice each board's bootloader sits on, as the package must name it: S140 7.3.0 on the
# T1000-E (Seeed's Arduino core, boards.txt, sd_fwid).
case $board in
t1000-e) sd_req=0x0123 ;;
*)
    echo "no SoftDevice known for $board: see flash.sh" >&2
    exit 1
    ;;
esac

if [ -z "$port" ]; then
    set -- /dev/cu.usbmodem* /dev/ttyACM*
    ports=
    for p in "$@"; do
        test -e "$p" && ports="$ports $p"
    done
    set -- $ports
    if [ $# -ne 1 ]; then
        echo "found ${ports:- no ports}: name the board's port" >&2
        exit 1
    fi
    port=$1
fi

pkg=$(mktemp -d)/tern.zip
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req "$sd_req" --application "$hex" "$pkg"
adafruit-nrfutil dfu serial --package "$pkg" -p "$port" -b 115200 --singlebank --touch 1200
rm -rf "$(dirname "$pkg")"
