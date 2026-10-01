# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(usb-to-coco C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(usb-to-coco
        main.c
        hid_app.c
        bt_hid_app.c
        matrix.c
        keymap.c
        )

pico_set_program_name(usb-to-coco "usb-to-coco")
pico_set_program_version(usb-to-coco "0.1")

# Bring-up/test mode: GPIO0/1 (CoCo row1/row2) double as this board's UART
# TX/RX, so matrix and UART debug output can't both use them. ON leaves the
# matrix GPIOs untouched (UART works, decoded key events are logged instead
# of driving real pins) -- for validating USB/BT/keymap with no CoCo
# attached. Leave OFF for real use driving an actual CoCo.
option(DEBUG_KEY_LOG "Log decoded key events over UART instead of driving the CoCo matrix GPIOs" OFF)
if(DEBUG_KEY_LOG)
    target_compile_definitions(usb-to-coco PRIVATE DEBUG_KEY_LOG=1)
endif()

# stdio over UART for debug printf -- USB is host-only here (feeding the
# CoCo keyboard matrix), so it can't also carry a CDC stdio connection.
pico_enable_stdio_uart(usb-to-coco 1)
pico_enable_stdio_usb(usb-to-coco 0)

# Add the standard include files to the build
target_include_directories(usb-to-coco PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# pico_stdlib for GPIO/stdio, tinyusb_host for the USB host stack (native
# RP2350 controller), tinyusb_board for board_init()/board_api support.
# pico_cyw43_arch_none: cyw43 driver with no lwIP/networking (BT only, no
# WiFi needed). pico_btstack_cyw43: wires BTstack's HCI transport to the
# cyw43 driver. pico_btstack_classic: classic BR/EDR incl. HID Host, for
# older keyboards. pico_btstack_ble: LE GAP central + GATT client + HOGP
# host, for modern keyboards (incl. the 8BitDo Retro Mechanical Keyboard)
# that only pair over BLE.
target_link_libraries(usb-to-coco
        pico_stdlib
        tinyusb_host
        tinyusb_board
        pico_cyw43_arch_none
        pico_btstack_cyw43
        pico_btstack_classic
        pico_btstack_ble
        )

pico_add_extra_outputs(usb-to-coco)

