# libopenmpt (third_party/libopenmpt, vendored release sources, BSD-3-Clause) as a static library `openmpt`.
#
# Mirrors the source lists and options of libopenmpt's own Makefile (LIBOPENMPT_CXX_SOURCES) for a build without
# external libraries: zlib, mpg123 and libvorbis are replaced by the bundled miniz, minimp3 and stb_vorbis, so
# compressed formats (MDZ, MO3 with MP3/Vorbis samples, ...) still load. Update: see third_party/libopenmpt/VENDORED.md.

set(OPENMPT_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/libopenmpt)

file(GLOB OPENMPT_CXX_SOURCES CONFIGURE_DEPENDS
    ${OPENMPT_DIR}/src/openmpt/all/*.cpp
    ${OPENMPT_DIR}/src/openmpt/base/*.cpp
    ${OPENMPT_DIR}/src/openmpt/logging/*.cpp
    ${OPENMPT_DIR}/src/openmpt/random/*.cpp
    ${OPENMPT_DIR}/common/*.cpp
    ${OPENMPT_DIR}/src/openmpt/fileformat_base/*.cpp
    ${OPENMPT_DIR}/src/openmpt/soundbase/*.cpp
    ${OPENMPT_DIR}/src/openmpt/soundfile_data/*.cpp
    ${OPENMPT_DIR}/soundlib/*.cpp
    ${OPENMPT_DIR}/soundlib/plugins/*.cpp
    ${OPENMPT_DIR}/soundlib/plugins/dmo/*.cpp
    ${OPENMPT_DIR}/sounddsp/*.cpp
    ${OPENMPT_DIR}/libopenmpt/*.cpp)
set(OPENMPT_C_SOURCES
    ${OPENMPT_DIR}/include/miniz/miniz.c
    ${OPENMPT_DIR}/include/minimp3/minimp3.c
    ${OPENMPT_DIR}/include/stb_vorbis/stb_vorbis.c)

add_library(openmpt STATIC ${OPENMPT_CXX_SOURCES} ${OPENMPT_C_SOURCES})
target_include_directories(openmpt PRIVATE ${OPENMPT_DIR} ${OPENMPT_DIR}/src ${OPENMPT_DIR}/common ${OPENMPT_DIR}/include)
# Users include <libopenmpt/libopenmpt.hpp> (and _ext.hpp for the interactive interface).
target_include_directories(openmpt SYSTEM INTERFACE ${OPENMPT_DIR})
target_compile_definitions(openmpt PRIVATE
    LIBOPENMPT_BUILD
    MPT_WITH_MINIZ MPT_WITH_MINIMP3
    MPT_WITH_STBVORBIS STB_VORBIS_NO_PULLDATA_API STB_VORBIS_NO_STDIO)
# Third-party code: no warnings of ours; optimised like the app.
target_compile_options(openmpt PRIVATE -w $<$<CONFIG:Release>:-O2>)
if(CMAKE_CROSSCOMPILING)
    target_compile_options(openmpt PRIVATE -ffunction-sections -fdata-sections)
endif()
target_link_libraries(openmpt PUBLIC Threads::Threads)
