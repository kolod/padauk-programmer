# SPDX-License-Identifier: GPL-3.0-or-later
# Toolchain file for Cortex-M4F targets (STM32F4): hard-float ABI with the single-precision FPU.
set(PDK_CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
include(${CMAKE_CURRENT_LIST_DIR}/arm-none-eabi.cmake)
