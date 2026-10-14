cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")   # use pico2_w for the Pico 2 W

# Pico SDK: set PICO_SDK_PATH in your environment (the VS Code Pico extension does this)
include(pico_sdk_import.cmake)

project(ps1_pico_video C CXX ASM)
pico_sdk_init()

# SD card + FatFs library:
#   git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
set(FATFS_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    CACHE PATH "Path to no-OS-FatFS-SD-SDIO-SPI-RPi-Pico")
add_subdirectory(${FATFS_LIB_DIR}/src fatfs_build)

add_executable(ps1_pico_video
    main.c
    hw_config.c
)

pico_generate_pio_header(ps1_pico_video ${CMAKE_CURRENT_LIST_DIR}/sio0_dev.pio)

target_link_libraries(ps1_pico_video
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# Status messages over USB (open the Pico's serial port at any baud rate)
pico_enable_stdio_usb(ps1_pico_video 1)
pico_enable_stdio_uart(ps1_pico_video 0)

pico_add_extra_outputs(ps1_pico_video)   # produces ps1_pico_video.uf2
