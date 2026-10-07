# The port's own code, and the portable core from the top of the repository, compiled as one
# component. The core is listed file by file, as the top-level CMakeLists.txt lists it.
set(root ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
    SRCS
        main.c
        board.c
        demo.c
        ${root}/src/address.c
        ${root}/src/aes.c
        ${root}/src/ccm.c
        ${root}/src/contact.c
        ${root}/src/duty.c
        ${root}/src/fe25519.c
        ${root}/src/hkdf.c
        ${root}/src/lora.c
        ${root}/src/radio.c
        ${root}/src/region.c
        ${root}/src/route.c
        ${root}/src/sha256.c
        ${root}/src/sha512.c
        ${root}/src/sx126x.c
        ${root}/src/unicast.c
        ${root}/src/x25519.c
    INCLUDE_DIRS . ${root}/include
    PRIV_REQUIRES bootloader_support esp_driver_gpio esp_hw_support esp_driver_spi esp_driver_uart esp_timer nvs_flash)
