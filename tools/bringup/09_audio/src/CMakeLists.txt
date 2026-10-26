# Local main.c + the shared audio driver (kept in sync with the main firmware
# build, same file/two targets).
FILE(GLOB_RECURSE local_sources ${CMAKE_SOURCE_DIR}/src/*.c)
set(shared_sources
    "${CMAKE_SOURCE_DIR}/../../../src/drivers/audio.c"
    "${CMAKE_SOURCE_DIR}/../../../src/audio/audio_melodies.c"
)

idf_component_register(
    SRCS ${local_sources} ${shared_sources}
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/../../../src"
        "${CMAKE_SOURCE_DIR}/../../../include"
)
