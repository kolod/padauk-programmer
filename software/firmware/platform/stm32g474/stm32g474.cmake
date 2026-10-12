# SPDX-License-Identifier: GPL-3.0-or-later
# STM32G474CEU6 on the WeAct Studio STM32G474 core board. Configure with
#   cmake -B build/g474 -G Ninja -DPLATFORM=stm32g474
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/software/firmware/cmake/arm-cortex-m4f.cmake software/firmware
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32_common.cmake)
pdk_fetch(cmsis_device_g4 https://github.com/STMicroelectronics/cmsis-device-g4.git v1.2.6)

# The VDD/VPP front end (DAC → regulators, ADC on dividers) is not built yet: keep it off until the
# board exists, otherwise floating ADC pins could pass the supply checks.
option(PDK_G474_SUPPLIES "Drive VDD/VPP through DAC1 and measure them with ADC1" OFF)

target_compile_definitions(pdk_core PUBLIC PDK_PLATFORM_STM32G474=1)

pdk_stm32_firmware(
    NAME pdkprog-g474
    DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32g474
    LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32g474/stm32g474ce.ld
    INCLUDES ${cmsis_device_g4_SOURCE_DIR}/Include
    DEFINES STM32G474xx CFG_TUSB_MCU=OPT_MCU_STM32G4 $<$<BOOL:${PDK_G474_SUPPLIES}>:PDK_G474_SUPPLIES=1>
    USB_DRIVER ${TUSB}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c)
