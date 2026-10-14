# PSn00bSDK example CMake script
# (C) 2021 spicyjpeg - MPL licensed

cmake_minimum_required(VERSION 3.21)

project(
	sioplay
	LANGUAGES    C CXX ASM
	VERSION      1.0.0
	DESCRIPTION  "PS1 Pico video player"
	HOMEPAGE_URL "http://lameguy64.net/?page=psn00bsdk"
)

psn00bsdk_add_executable(sioplay GPREL main.c)

psn00bsdk_add_cd_image(
	iso      # Target name
	sioplay  # Output file name (= sioplay.bin + sioplay.cue)
	iso.xml  # Path to config file
	DEPENDS sioplay system.cnf
)
