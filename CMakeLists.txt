cmake_minimum_required(VERSION 3.25)

# ---- Your app's identity: change these four lines ------------------------------------------------------------------
# NAME: the executable, the boot menu's app id, the data folder on the stick (<NAME>-data) and the settings prefix
# (<NAME>_<KEY> in upper case, '-' → '_'). DISPLAY_NAME (≤ 47 bytes), ID (≤ 39) and VERSION (≤ 23) are what the boot
# menu shows, read from the binary.
set(APP_NAME "track16")
set(APP_DISPLAY_NAME "Track16")
set(APP_ID "io.github.oldwired.track16")
set(APP_VERSION "0.3.0")

project(${APP_NAME} VERSION ${APP_VERSION} LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

# The unit (armel, toolchain/cross.cmake) or a Linux desktop.
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    set(PLATFORM imx6)
    set(GL_ARCH arm)
else()
    set(PLATFORM linux)
    set(GL_ARCH x86_64)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_package(X11 REQUIRED)

# The identity for the code (identity::name, …).
string(TOUPPER "${APP_NAME}" APP_ENV_PREFIX)
string(REPLACE "-" "_" APP_ENV_PREFIX "${APP_ENV_PREFIX}")
set(GENERATED ${CMAKE_BINARY_DIR}/generated)
file(CONFIGURE OUTPUT ${GENERATED}/ProjectIdentity.h CONTENT [[
#pragma once
namespace identity
{
inline constexpr const char* name = "@APP_NAME@";
inline constexpr const char* displayName = "@APP_DISPLAY_NAME@";
inline constexpr const char* id = "@APP_ID@";
inline constexpr const char* version = "@APP_VERSION@";
inline constexpr const char* envPrefix = "@APP_ENV_PREFIX@"; // settings: <PREFIX>_<KEY>
}
]] @ONLY)

# ---- JUCE 9 (git submodule third_party/JUCE): only the modules a component UI needs, no windowing code is used ----
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt)
    message(FATAL_ERROR "JUCE is missing: git submodule update --init --depth 1")
endif()
add_subdirectory(third_party/JUCE ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
add_library(juce STATIC)
target_link_libraries(juce
    PRIVATE juce::juce_core juce::juce_events juce::juce_data_structures juce::juce_graphics juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags)
target_compile_definitions(juce PUBLIC
    JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_USE_XSHM=0 JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
    JUCE_USE_XRENDER=0)
target_include_directories(juce SYSTEM INTERFACE $<TARGET_PROPERTY:juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(juce INTERFACE $<TARGET_PROPERTY:juce,COMPILE_DEFINITIONS>)
target_compile_options(juce PRIVATE $<$<CONFIG:Release>:-O3>)

# The embedded fonts (the unit has no font files).
juce_add_binary_data(app_fonts HEADER_NAME AppFonts.h NAMESPACE AppFonts SOURCES
    resources/fonts/Inter-Regular.subset.ttf resources/fonts/Inter-Medium.subset.ttf resources/fonts/Inter-SemiBold.subset.ttf
    resources/fonts/JetBrainsMonoNL-Medium.subset.ttf resources/fonts/JetBrainsMonoNL-Bold.subset.ttf)

# ---- libopenmpt (third_party/libopenmpt): module playback -----------------------------------------------------------
include(cmake/libopenmpt.cmake)

# ---- The code: the hardware layer (hw/) and the app (app/), one library -------------------------------------------
file(GLOB HW_SOURCES CONFIGURE_DEPENDS hw/controls/*.cpp hw/common/*.cpp hw/${PLATFORM}/*.cpp)
list(FILTER HW_SOURCES EXCLUDE REGEX "/GlDispatch.cpp$") # in the executable: it includes the generated stub list
file(GLOB APP_SOURCES CONFIGURE_DEPENDS app/*.cpp)
list(FILTER APP_SOURCES EXCLUDE REGEX "/Main.cpp$")
add_library(code STATIC ${HW_SOURCES} ${APP_SOURCES})
target_include_directories(code PUBLIC hw ${CMAKE_SOURCE_DIR} ${GENERATED})
target_link_libraries(code PUBLIC juce PRIVATE app_fonts PUBLIC ALSA::ALSA X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(code PUBLIC openmpt)
target_compile_options(code PRIVATE -Wall -Wextra -Wshadow $<$<CONFIG:Release>:-O3>)
# The synth: let GCC vectorise float loops for NEON (NEON flushes denormals anyway).
set_source_files_properties(app/Synth.cpp PROPERTIES COMPILE_OPTIONS
    "-fno-math-errno;-fno-trapping-math;$<$<BOOL:${CMAKE_CROSSCOMPILING}>:-funsafe-math-optimizations>")
if(CMAKE_CROSSCOMPILING)
    target_compile_options(code PRIVATE -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
endif()

# A build-machine tool that renders the screen to PNG files (tools/preview.cpp, scripts/preview.sh); not for the unit.
option(TRACK16_PREVIEW "build track16_preview" OFF)
if(TRACK16_PREVIEW)
    add_executable(track16_preview tools/preview.cpp)
    target_link_libraries(track16_preview PRIVATE code)
endif()

add_executable(${APP_NAME} app/Main.cpp hw/common/GlDispatch.cpp)
target_link_libraries(${APP_NAME} PRIVATE code)
set_target_properties(${APP_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GL dispatch: a stub for every egl*/gl*/fb* function the code calls, bound at run time to one driver flavour (the
# unit's GPU driver comes in -fb and -x11 flavours that must not be mixed), so no GL library is linked.
add_custom_command(
    OUTPUT ${GENERATED}/gldispatch_stubs.S ${GENERATED}/gldispatch_names.inc
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen-gl-dispatch.py --arch ${GL_ARCH} ${CMAKE_NM} ${GENERATED}
            $<TARGET_FILE:code>
    DEPENDS code tools/gen-gl-dispatch.py
    VERBATIM)
target_sources(${APP_NAME} PRIVATE ${GENERATED}/gldispatch_stubs.S)

# The boot menu reads the name, version, id and a 128 × 128 icon from an ELF note, without running the program.
add_custom_command(
    OUTPUT ${GENERATED}/AppInfoNote.cpp
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen-app-info.py ${CMAKE_SOURCE_DIR}/resources/app-icon.png
            ${GENERATED}/AppInfoNote.cpp ${APP_DISPLAY_NAME} ${APP_VERSION} ${APP_ID}
    DEPENDS tools/gen-app-info.py resources/app-icon.png
    VERBATIM)
target_sources(${APP_NAME} PRIVATE ${GENERATED}/AppInfoNote.cpp)
