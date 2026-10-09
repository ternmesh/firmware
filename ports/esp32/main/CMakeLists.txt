# The port's own code, the node every port shares (ports/node), and the portable core from the top
# of the repository, compiled as one component. The core is listed file by file, as the top-level
# CMakeLists.txt lists it.
set(root ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(node ${root}/ports/node)
idf_component_register(
    SRCS
        platform.c
        ble.c
        board.c
        boards.c
        ${node}/node.c
        ${node}/link.c
        ${node}/demo.c
        ${node}/display.c
        ${node}/status.c
        ${node}/ui.c
        ${node}/qr.c
        ${node}/power.c
        ${root}/src/address.c
        ${root}/src/aes.c
        ${root}/src/card.c
        ${root}/src/ccm.c
        ${root}/src/companion.c
        ${root}/src/contact.c
        ${root}/src/duty.c
        ${root}/src/fe25519.c
        ${root}/src/flood.c
        ${root}/src/forward.c
        ${root}/src/group.c
        ${root}/src/listen.c
        ${root}/src/hkdf.c
        ${root}/src/lora.c
        ${root}/src/position.c
        ${root}/src/radio.c
        ${root}/src/region.c
        ${root}/src/route.c
        ${root}/src/sha256.c
        ${root}/src/sha512.c
        ${root}/src/share.c
        ${root}/src/sx126x.c
        ${root}/src/unicast.c
        ${root}/src/x25519.c
    INCLUDE_DIRS . ${node} ${root}/include
    PRIV_REQUIRES bootloader_support bt esp_adc esp_driver_gpio esp_driver_i2c esp_hw_support esp_driver_spi esp_driver_uart esp_driver_usb_serial_jtag esp_timer app_update nvs_flash)
