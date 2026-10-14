cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(../pico/pico_sdk_import.cmake)

project(sdtest C CXX ASM)
pico_sdk_init()

# same SD library and wiring as the main firmware
set(FATFS_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/../pico/lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    CACHE PATH "Path to no-OS-FatFS-SD-SDIO-SPI-RPi-Pico")
add_subdirectory(${FATFS_LIB_DIR}/src fatfs_build)

add_executable(sdtest
    main.c
    ../pico/hw_config.c
)

target_link_libraries(sdtest
    pico_stdlib
    hardware_spi
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

pico_enable_stdio_usb(sdtest 1)
pico_enable_stdio_uart(sdtest 0)
pico_add_extra_outputs(sdtest)
