# Local sources in this bring-up + the shared driver and OTA validator sources
# from the main src tree.
#
# Path convention: ${CMAKE_SOURCE_DIR} is this bring-up's IDF project root
# (tools/bringup/13_self_tests/), so `../../../<x>` reaches the repo root
# and `../../../src/...` reaches the main firmware src tree.
FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${CMAKE_SOURCE_DIR}/../../../src/drivers/display.c
    ${CMAKE_SOURCE_DIR}/../../../src/drivers/touch.c
    ${CMAKE_SOURCE_DIR}/../../../src/drivers/touch_cal_math.c
    ${CMAKE_SOURCE_DIR}/../../../src/drivers/thermocouple.c
    ${CMAKE_SOURCE_DIR}/../../../src/drivers/ssr.c
    ${CMAKE_SOURCE_DIR}/../../../src/drivers/ssr_scheduler.c
    ${CMAKE_SOURCE_DIR}/../../../src/ota/ota_validator.c
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/../../../src"
        "${CMAKE_SOURCE_DIR}/../../../include"
)
