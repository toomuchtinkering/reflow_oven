cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL's assert handler is redirected to ours (src/diag/lv_assert_trap.h) via
# CONFIG_LV_ASSERT_HANDLER_INCLUDE. lv_assert.h is compiled inside the lvgl
# managed component, which has no dependency on the app component, so src/ has
# to be on the *global* include path for that #include to resolve. Set before
# project() so it reaches every component.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/src" APPEND)
project(reflow_oven)

# Version embedding (FIRMWARE_VERSION_STR) is handled by the pre-build hook
# tools/pio_gen_version.py, which writes <build_dir>/version.h from git describe
# on every build. Doing it here via configure_file only ran at configure time
# (went stale without `touch CMakeLists.txt`) and leaked across envs — see that
# script's header and platformio.ini's `-I .pio/build/${PIOENV}`.
