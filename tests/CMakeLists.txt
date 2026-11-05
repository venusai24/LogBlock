# Unit tests: GoogleTest against the engine only, no nbdkit and no kernel.
# Later tasks add their test files to this one executable.
# Test-only helpers shared by the test executables, such as the in-memory Io
# that injects failures. Never linked into the plugin.
add_library(test_support STATIC support/fault_io.cc)
target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_support PUBLIC logblock_core)

add_executable(unit_tests
  unit/smoke_test.cc
  unit/status_test.cc
  unit/io_posix_test.cc
  unit/fault_io_test.cc
  unit/engine_constants_test.cc
  unit/record_test.cc
  unit/block_map_test.cc
  unit/log_file_test.cc
  unit/engine_create_test.cc
  unit/engine_read_write_test.cc
  unit/engine_trim_test.cc
  unit/engine_range_test.cc
  unit/engine_cap_test.cc
  unit/engine_corrupt_test.cc
  unit/engine_failed_append_test.cc
  unit/engine_flush_test.cc
  unit/log_scan_test.cc)
target_link_libraries(unit_tests PRIVATE test_support logblock_core GTest::gtest_main)

# Tests that need real files put them here, under the build directory. /tmp is
# tmpfs on Fedora, where fsync proves little.
target_compile_definitions(unit_tests PRIVATE
  LOGBLOCK_TEST_TMP_DIR="${CMAKE_BINARY_DIR}/test-tmp")

# gtest_discover_tests registers every TEST as its own CTest test, so
# `ctest -R <name>` runs one of them. The "unit" label lets scripts/check.sh run
# only these in the TSan build.
include(GoogleTest)
gtest_discover_tests(unit_tests PROPERTIES LABELS unit)

# Rootless integration tests against a real nbdkit (label "integration") and the
# Python script tests (label "scripts"). Each directory has its own CMakeLists.txt.
add_subdirectory(integration)
add_subdirectory(scripts)

add_subdirectory(crash)
