# ESP32 boards with an SX1262, built with ESP-IDF 5.5, one board at a time:
#
#   ./build.sh heltec-v4 && idf.py -B build-heltec-v4 flash monitor
#
# See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tern)
