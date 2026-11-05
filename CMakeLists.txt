cmake_minimum_required(VERSION 3.20)
project(LogBlock LANGUAGES CXX)

# --- Language settings -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Extensions off: -std=c++20 instead of -std=gnu++20, so we only use standard C++.
set(CMAKE_CXX_EXTENSIONS OFF)
# clang-tidy and editors read build/compile_commands.json.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The documented configure command passes -DCMAKE_BUILD_TYPE=Debug, but make
# Debug the default so a bare `cmake -S . -B build` behaves the same way.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# --- Sanitizers --------------------------------------------------------------
# -DSANITIZE=address -> AddressSanitizer + UBSan (they combine well).
# -DSANITIZE=thread  -> ThreadSanitizer (cannot be combined with ASan).
# The flags must be given at both compile and link time, so the runtime library
# is linked in.
set(SANITIZE "" CACHE STRING "Sanitizer build: empty, 'address' or 'thread'")
set_property(CACHE SANITIZE PROPERTY STRINGS "" address thread)

if(SANITIZE STREQUAL "address")
  # Do not let UBSan recover, so any undefined behavior fails the test run.
  set(_san_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(SANITIZE STREQUAL "thread")
  set(_san_flags -fsanitize=thread)
elseif(NOT SANITIZE STREQUAL "")
  message(FATAL_ERROR "SANITIZE must be empty, 'address' or 'thread', got '${SANITIZE}'")
endif()

if(_san_flags)
  # Frame pointers give readable stack traces in sanitizer reports.
  add_compile_options(${_san_flags} -fno-omit-frame-pointer)
  add_link_options(${_san_flags})
  message(STATUS "Sanitizer build: ${SANITIZE}")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# --- Dependencies ------------------------------------------------------------
# Used in M1. LZ4, Google Benchmark and CLI11 are left out on purpose.
find_package(GTest REQUIRED)
find_package(Crc32c REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NBDKIT REQUIRED IMPORTED_TARGET nbdkit) # plugin header only
pkg_check_modules(LIBNBD REQUIRED IMPORTED_TARGET libnbd) # test client

# --- Core library ------------------------------------------------------------
# Static, but position-independent so the same code can be linked into the
# plugin (a shared object) and into the tests.
add_library(logblock_core STATIC
  src/block_map.cc
  src/engine.cc
  src/io_posix.cc
  src/log_file.cc
  src/log_scan.cc
  src/record.cc)
set_target_properties(logblock_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# include/ is the public interface. src/ holds internal headers such as
# record.h, and is visible too so the unit tests can test those pieces directly.
target_include_directories(logblock_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(logblock_core PUBLIC Crc32c::crc32c spdlog::spdlog fmt::fmt)

# --- nbdkit plugin -----------------------------------------------------------
# MODULE: a library that is only loaded at run time with dlopen, never linked
# against. nbdkit looks for a file named like nbdkit-<name>-plugin.so, so no
# "lib" prefix. It is written to the top of the build directory, which is where
# plugin-cli.md says to find it.
add_library(nbdkit_logblock_plugin MODULE src/nbdkit/plugin.cc)
set_target_properties(nbdkit_logblock_plugin PROPERTIES
  OUTPUT_NAME nbdkit-logblock-plugin
  PREFIX ""
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(nbdkit_logblock_plugin PRIVATE logblock_core PkgConfig::NBDKIT)

# --- Tests -------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
