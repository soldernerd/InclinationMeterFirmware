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

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
