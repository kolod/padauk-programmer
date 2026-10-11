# SPDX-License-Identifier: GPL-3.0-or-later
# Toolchain file for Cortex-M targets (arm-none-eabi-gcc). Cortex-M0 by default; a wrapper
# toolchain file sets PDK_CPU_FLAGS first for another core (see arm-cortex-m4f.cmake).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR           arm-none-eabi-gcc-ar)       # LTO objects need the plugin-aware archiver
set(CMAKE_RANLIB       arm-none-eabi-gcc-ranlib)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

if(NOT DEFINED PDK_CPU_FLAGS)
    set(PDK_CPU_FLAGS "-mcpu=cortex-m0 -mthumb")
endif()
set(CMAKE_C_FLAGS_INIT   "${PDK_CPU_FLAGS} -Os -flto -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${PDK_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${PDK_CPU_FLAGS} -flto -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs")
