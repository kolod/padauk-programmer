# SPDX-License-Identifier: GPL-3.0-or-later
# STM32F072 bring-up target (Easy PDK programmer boards). Configure with
#   cmake -B build/f072 -G Ninja -DPLATFORM=stm32f072
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/software/firmware/cmake/arm-none-eabi.cmake software/firmware
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32_common.cmake)
pdk_fetch(cmsis_device_f0 https://github.com/STMicroelectronics/cmsis-device-f0.git v2.3.7)

target_compile_definitions(pdk_core PUBLIC PDK_PLATFORM_STM32F072=1)

pdk_stm32_firmware(
    NAME pdkprog
    DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32f072
    LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32f072/stm32f072c8.ld
    INCLUDES ${cmsis_device_f0_SOURCE_DIR}/Include
    DEFINES STM32F072xB CFG_TUSB_MCU=OPT_MCU_STM32F0
    USB_DRIVER ${TUSB}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c)
