cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME InclinationMeterFirmware)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# usbd_conf.h (hand-augmented, see its own USER CODE INCLUDE comment) pulls
# in Config/config.h, and hal_usb.h — needed by USB_Device_Library's and
# STM32_Drivers' own sources via that same header chain, not just this
# project's own App-layer target below. A CubeMX regen of
# cmake/stm32cubemx/CMakeLists.txt does not know about either directory and
# will not re-add them to MX_Include_Dirs, so this needs to live here
# instead, in the one CMakeLists.txt CubeMX never regenerates.
target_include_directories(stm32cubemx INTERFACE
    "${CMAKE_SOURCE_DIR}/Config"
    "${CMAKE_SOURCE_DIR}/HAL_App"
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/App/*.c"
    "${CMAKE_SOURCE_DIR}/Services/*.c"
    "${CMAKE_SOURCE_DIR}/Math/*.c"
    "${CMAKE_SOURCE_DIR}/Drivers_App/*.c"
    "${CMAKE_SOURCE_DIR}/HAL_App/*.c"
)

# Pull in only the u8g2 csrc files we use; skip mui_*, u8log_*, and dev/test files
file(GLOB U8G2_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Middleware/u8g2/csrc/u8g2_*.c"
    "${CMAKE_SOURCE_DIR}/Middleware/u8g2/csrc/u8x8_*.c"
)
list(FILTER U8G2_SOURCES EXCLUDE REGEX ".*/mui_.*\\.c$")
list(FILTER U8G2_SOURCES EXCLUDE REGEX ".*/u8log.*\\.c$")

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${APP_SOURCES}
    ${U8G2_SOURCES}
    "${CMAKE_SOURCE_DIR}/system_state.c"
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/App"
    "${CMAKE_SOURCE_DIR}/Services"
    "${CMAKE_SOURCE_DIR}/Math"
    "${CMAKE_SOURCE_DIR}/Drivers_App"
    "${CMAKE_SOURCE_DIR}/HAL_App"
    "${CMAKE_SOURCE_DIR}/Config"
    "${CMAKE_SOURCE_DIR}/Middleware/u8g2/csrc"
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Strict warnings for our own application code only (CubeMX-generated code is excluded)
set_source_files_properties(${APP_SOURCES} "${CMAKE_SOURCE_DIR}/system_state.c"
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# ADS131M04 acquisition hot path. The TIM7 trigger ISR (on_trigger) runs
# ~41667x/s and the SysTick drain (sign-extend + CRC + single-bin DFT MAC)
# ~1000x/s over ~21 frames. At the Debug default -O0 this pair is heavy
# enough to starve the cooperative scheduler (a display render balloons
# ~7x; see docs/adc_acquisition_redesign.md). Force -O2 on the whole hot
# path — the acquisition driver, the DFT, the per-frame CRC, and the
# low-level SPI-DMA / GPIO register pokes it calls — in every config;
# the rest of the firmware stays at the build type's -O for debuggability.
# (Overrides the COMPILE_OPTIONS set just above, so the warning flags are
# repeated here. Math/HAL_App files aren't in ${APP_SOURCES}'s warning set
# but -Werror-clean anyway.)
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/Services/svc_signal_analysis.c"
    "${CMAKE_SOURCE_DIR}/Drivers_App/drv_ads131m04.c"
    "${CMAKE_SOURCE_DIR}/Math/math_crc.c"
    "${CMAKE_SOURCE_DIR}/HAL_App/hal_spi.c"
    "${CMAKE_SOURCE_DIR}/HAL_App/hal_gpio.c"
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-O2")

# Display render path. task_display does a full 400x240 u8g2 render + frame
# parse in one blocking call; at -O0 that's ~120 ms, and it fires ~1/s
# (a 1 Hz value on screen changes), stalling the cooperative scheduler /
# API for that long. -O2 the graphics path (u8g2, the byte sink, the
# panel driver, the compositor) so the render is ~25 ms instead.
set_source_files_properties(
    ${U8G2_SOURCES}
    "${CMAKE_SOURCE_DIR}/App/app_display.c"
    "${CMAKE_SOURCE_DIR}/App/u8g2_hal_callback.c"
    "${CMAKE_SOURCE_DIR}/Drivers_App/drv_sharp_lcd.c"
    PROPERTIES COMPILE_OPTIONS "-O2")
# keep the project warning flags on our own files in that set
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/App/app_display.c"
    "${CMAKE_SOURCE_DIR}/App/u8g2_hal_callback.c"
    "${CMAKE_SOURCE_DIR}/Drivers_App/drv_sharp_lcd.c"
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-O2")

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
