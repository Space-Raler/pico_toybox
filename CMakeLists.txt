cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)
set(PICO_PLATFORM rp2350)

# The SDK location comes from the PICO_SDK_PATH environment variable.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_toybox C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(pico_toybox
    src/main.c
    src/log.c
    src/store.c
    src/persist.c
    src/ctl.c
    src/plat_pico.c
    src/ble_link.c
    src/usb_modes.c
    src/mode_inf.c
    src/mode_sky.c
    src/mode_ld.c
    src/ld_pad.c

    # reused from the Flipper project, unchanged (vendor/)
    vendor/inf/infinity_core.c
    vendor/inf/inf_crypto.c
    vendor/inf/infinity_figures.c
    vendor/inf/ble_proto.c
    vendor/sky/core/sky_core.c
    vendor/sky/sky_ble.c
    vendor/ld/ld_ble.c
    vendor/ld/tea.c
    vendor/ld/burtle.c
    vendor/ld/bytes.c
    vendor/ld/minifigures.c
    vendor/ld/debug.c
    vendor/ld/usb/frame.c
)

target_include_directories(pico_toybox PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}          # tusb_config.h, btstack_config.h
    ${CMAKE_CURRENT_LIST_DIR}/shim     # tiny <furi.h> for tea.c / minifigures.c
    ${CMAKE_CURRENT_LIST_DIR}/vendor/ld/usb
)

# Generates toybox.h (the GATT database) from toybox.gatt
pico_btstack_make_gatt_header(pico_toybox PRIVATE ${CMAKE_CURRENT_LIST_DIR}/toybox.gatt)

target_link_libraries(pico_toybox PRIVATE
    pico_stdlib
    pico_unique_id
    pico_flash
    hardware_flash
    hardware_watchdog
    tinyusb_device
    pico_cyw43_arch_none
    pico_btstack_ble
    pico_btstack_cyw43
)

# USB belongs to the console connection, so no USB serial: the log goes over Bluetooth.
pico_enable_stdio_usb(pico_toybox 0)
pico_enable_stdio_uart(pico_toybox 0)

target_compile_options(pico_toybox PRIVATE -Wall -Wextra -Wno-unused-parameter)

pico_add_extra_outputs(pico_toybox)
