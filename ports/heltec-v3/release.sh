#!/bin/sh
# Builds the images a release carries: one for each region, since a board must not send on
# another region's frequency before anyone has told it where it is.
#
#   ./release.sh 0.1.0-alpha.1
#
# Run it in an ESP-IDF 5.5 shell. It leaves in release/, for each region:
#
#   tern-heltec-v3-<region>-<version>.bin         the whole flash, written at 0x0: a new node
#   tern-heltec-v3-<region>-<version>-boot.bin    the bootloader and the partition table, written
#                                                 at 0x0, ending where NVS begins (0x9000)
#   tern-heltec-v3-<region>-<version>-update.bin  otadata, blank, and the firmware in the first
#                                                 slot, written at 0xF000, just past NVS
#   tern-heltec-v3-<region>-<version>-app.bin     the firmware alone: what a phone sends over the
#                                                 companion link to update a node
#
# -boot.bin and -update.bin, written together over USB, update a board and leave NVS, and with it
# its identity, sessions, contacts and bonds, alone. They are the whole image cut either side of
# NVS (partitions.csv), so they are the same bytes as the whole flash, wherever they fall.
#
# and SHA256SUMS over them all.
set -eu

version=${1:?usage: release.sh <version>}
cd "$(dirname "$0")"
rm -rf release
mkdir release

for region in us915 eu868; do
    build=build-$region
    upper=$(echo "$region" | tr a-z A-Z)
    rm -rf "$build"
    mkdir "$build"
    {
        echo "CONFIG_TERN_VERSION=\"$version\""
        echo "CONFIG_TERN_REGION_$upper=y"
    } >"$build/release.defaults"
    idf.py -B "$build" -D SDKCONFIG="$build/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$build/release.defaults" build
    name=tern-heltec-v3-$region-$version
    idf.py -B "$build" merge-bin -o "$name.bin"
    cp "$build/$name.bin" "release/$name.bin"
    head -c $((0x9000)) "$build/$name.bin" >"release/$name-boot.bin"
    tail -c +$((0xF000 + 1)) "$build/$name.bin" >"release/$name-update.bin"
    cp "$build/tern-heltec-v3.bin" "release/$name-app.bin"
done

cd release
if command -v sha256sum >/dev/null; then
    sha256sum -- *.bin >SHA256SUMS
else
    shasum -a 256 -- *.bin >SHA256SUMS
fi
cat SHA256SUMS
