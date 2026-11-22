cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake"
        CACHE FILEPATH "ARM GCC toolchain" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
    set(CMAKE_COLOR_DIAGNOSTICS ON)
endif()

set(CMAKE_PROJECT_NAME Hall_Senser)

project(${CMAKE_PROJECT_NAME} C ASM)

add_executable(${CMAKE_PROJECT_NAME})

set(GD32_CMSIS "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS")
set(GD32_SPL   "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/GD32E23x_standard_peripheral")

file(GLOB HALL_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/App/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Hall/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Port/gd32e23x/*.c"
)
file(GLOB HALL_SPL_SOURCES CONFIGURE_DEPENDS
    "${GD32_SPL}/Source/*.c"
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Port/gd32e23x/startup_gd32e23x.S
    ${GD32_CMSIS}/GD/GD32E23x/Source/system_gd32e23x.c
    ${GD32_CMSIS}/GD/GD32E23x/Source/syscalls.c
    ${HALL_SPL_SOURCES}
    ${HALL_APP_SOURCES}
    Hw/RTT/SEGGER_RTT.c
    Hw/RTT/SEGGER_RTT_printf.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App
    Hall
    Port/gd32e23x
    Hw/RTT
    ${GD32_CMSIS}
    ${GD32_CMSIS}/GD/GD32E23x/Include
    ${GD32_SPL}/Include
)

# Same macros as the GD32EB project (.cproject, GD32E230G6U6TR).
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    GD32E230
    USE_STDPERIPH_DRIVER
)

set_source_files_properties(Port/gd32e23x/startup_gd32e23x.S PROPERTIES LANGUAGE ASM)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Port/gd32e23x/gd32e23x_flash.ld" HALL_LINKER_SCRIPT)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T "${HALL_LINKER_SCRIPT}"
    "LINKER:-Map=$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.map"
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    LINK_DEPENDS "${HALL_LINKER_SCRIPT}"
)

if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "CMAKE_OBJCOPY is not set (arm-none-eabi-objcopy not found)")
endif()
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary
            "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin"
    COMMAND "${CMAKE_OBJCOPY}" -O ihex
            "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.hex"
    COMMAND "${CMAKE_SIZE}" --format=berkeley "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
    COMMENT "Generating bin/hex"
    VERBATIM
)

# cmake --build --preset debug --target flash
add_custom_target(flash
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/../tools/flash.ps1"
            -ElfFile "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
    DEPENDS ${CMAKE_PROJECT_NAME}
    USES_TERMINAL
    COMMENT "Flash ${CMAKE_PROJECT_NAME} with OpenOCD"
    VERBATIM
)
