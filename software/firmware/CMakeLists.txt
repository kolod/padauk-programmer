# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

set(PLATFORM "host" CACHE STRING "Target platform: host | stm32f072")
set_property(CACHE PLATFORM PROPERTY STRINGS host stm32f072)

if(PLATFORM STREQUAL "host")
    project(padauk_programmer C)
else()
    project(padauk_programmer C ASM)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PDK_WARNINGS -Wall -Wextra -Werror -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Platform-independent core
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PDK_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/*.c)
add_library(pdk_core STATIC ${PDK_CORE_SOURCES})
target_include_directories(pdk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/hal)
target_compile_options(pdk_core PRIVATE ${PDK_WARNINGS})

# Optional features (may be disabled for the F072 bring-up target, see INFO.capabilities)
option(PDK_FEATURE_MEASURE_MASTER "COPY with MEASURE_MASTER" ON)
option(PDK_FEATURE_PATCH "PATCH operation" ON)
if(PDK_FEATURE_MEASURE_MASTER)
    target_compile_definitions(pdk_core PUBLIC PDK_FEATURE_MEASURE_MASTER=1)
endif()
if(PDK_FEATURE_PATCH)
    target_compile_definitions(pdk_core PUBLIC PDK_FEATURE_PATCH=1)
endif()

# ---------------------------------------------------------------------------
# Host: simulator + unit tests
# ---------------------------------------------------------------------------
if(PLATFORM STREQUAL "host")
    target_compile_definitions(pdk_core PUBLIC PDK_PLATFORM_HOST=1)

    file(GLOB PDK_HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/host/*.c)
    add_executable(pdk-sim ${PDK_HOST_SOURCES})
    target_link_libraries(pdk-sim PRIVATE pdk_core)
    target_compile_options(pdk-sim PRIVATE ${PDK_WARNINGS})
    if(WIN32)
        target_link_libraries(pdk-sim PRIVATE ws2_32 bcrypt)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(pdk-sim PRIVATE Threads::Threads)
    endif()

    enable_testing()
    add_subdirectory(tests)

# ---------------------------------------------------------------------------
# STM32F072 bring-up target (Easy PDK boards)
# ---------------------------------------------------------------------------
elseif(PLATFORM STREQUAL "stm32f072")
    include(${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32f072/stm32f072.cmake)
else()
    message(FATAL_ERROR "Unknown PLATFORM=${PLATFORM}")
endif()
