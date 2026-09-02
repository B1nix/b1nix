# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.13)

# Include Pico SDK
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico-sdk/pico_sdk_init.cmake")
    include(pico-sdk/pico_sdk_init.cmake)
elseif (DEFINED ENV{PICO_SDK_PATH})
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
endif()

project(b1nix_rp2350_uart_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (NOT PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Target board (pico2, waveshare_rp2350_zero, pico, etc.)")
endif()

SET(BOARD_DEFINITION ${CMAKE_CURRENT_SOURCE_DIR}/boards/${PICO_BOARD}.cmake)
if (NOT EXISTS ${BOARD_DEFINITION})
    SET(BOARD_DEFINITION ${CMAKE_CURRENT_SOURCE_DIR}/boards/unknown.cmake)
endif()

include(${BOARD_DEFINITION} OPTIONAL)

pico_sdk_init()

add_executable(b1nix_uart_bridge
    uart-bridge.c
    usb-descriptors.c
    led.c
)

target_include_directories(b1nix_uart_bridge PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(b1nix_uart_bridge
    hardware_flash
    hardware_gpio
    hardware_uart
    hardware_pwm
    pico_status_led
    pico_multicore
    pico_stdlib
    pico_time
    tinyusb_device
)

pico_add_extra_outputs(b1nix_uart_bridge)
