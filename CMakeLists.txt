cmake_minimum_required(VERSION 3.20)
project(tern VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(TERN_WERROR "Treat warnings as errors" OFF)
option(TERN_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(TERN_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
    -Wstrict-prototypes -Wmissing-prototypes)
if(TERN_WERROR)
    list(APPEND TERN_WARNINGS -Werror)
endif()

if(TERN_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# The protocol core. It is portable C17 that needs no heap, no operating system and nothing from
# libc beyond the freestanding headers, so the same library builds for a microcontroller
# (cmake/arm-none-eabi.cmake) and for a host. Board ports and the Linux build link against it.
add_library(tern
    src/address.c
    src/aes.c
    src/ccm.c
    src/companion.c
    src/contact.c
    src/duty.c
    src/fe25519.c
    src/forward.c
    src/hkdf.c
    src/listen.c
    src/lora.c
    src/radio.c
    src/region.c
    src/route.c
    src/sha256.c
    src/sha512.c
    src/sx126x.c
    src/unicast.c
    src/x25519.c
)
target_include_directories(tern PUBLIC include)
target_compile_options(tern PRIVATE ${TERN_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    foreach(t address companion contact crypto duty forward listen lora mesh radio region route sx126x unicast)
        add_executable(test_${t} tests/${t}.c)
        target_link_libraries(test_${t} PRIVATE tern)
        # The test files define their cases as static functions, so the prototype warning is
        # the library's rule, not theirs.
        target_compile_options(test_${t} PRIVATE ${TERN_WARNINGS} -Wno-missing-prototypes)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    # The unicast tests read the specification's vectors through a header generated from its JSON
    # (tests/vectors/), so the numbers they check are the specification's, copied, not retyped.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(vectors_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/unicast_security.h)
    add_custom_command(
        OUTPUT ${vectors_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/unicast_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/unicast-security.json ${vectors_h}
        DEPENDS tests/vectors/unicast_to_c.py tests/vectors/unicast-security.json
        VERBATIM)
    target_sources(test_unicast PRIVATE ${vectors_h})

    set(contact_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/first_contact.h)
    add_custom_command(
        OUTPUT ${contact_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/contact_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/first-contact.json ${contact_h}
        DEPENDS tests/vectors/contact_to_c.py tests/vectors/first-contact.json
        VERBATIM)
    target_sources(test_contact PRIVATE ${contact_h})
    target_include_directories(test_contact PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)

    set(phy_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/phy.h)
    add_custom_command(
        OUTPUT ${phy_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/phy_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/phy.json ${phy_h}
        DEPENDS tests/vectors/phy_to_c.py tests/vectors/phy.json
        VERBATIM)
    target_sources(test_region PRIVATE ${phy_h})
    target_include_directories(test_region PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)

    set(routing_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/routing.h)
    add_custom_command(
        OUTPUT ${routing_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/routing_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/routing.json ${routing_h}
        DEPENDS tests/vectors/routing_to_c.py tests/vectors/routing.json
        VERBATIM)
    target_sources(test_route PRIVATE ${routing_h})
    target_include_directories(test_route PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)

    set(forwarding_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/forwarding.h)
    add_custom_command(
        OUTPUT ${forwarding_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/forwarding_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/forwarding.json ${forwarding_h}
        DEPENDS tests/vectors/forwarding_to_c.py tests/vectors/forwarding.json
        VERBATIM)
    target_sources(test_forward PRIVATE ${forwarding_h})
    target_include_directories(test_forward PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)

    set(listening_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/listening.h)
    add_custom_command(
        OUTPUT ${listening_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/forwarding_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/forwarding.json ${listening_h} listens
        DEPENDS tests/vectors/forwarding_to_c.py tests/vectors/forwarding.json
        VERBATIM)
    target_sources(test_listen PRIVATE ${listening_h})
    target_include_directories(test_listen PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)

    set(companion_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/companion.h)
    add_custom_command(
        OUTPUT ${companion_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/companion_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/companion.json ${companion_h}
        DEPENDS tests/vectors/companion_to_c.py tests/vectors/companion.json
        VERBATIM)
    target_sources(test_companion PRIVATE ${companion_h})
    target_include_directories(test_companion PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)

    set(exchange_h ${CMAKE_CURRENT_BINARY_DIR}/vectors/companion_exchange.h)
    add_custom_command(
        OUTPUT ${exchange_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/vectors
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/companion_to_c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/companion.json ${exchange_h} exchange
        DEPENDS tests/vectors/companion_to_c.py tests/vectors/companion.json
        VERBATIM)

    # The Heltec V3 port's node logic touches no hardware, so it is tested here too.
    add_executable(test_demo tests/demo.c ports/heltec-v3/main/demo.c)
    target_include_directories(test_demo PRIVATE ports/heltec-v3/main)
    target_link_libraries(test_demo PRIVATE tern)
    target_compile_options(test_demo PRIVATE ${TERN_WARNINGS} -Wno-missing-prototypes)
    add_test(NAME demo COMMAND test_demo)
    add_executable(test_link tests/link.c ports/heltec-v3/main/link.c ${exchange_h})
    target_include_directories(test_link PRIVATE ports/heltec-v3/main
        ${CMAKE_CURRENT_BINARY_DIR}/vectors)
    target_link_libraries(test_link PRIVATE tern)
    target_compile_options(test_link PRIVATE ${TERN_WARNINGS} -Wno-missing-prototypes)
    add_test(NAME link COMMAND test_link)
    # The laptop script (tools/companion.py), against the vectors and against the link on a
    # pseudo-terminal: everything but the radio and the USB cable.
    add_test(NAME companion_script COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/companion.py selftest
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/companion.json)
    if(UNIX)
        add_executable(test_link_pty tests/link_pty.c ports/heltec-v3/main/link.c)
        target_include_directories(test_link_pty PRIVATE ports/heltec-v3/main)
        target_link_libraries(test_link_pty PRIVATE tern)
        target_compile_options(test_link_pty PRIVATE ${TERN_WARNINGS} -Wno-missing-prototypes)
        add_test(NAME link_script COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/link_script.py $<TARGET_FILE:test_link_pty>
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/companion.py)
    endif()
    add_executable(test_status tests/status.c ports/heltec-v3/main/status.c
        ports/heltec-v3/main/display.c)
    target_include_directories(test_status PRIVATE ports/heltec-v3/main)
    target_compile_options(test_status PRIVATE ${TERN_WARNINGS} -Wno-missing-prototypes)
    add_test(NAME status COMMAND test_status)
    target_include_directories(test_unicast PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)
endif()
