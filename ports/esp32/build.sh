#!/bin/sh
# Builds the firmware for one board, in build-<board>/:
#
#   ./build.sh heltec-v4
#   idf.py -B build-heltec-v4 -p /dev/ttyACM0 flash monitor
#
# Run it in an ESP-IDF 5.5 shell. The boards are the files in boards/: each is the settings that
# board needs over sdkconfig.defaults. A plain 'idf.py build' builds for the Heltec V3.
set -eu

board=${1:?usage: build.sh <board>, one of: $(cd "$(dirname "$0")/boards" && ls | sed 's/\.defaults$//' | tr '\n' ' ')}
cd "$(dirname "$0")"
if [ ! -f "boards/$board.defaults" ]; then
    echo "no board $board: see boards/" >&2
    exit 1
fi
shift
# The chip is the board's (boards/<board>.defaults), whatever the shell had set.
target=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' "boards/$board.defaults")
export IDF_TARGET="${target:-esp32s3}"
idf.py -B "build-$board" -D TERN_BOARD="$board" -D SDKCONFIG="build-$board/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$board.defaults" build "$@"
