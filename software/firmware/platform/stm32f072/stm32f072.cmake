# SPDX-License-Identifier: GPL-3.0-or-later
# STM32F072 bring-up target (Easy PDK programmer boards). Configure with
#   cmake -B build/f072 -G Ninja -DPLATFORM=stm32f072 -DCMAKE_TOOLCHAIN_FILE=software/firmware/cmake/arm-none-eabi.cmake software/firmware
include(FetchContent)

# Pinned third-party sources (fetched at configure time, never vendored).
FetchContent_Declare(cmsis_core
    GIT_REPOSITORY https://github.com/STMicroelectronics/cmsis_core.git
    GIT_TAG v5.9.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(cmsis_device_f0
    GIT_REPOSITORY https://github.com/STMicroelectronics/cmsis-device-f0.git
    GIT_TAG v2.3.7
    GIT_SHALLOW TRUE)
FetchContent_Declare(tinyusb
    GIT_REPOSITORY https://github.com/hathach/tinyusb.git
    GIT_TAG 0.18.0
    GIT_SHALLOW TRUE)
foreach(dep cmsis_core cmsis_device_f0 tinyusb)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})   # sources only: none of them is added as a CMake project
    endif()
endforeach()

set(F072_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32f072)
set(TUSB ${tinyusb_SOURCE_DIR}/src)

add_library(f072_deps INTERFACE)
target_include_directories(f072_deps INTERFACE
    ${F072_DIR}
    ${cmsis_core_SOURCE_DIR}/Include
    ${cmsis_device_f0_SOURCE_DIR}/Include
    ${TUSB})
target_compile_definitions(f072_deps INTERFACE STM32F072xB CFG_TUSB_MCU=OPT_MCU_STM32F0)

add_library(tinyusb STATIC
    ${TUSB}/tusb.c
    ${TUSB}/common/tusb_fifo.c
    ${TUSB}/device/usbd.c
    ${TUSB}/device/usbd_control.c
    ${TUSB}/class/cdc/cdc_device.c
    ${TUSB}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c)
target_link_libraries(tinyusb PUBLIC f072_deps)
set_target_properties(tinyusb PROPERTIES C_EXTENSIONS ON)   # uses GNU `asm`

target_compile_definitions(pdk_core PUBLIC PDK_PLATFORM_STM32F072=1)

file(GLOB F072_SOURCES CONFIGURE_DEPENDS ${F072_DIR}/*.c)
add_executable(pdkprog.elf ${F072_SOURCES})
target_link_libraries(pdkprog.elf PRIVATE pdk_core tinyusb f072_deps)
target_compile_options(pdkprog.elf PRIVATE ${PDK_WARNINGS})
target_link_options(pdkprog.elf PRIVATE
    -T${F072_DIR}/stm32f072c8.ld
    -nostartfiles   # own Reset_Handler in startup.c; crt0 would pull exit() and stdio
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/pdkprog.map
    -Wl,--print-memory-usage)
set_target_properties(pdkprog.elf PROPERTIES LINK_DEPENDS ${F072_DIR}/stm32f072c8.ld)

add_custom_command(TARGET pdkprog.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary pdkprog.elf pdkprog.bin
    COMMAND ${CMAKE_SIZE} pdkprog.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "pdkprog.bin")
