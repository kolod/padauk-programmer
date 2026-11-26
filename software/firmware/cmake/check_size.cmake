# SPDX-License-Identifier: GPL-3.0-or-later
# Post-build size check (T125): cmake -DSIZE=<arm-none-eabi-size> -DELF=<file> -DLIMIT=<bytes> -P check_size.cmake
# Fails when .text + .data (what goes into flash) exceeds LIMIT.
execute_process(COMMAND ${SIZE} --format=berkeley ${ELF} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "size failed for ${ELF}")
endif()
string(REGEX MATCH "\n[ \t]*([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)" row "${out}")
if(NOT row)
    message(FATAL_ERROR "cannot parse size output:\n${out}")
endif()
math(EXPR flash "${CMAKE_MATCH_1} + ${CMAKE_MATCH_2}")
math(EXPR pct "${flash} * 100 / ${LIMIT}")
if(flash GREATER LIMIT)
    message(FATAL_ERROR "${ELF}: .text + .data = ${flash} B exceeds the ${LIMIT} B limit "
                        "(turn PDK_FEATURE_MEASURE_MASTER / PDK_FEATURE_PATCH off)")
endif()
message(STATUS "${ELF}: .text + .data = ${flash} B of ${LIMIT} B (${pct} %)")
