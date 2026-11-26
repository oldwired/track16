# Host tests of the player engine (no hardware, no UI): build and run inside the toolchain container with
# scripts/test-host.sh. Uses the build machine's own compiler, not the cross toolchain.
cmake_minimum_required(VERSION 3.25)
project(track16_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/libopenmpt.cmake)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
add_executable(player_test player_test.cpp LogStub.cpp ${ROOT}/app/Player.cpp ${ROOT}/app/Song.cpp ${ROOT}/app/Library.cpp)
target_include_directories(player_test PRIVATE ${ROOT} ${ROOT}/hw)
target_link_libraries(player_test PRIVATE openmpt Threads::Threads)
target_compile_options(player_test PRIVATE -Wall -Wextra)
