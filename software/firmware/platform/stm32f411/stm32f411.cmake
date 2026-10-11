# SPDX-License-Identifier: GPL-3.0-or-later
# STM32F411CEU6 on WeAct Studio MiniSTM32F4x1 with a W25Q64 SPI flash. Configure with
#   cmake -B build/f411 -G Ninja -DPLATFORM=stm32f411
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/software/firmware/cmake/arm-cortex-m4f.cmake software/firmware
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32_common.cmake)
pdk_fetch(cmsis_device_f4 https://github.com/STMicroelectronics/cmsis-device-f4.git v2.6.9)

target_compile_definitions(pdk_core PUBLIC PDK_PLATFORM_STM32F411=1)

pdk_stm32_firmware(
    NAME pdkprog-f411
    DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32f411
    LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32f411/stm32f411ce.ld
    INCLUDES ${cmsis_device_f4_SOURCE_DIR}/Include
    DEFINES STM32F411xE CFG_TUSB_MCU=OPT_MCU_STM32F4
    USB_DRIVER ${TUSB}/portable/synopsys/dwc2/dcd_dwc2.c ${TUSB}/portable/synopsys/dwc2/dwc2_common.c)
