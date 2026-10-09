# The nRF52840 port, built with Zephyr: the node every port shares (../node), the portable core
# from the top of the repository, and what is Zephyr's here (src/). See README.md:
#
#   west build -b heltec_t114_v2/nrf52840/uf2 ports/nrf52
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tern-nrf52 C)

set(root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(node ${root}/ports/node)

# The core, file by file, as the top-level CMakeLists.txt lists it.
file(GLOB core ${root}/src/*.c)

target_sources(app PRIVATE
    src/platform.c
    src/board.c
    src/ble.c
    ${node}/node.c
    ${node}/link.c
    ${node}/demo.c
    ${node}/display.c
    ${node}/status.c
    ${node}/ui.c
    ${node}/qr.c
    ${node}/power.c
    ${core})
target_include_directories(app PRIVATE src ${node} ${root}/include)
# Warnings as the rest of the repository has them, on what is ours (not Zephyr's headers' doing).
target_compile_options(app PRIVATE -Wextra -Wshadow -Wno-unused-parameter -Werror)
