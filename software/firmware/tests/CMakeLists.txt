# SPDX-License-Identifier: GPL-3.0-or-later
# Every test_*.c is a separate executable linked against the core and the fake HAL.
file(GLOB PDK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
file(GLOB PDK_FAKE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fakes/*.c)

add_library(pdk_fakes STATIC ${PDK_FAKE_SOURCES})
target_link_libraries(pdk_fakes PUBLIC pdk_core)
target_include_directories(pdk_fakes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
target_compile_options(pdk_fakes PRIVATE ${PDK_WARNINGS})

foreach(src ${PDK_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_compile_definitions(${name} PRIVATE PDK_TEST_MAIN=1)
    # core and fakes reference each other: link the core twice to resolve both directions
    target_link_libraries(${name} PRIVATE pdk_fakes pdk_core pdk_fakes)
    target_compile_options(${name} PRIVATE ${PDK_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endforeach()
