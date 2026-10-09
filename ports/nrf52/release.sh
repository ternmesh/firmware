#!/bin/sh
# Builds the images a release carries for the nRF52840 boards: one for each region, since a board
# must not send on another region's frequency before anyone has told it where it is.
#
#   ./release.sh 0.3.0
#
# Run it in a west workspace (west.yml), with the Zephyr SDK installed. It leaves in release/:
#
#   tern-heltec-t114-<region>-<version>.uf2    copied onto the board in its bootloader (README.md)
#
# and SHA256SUMS over them.
set -eu

version=${1:?usage: release.sh <version>}
cd "$(dirname "$0")"
rm -rf release
mkdir release

board=heltec-t114
target=heltec_t114_v2/nrf52840/uf2
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

cd release
if command -v sha256sum >/dev/null; then
    sha256sum -- *.uf2 >SHA256SUMS
else
    shasum -a 256 -- *.uf2 >SHA256SUMS
fi
cat SHA256SUMS
