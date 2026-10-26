# Local sources in this bring-up + the shared pure-C calibration math from
# the main src tree. Pulling the math from src/drivers keeps it in sync
# with the [env:native] unit test (test/test_touch_cal_math/) — same file,
# two build targets.
#
# Path convention: ${CMAKE_SOURCE_DIR} is this bring-up's IDF project root
# (tools/bringup/08_touch_cal/), so `../../../<x>` reaches the repo root
# and `../../../src/...` reaches the main firmware src tree. Strings in
# idf_component_register's INCLUDE_DIRS would otherwise be resolved
# relative to this CMakeLists.txt file (i.e. .../08_touch_cal/src/), which
# is one level too deep — hence the explicit ${CMAKE_SOURCE_DIR} prefix.
FILE(GLOB_RECURSE local_sources ${CMAKE_SOURCE_DIR}/src/*.c)
set(shared_sources "${CMAKE_SOURCE_DIR}/../../../src/drivers/touch_cal_math.c")

idf_component_register(
    SRCS ${local_sources} ${shared_sources}
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/../../../src"
        "${CMAKE_SOURCE_DIR}/../../../include"
)
