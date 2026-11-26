# SPDX-License-Identifier: GPL-3.0-or-later
# Every test_*.c is a separate executable linked against the core and the fake HAL.
#  - test_engine_*.c test the real chip engine (pdk_engine) over the fake IC lines (fakes/engine/);
#  - every other test runs the pipelines on the fake engine (fakes/fake_chip.c).
file(GLOB PDK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
file(GLOB PDK_FAKE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fakes/*.c)
list(FILTER PDK_FAKE_SOURCES EXCLUDE REGEX "fake_chip\.c$")
file(GLOB PDK_FAKE_ICP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fakes/engine/*.c)

add_library(pdk_fakes STATIC ${PDK_FAKE_SOURCES})
target_link_libraries(pdk_fakes PUBLIC pdk_core)
target_include_directories(pdk_fakes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
target_compile_options(pdk_fakes PRIVATE ${PDK_WARNINGS})

add_library(pdk_fake_chip STATIC ${CMAKE_CURRENT_SOURCE_DIR}/fakes/fake_chip.c)
target_link_libraries(pdk_fake_chip PUBLIC pdk_fakes)
target_compile_options(pdk_fake_chip PRIVATE ${PDK_WARNINGS})

add_library(pdk_fake_icp STATIC ${PDK_FAKE_ICP_SOURCES})
target_link_libraries(pdk_fake_icp PUBLIC pdk_fakes pdk_engine)
target_include_directories(pdk_fake_icp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fakes/engine ${CMAKE_CURRENT_SOURCE_DIR}/../core/engine)
target_compile_options(pdk_fake_icp PRIVATE ${PDK_WARNINGS})

foreach(src ${PDK_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_compile_definitions(${name} PRIVATE PDK_TEST_MAIN=1)
    # core, engine and fakes reference each other: list them twice to resolve every direction
    if(name MATCHES "^test_engine_")
        target_link_libraries(${name} PRIVATE pdk_fake_icp pdk_engine pdk_fakes pdk_core
                                              pdk_fake_icp pdk_engine pdk_fakes pdk_core)
        target_compile_definitions(${name} PRIVATE
            PDK_TRACE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vectors/traces")
    else()
        target_link_libraries(${name} PRIVATE pdk_fake_chip pdk_fakes pdk_core pdk_fake_chip pdk_fakes pdk_core)
    endif()
    target_compile_options(${name} PRIVATE ${PDK_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endforeach()
