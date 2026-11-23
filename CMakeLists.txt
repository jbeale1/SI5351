cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2040_zero)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(blink_ws2812 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(blink_ws2812 main.c)
pico_generate_pio_header(blink_ws2812 ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)
target_link_libraries(blink_ws2812 pico_stdlib hardware_pio hardware_clocks hardware_i2c)
pico_add_extra_outputs(blink_ws2812)
