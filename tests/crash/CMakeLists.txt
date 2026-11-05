# Crash test: its own executable, deliberately NOT registered with CTest,
# because the Stop hook runs all of CTest with a 60-second timeout. Run it with
# scripts/crash_test.sh. It reuses the integration harness by compiling
# the same source file; tests/integration/CMakeLists.txt is not touched.
add_executable(crash_test
  crash_test.cc
  ../integration/harness.cc)
target_include_directories(crash_test PRIVATE ../integration)
# harness.cc reports problems with gtest's ADD_FAILURE, so gtest is linked, but
# crash_test.cc has its own main().
target_link_libraries(crash_test PRIVATE
  logblock_core PkgConfig::LIBNBD GTest::gtest)
add_dependencies(crash_test nbdkit_logblock_plugin)
target_compile_definitions(crash_test PRIVATE
  LOGBLOCK_TEST_TMP_DIR="${CMAKE_BINARY_DIR}/test-tmp"
  LOGBLOCK_PLUGIN_PATH="$<TARGET_FILE:nbdkit_logblock_plugin>")
