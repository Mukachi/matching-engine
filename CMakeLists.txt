cmake_minimum_required(VERSION 3.28)
project(matching-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(me INTERFACE)
target_include_directories(me INTERFACE include)
target_compile_options(me INTERFACE -Wall -Wextra -Wpedantic -Werror)
# x86-64-v3, as the repos before it: a named level, recorded in every run's flags.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(ME_ARCH_FLAGS -march=x86-64-v3)
  target_compile_options(me INTERFACE ${ME_ARCH_FLAGS})
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()

# The repos before it, each fetched at a pinned commit into build/_deps,
# headers only (SOURCE_SUBDIR names no directory): lockfree-ipc's harness, and
# the feed handler's decoder and reference book for the sample day (note 002,
# D7; note 004).
include(FetchContent)
set(ME_LFIPC_COMMIT c733131c95198f08cc9032b7ae105cfaf6f7cd88)
set(ME_ITCHFH_COMMIT 36d7a92a6bcab383320697b8f072bf83bbfe4118)
FetchContent_Declare(lockfree_ipc
  GIT_REPOSITORY https://github.com/Mukachi/lockfree-ipc.git
  GIT_TAG ${ME_LFIPC_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_Declare(itch_feed_handler
  GIT_REPOSITORY https://github.com/Mukachi/itch-feed-handler.git
  GIT_TAG ${ME_ITCHFH_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(lockfree_ipc itch_feed_handler)

# One harness: the feed handler must pin the same lockfree-ipc as this repo.
file(STRINGS ${itch_feed_handler_SOURCE_DIR}/CMakeLists.txt itchfh_pin REGEX "^set\\(ITCHFH_LFIPC_COMMIT ")
string(REGEX REPLACE "^set\\(ITCHFH_LFIPC_COMMIT ([0-9a-f]+)\\)$" "\\1" itchfh_pin "${itchfh_pin}")
if(NOT itchfh_pin STREQUAL ME_LFIPC_COMMIT)
  message(FATAL_ERROR "itch-feed-handler pins lockfree-ipc at '${itchfh_pin}', this repo at ${ME_LFIPC_COMMIT}")
endif()

# Build provenance for this repository, written by lockfree-ipc's script into
# lfipc/build_info.hpp, where its environment capture reads it (lockfree-ipc
# note 004).
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
add_custom_target(build_info ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          "-DFLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} -Wall -Wextra -Wpedantic -Werror ${ME_ARCH_FLAGS}"
          -P ${lockfree_ipc_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
  COMMENT "Recording build provenance")

add_library(deps INTERFACE)
target_include_directories(deps SYSTEM INTERFACE ${lockfree_ipc_SOURCE_DIR}/include ${itch_feed_handler_SOURCE_DIR}/include
                                                 ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(deps INTERFACE ME_LFIPC_COMMIT="${ME_LFIPC_COMMIT}" ME_ITCHFH_COMMIT="${ME_ITCHFH_COMMIT}")
add_dependencies(deps build_info)
target_link_libraries(me INTERFACE deps Threads::Threads)

# The sample day (note 001): not in the repo and never redistributed;
# itch-feed-handler's tools/fetch_day.py fetches it.
set(ME_DATA_DIR ${CMAKE_SOURCE_DIR}/data CACHE PATH "Where the sample day is")

# Tests: plain executables under CTest. Each one added with its component.
set(ME_TESTS book_test random_test)
foreach(t ${ME_TESTS})
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE me)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# S3 of note 002: the operations allocate nothing and make no system call,
# checked in the compiled code (not under sanitizers, which add calls).
add_executable(hot_path_subject tests/hot_path_subject.cpp)
target_link_libraries(hot_path_subject PRIVATE me)
if(CMAKE_OBJDUMP AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  add_test(NAME check_hot_path
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_hot_path.py
                   ${CMAKE_OBJDUMP} $<TARGET_FILE:hot_path_subject>)
endif()
