set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

# CMAKE_SYSTEM_* must be set on every toolchain load. Flags and find_* must not.
if(HALL_ARM_TOOLCHAIN_CONFIGURED)
    return()
endif()
set(HALL_ARM_TOOLCHAIN_CONFIGURED TRUE)

if(CMAKE_HOST_WIN32)
    set(_HALL_HOST_EXE ".exe")
else()
    set(_HALL_HOST_EXE "")
endif()

# Ninja is often bundled with CLion and is not on PATH.
if(NOT CMAKE_MAKE_PROGRAM)
    set(_HALL_NINJA_DIRS "")
    if(CMAKE_HOST_WIN32)
        file(GLOB _HALL_NINJA_DIRS
            "C:/Program Files/JetBrains/CLion */bin/ninja/win/x64"
            "C:/Program Files (x86)/JetBrains/CLion */bin/ninja/win/x64"
            "D:/Program Files/JetBrains/CLion */bin/ninja/win/x64"
            "D:/JetBrains/CLion */bin/ninja/win/x64"
            "$ENV{LOCALAPPDATA}/Programs/CLion/bin/ninja/win/x64"
            "D:/ClionProject/YFOCV3_LITE/tools/ninja"
        )
        if(_HALL_NINJA_DIRS)
            list(SORT _HALL_NINJA_DIRS COMPARE NATURAL ORDER DESCENDING)
        endif()
    endif()

    find_program(CMAKE_MAKE_PROGRAM
        NAMES ninja ninja.exe
        PATHS ${_HALL_NINJA_DIRS}
        DOC "Ninja build tool"
    )
endif()

set(_HALL_ARM_HINTS "")
if(ARM_NONE_EABI_TOOLCHAIN_PATH)
    list(APPEND _HALL_ARM_HINTS "${ARM_NONE_EABI_TOOLCHAIN_PATH}")
endif()
if(DEFINED ENV{ARM_NONE_EABI_TOOLCHAIN_PATH})
    list(APPEND _HALL_ARM_HINTS "$ENV{ARM_NONE_EABI_TOOLCHAIN_PATH}")
endif()

if(CMAKE_HOST_WIN32)
    file(GLOB _HALL_ARM_GLOBS
        "D:/GD32EB*/GD32EB*/GD32EB/plugins/com.gd.tools_*/Tools/ARMEmbeddedGCC/bin"
        "D:/GD32EB_v1.5.11_Rel/GD32EB_v1.5.11_Rel/GD32EB/plugins/com.gd.tools_*/Tools/ARMEmbeddedGCC/bin"
        "C:/GD32EB*/GD32EB*/GD32EB/plugins/com.gd.tools_*/Tools/ARMEmbeddedGCC/bin"
        "D:/GD32EB*/GD32EB*/GD32EB/plugins/com.gd.tools.stdarmgcc*/Tools/StdARMGCC*/bin"
        "D:/GD32EB*/GD32EB*/GD32EB/plugins/com.gd.tools.gdarmgcc*/Tools/GdARMGCC*/bin"
        "D:/ClionProject/YFOCV3_LITE/tools/arm-gnu/bin"
        "D:/ClionProject/YFOCV3_LITE/tools/arm-gnu/*/bin"
        "C:/ST/STM32CubeIDE*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin"
        "D:/ST/STM32CubeIDE*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin"
        "C:/Program Files/Arm GNU Toolchain arm-none-eabi/*/bin"
        "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/*/bin"
        "C:/Program Files (x86)/GNU Arm Embedded Toolchain/*/bin"
    )
    if(_HALL_ARM_GLOBS)
        list(SORT _HALL_ARM_GLOBS COMPARE NATURAL ORDER DESCENDING)
        list(APPEND _HALL_ARM_HINTS ${_HALL_ARM_GLOBS})
    endif()
endif()

# Prefer the GD32EB toolchain used by this project, then any other ARM GCC.
set(_HALL_GD32_GCC_HINTS "")
if(CMAKE_HOST_WIN32)
    file(GLOB _HALL_GD32_GCC_HINTS
        "D:/GD32EB_v1.5.11_Rel/GD32EB_v1.5.11_Rel/GD32EB/plugins/com.gd.tools_*/Tools/ARMEmbeddedGCC/bin"
        "D:/GD32EB*/GD32EB*/GD32EB/plugins/com.gd.tools_*/Tools/ARMEmbeddedGCC/bin"
        "C:/GD32EB*/GD32EB*/GD32EB/plugins/com.gd.tools_*/Tools/ARMEmbeddedGCC/bin"
    )
