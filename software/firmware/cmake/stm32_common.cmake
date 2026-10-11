# SPDX-License-Identifier: GPL-3.0-or-later
# Shared pieces of the STM32 platforms: pinned third-party sources (fetched at configure time,
# never vendored) and pdk_stm32_firmware() that builds <name>.elf/.bin from a platform directory
# plus platform/common.
include(FetchContent)

# pdk_fetch(<name> <git url> <tag>): populates sources only, nothing is added as a CMake project.
function(pdk_fetch name url tag)
    FetchContent_Declare(${name} GIT_REPOSITORY ${url} GIT_TAG ${tag} GIT_SHALLOW TRUE)
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
        FetchContent_Populate(${name})
    endif()
    set(${name}_SOURCE_DIR ${${name}_SOURCE_DIR} PARENT_SCOPE)
endfunction()

pdk_fetch(cmsis_core https://github.com/STMicroelectronics/cmsis_core.git v5.9.0)
pdk_fetch(tinyusb https://github.com/hathach/tinyusb.git 0.18.0)
set(TUSB ${tinyusb_SOURCE_DIR}/src)
set(PDK_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/common)

# pdk_stm32_firmware(NAME <elf name> DIR <platform dir> LDSCRIPT <file>
#                    INCLUDES <dirs...> DEFINES <defs...> USB_DRIVER <tinyusb sources...>)
function(pdk_stm32_firmware)
    cmake_parse_arguments(A "" "NAME;DIR;LDSCRIPT" "INCLUDES;DEFINES;USB_DRIVER" ${ARGN})

    add_library(stm32_deps INTERFACE)
    target_include_directories(stm32_deps INTERFACE ${A_DIR} ${PDK_COMMON_DIR} ${cmsis_core_SOURCE_DIR}/Include
                               ${A_INCLUDES} ${TUSB})
    target_compile_definitions(stm32_deps INTERFACE ${A_DEFINES})

    add_library(tinyusb STATIC
        ${TUSB}/tusb.c
        ${TUSB}/common/tusb_fifo.c
        ${TUSB}/device/usbd.c
        ${TUSB}/device/usbd_control.c
        ${TUSB}/class/cdc/cdc_device.c
        ${A_USB_DRIVER})
    target_link_libraries(tinyusb PUBLIC stm32_deps)
    set_target_properties(tinyusb PROPERTIES C_EXTENSIONS ON)   # uses GNU `asm`

    file(GLOB sources CONFIGURE_DEPENDS ${A_DIR}/*.c ${PDK_COMMON_DIR}/*.c)
    add_executable(${A_NAME}.elf ${sources})
    target_link_libraries(${A_NAME}.elf PRIVATE pdk_core tinyusb stm32_deps)
    target_compile_options(${A_NAME}.elf PRIVATE ${PDK_WARNINGS})
    target_link_options(${A_NAME}.elf PRIVATE
        -T${A_LDSCRIPT}
        -nostartfiles   # own Reset_Handler in startup.c; crt0 would pull exit() and stdio
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${A_NAME}.map
        -Wl,--print-memory-usage)
    set_target_properties(${A_NAME}.elf PROPERTIES LINK_DEPENDS ${A_LDSCRIPT})

    add_custom_command(TARGET ${A_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary ${A_NAME}.elf ${A_NAME}.bin
        COMMAND ${CMAKE_SIZE} ${A_NAME}.elf
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "${A_NAME}.bin")
endfunction()
