#!/bin/sh
# Builds the images a release carries for the nRF52840 boards: for each board, one for each region,
# since a board must not send on another region's frequency before anyone has told it where it is.
#
#   ./release.sh 0.3.0                  every board below
#   ./release.sh 0.3.0 wio-tracker-l1   only those named
#
# Run it in a west workspace (west.yml), with the Zephyr SDK installed. It leaves in release/:
#
#   tern-<board>-<region>-<version>.uf2    copied onto the board in its bootloader (README.md)
#
# and SHA256SUMS over them.
set -eu

# Each board's name, and the Zephyr board it is built for: its devicetree, with this port's overlay
# on it in boards/, is everything about it (src/board.c).
targets="heltec-t114:heltec_t114_v2/nrf52840/uf2 rak4631:rak4631/nrf52840 wio-tracker-l1:wio_tracker_l1/nrf52840"

version=${1:?usage: release.sh <version> [board...]}
shift
cd "$(dirname "$0")"
if [ $# -eq 0 ]; then
    set -- $(for t in $targets; do echo "${t%%:*}"; done)
fi
rm -rf release
mkdir release

for board in "$@"; do
    target=
    for t in $targets; do
        if [ "${t%%:*}" = "$board" ]; then
            target=${t#*:}
        fi
    done
    test -n "$target" || { echo "no board $board: see release.sh" >&2; exit 1; }
    for region in us915 eu868; do
        build=build-$board-$region
        upper=$(echo "$region" | tr a-z A-Z)
        rm -rf "$build"
        mkdir "$build"
        {
            echo "CONFIG_TERN_VERSION=\"$version\""
            echo "CONFIG_TERN_REGION_$upper=y"
        } >"$build/release.conf"
        west build -p -b "$target" -d "$build" . -- -DEXTRA_CONF_FILE="$PWD/$build/release.conf"
        cp "$build/zephyr/zephyr.uf2" "release/tern-$board-$region-$version.uf2"
    done
done

cd release
if command -v sha256sum >/dev/null; then
    sha256sum -- *.uf2 >SHA256SUMS
else
    shasum -a 256 -- *.uf2 >SHA256SUMS
fi
cat SHA256SUMS
