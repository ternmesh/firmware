cmake_minimum_required(VERSION 3.20)
project(tern VERSION 0.0.0 LANGUAGES C)

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
    src/aes.c
    src/ccm.c
    src/hkdf.c
    src/lora.c
    src/radio.c
    src/sha256.c
    src/unicast.c
)
target_include_directories(tern PUBLIC include)
target_compile_options(tern PRIVATE ${TERN_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    foreach(t crypto lora radio unicast)
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
    target_include_directories(test_unicast PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)
endif()
