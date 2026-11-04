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
set(ME_TESTS book_test random_test engine_test engine_random_test replay_test)
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
# S4 of note 002: the sample day's book events (D7), the whole day (label `data`).
add_executable(day_test tests/day_test.cpp)
target_link_libraries(day_test PRIVATE me)
target_compile_definitions(day_test PRIVATE ME_DATA_DIR="${ME_DATA_DIR}")
add_test(NAME day_test COMMAND day_test)
set_tests_properties(day_test PROPERTIES LABELS data TIMEOUT 14400)
# S2 and S3 of note 004: the day replayed as order entry, every combination in lockstep (label `data`).
add_executable(replay_day_test tests/replay_day_test.cpp)
target_link_libraries(replay_day_test PRIVATE me)
target_compile_definitions(replay_day_test PRIVATE ME_DATA_DIR="${ME_DATA_DIR}")
add_test(NAME replay_day_test COMMAND replay_day_test)
set_tests_properties(replay_day_test PROPERTIES LABELS data TIMEOUT 14400)
# S5 of note 002: the books' cost, the day and the synthetic cases.
add_executable(book_bench bench/book_bench.cpp)
target_link_libraries(book_bench PRIVATE me)
target_compile_definitions(book_bench PRIVATE ME_DATA_DIR="${ME_DATA_DIR}" LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")
# S4 of note 003: matching's synthetic cases (D6).
add_executable(match_bench bench/match_bench.cpp)
target_link_libraries(match_bench PRIVATE me)
target_compile_definitions(match_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")
# S4 of note 004: the oracle's counts, the day through the default book.
add_executable(replay_oracle bench/replay_oracle.cpp)
target_link_libraries(replay_oracle PRIVATE me)
target_compile_definitions(replay_oracle PRIVATE ME_DATA_DIR="${ME_DATA_DIR}" LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")
# One number everywhere: RESULTS.md and the README agree with the published JSON.
add_test(NAME report_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py report --check
                                  --lockfree-ipc ${lockfree_ipc_SOURCE_DIR})