endif()
find_program(HALL_ARM_GCC
    NAMES arm-none-eabi-gcc arm-none-eabi-gcc.exe
    PATHS ${_HALL_GD32_GCC_HINTS}
    NO_DEFAULT_PATH
    DOC "ARM GCC cross compiler"
)
if(NOT HALL_ARM_GCC)
    find_program(HALL_ARM_GCC
        NAMES arm-none-eabi-gcc arm-none-eabi-gcc.exe
        PATHS ${_HALL_ARM_HINTS}
        DOC "ARM GCC cross compiler"
    )
endif()

if(NOT HALL_ARM_GCC)
    message(FATAL_ERROR
        "arm-none-eabi-gcc not found.\n"
        "Install GD32EB, or set ARM_NONE_EABI_TOOLCHAIN_PATH to the toolchain bin directory."
    )
endif()

get_filename_component(HALL_ARM_TOOLCHAIN_DIR "${HALL_ARM_GCC}" DIRECTORY)

find_program(HALL_ARM_GXX
    NAMES arm-none-eabi-g++ arm-none-eabi-g++.exe
    HINTS "${HALL_ARM_TOOLCHAIN_DIR}"
    NO_DEFAULT_PATH
)
find_program(HALL_ARM_OBJCOPY
    NAMES arm-none-eabi-objcopy arm-none-eabi-objcopy.exe
    HINTS "${HALL_ARM_TOOLCHAIN_DIR}"
    NO_DEFAULT_PATH
)
find_program(HALL_ARM_SIZE
    NAMES arm-none-eabi-size arm-none-eabi-size.exe
    HINTS "${HALL_ARM_TOOLCHAIN_DIR}"
    NO_DEFAULT_PATH
)

if(NOT HALL_ARM_GXX)
    set(HALL_ARM_GXX "${HALL_ARM_TOOLCHAIN_DIR}/arm-none-eabi-g++${_HALL_HOST_EXE}")
endif()
if(NOT HALL_ARM_OBJCOPY)
    set(HALL_ARM_OBJCOPY "${HALL_ARM_TOOLCHAIN_DIR}/arm-none-eabi-objcopy${_HALL_HOST_EXE}")
endif()
if(NOT HALL_ARM_SIZE)
    set(HALL_ARM_SIZE "${HALL_ARM_TOOLCHAIN_DIR}/arm-none-eabi-size${_HALL_HOST_EXE}")
endif()

set(CMAKE_C_COMPILER   "${HALL_ARM_GCC}"     CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_ASM_COMPILER "${HALL_ARM_GCC}"     CACHE FILEPATH "ASM compiler" FORCE)
set(CMAKE_CXX_COMPILER "${HALL_ARM_GXX}"     CACHE FILEPATH "CXX compiler" FORCE)
set(CMAKE_LINKER       "${HALL_ARM_GXX}"     CACHE FILEPATH "Linker" FORCE)
set(CMAKE_OBJCOPY      "${HALL_ARM_OBJCOPY}" CACHE FILEPATH "objcopy" FORCE)
set(CMAKE_SIZE         "${HALL_ARM_SIZE}"    CACHE FILEPATH "size" FORCE)

message(STATUS "ARM GCC: ${HALL_ARM_GCC}")
if(CMAKE_MAKE_PROGRAM)
    message(STATUS "Ninja:   ${CMAKE_MAKE_PROGRAM}")
endif()

# GD32E230G6: Cortex-M23, no FPU. Match Port/gd32e23x/startup_gd32e23x.S (.fpu softvfp).
set(TARGET_FLAGS "-mcpu=cortex-m23 -mthumb -mfloat-abi=soft")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections")

set(CMAKE_C_FLAGS_DEBUG          "-O0 -g3"       CACHE STRING "Debug C flags" FORCE)
set(CMAKE_C_FLAGS_RELEASE        "-Os -g"        CACHE STRING "Release C flags" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-Os -g"        CACHE STRING "RelWithDebInfo C flags" FORCE)
set(CMAKE_C_FLAGS_MINSIZEREL     "-Os -DNDEBUG"  CACHE STRING "MinSizeRel C flags" FORCE)

set(CMAKE_CXX_FLAGS_DEBUG          "${CMAKE_C_FLAGS_DEBUG}"          CACHE STRING "Debug CXX flags" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE        "${CMAKE_C_FLAGS_RELEASE}"        CACHE STRING "Release CXX flags" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}" CACHE STRING "RelWithDebInfo CXX flags" FORCE)
set(CMAKE_CXX_FLAGS_MINSIZEREL     "${CMAKE_C_FLAGS_MINSIZEREL}"     CACHE STRING "MinSizeRel CXX flags" FORCE)

set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}")

set(CMAKE_C_LINK_FLAGS "${TARGET_FLAGS}")
set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} --specs=nano.specs")
set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} -Wl,--gc-sections")
set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} -Wl,--print-memory-usage")
set(CMAKE_CXX_LINK_FLAGS "${CMAKE_C_LINK_FLAGS}")
