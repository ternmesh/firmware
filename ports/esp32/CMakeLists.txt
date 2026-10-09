# ESP32 boards with an SX1262, built with ESP-IDF 5.5, one board at a time:
#
#   ./build.sh heltec-v4 && idf.py -B build-heltec-v4 flash monitor
#
# See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project is named for the board, tern-<board>, because the name is what a node checks an
# update over the companion link against (main.c, link_update_run()): it takes only an image whose
# name is its own. So a V4's image is refused by a V3, and the V3 keeps the name its first
# releases had, tern-heltec-v3, which every V3 in the field already expects. build.sh and
# release.sh say which board; a plain 'idf.py build' is the V3's.
if(NOT DEFINED TERN_BOARD)
    set(TERN_BOARD heltec-v3)
endif()
project(tern-${TERN_BOARD})
if(NOT "${CONFIG_TERN_BOARD_NAME}" STREQUAL "${TERN_BOARD}")
    message(FATAL_ERROR "this build's sdkconfig is for ${CONFIG_TERN_BOARD_NAME}, not "
        "${TERN_BOARD}: build with ./build.sh <board>")
endif()
