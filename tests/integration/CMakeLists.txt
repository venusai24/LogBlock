# Integration tests: a real nbdkit with the logblock plugin, a libnbd client, and a
# Unix socket under the build directory. No root and no kernel NBD device.
add_executable(integration_tests
  harness.cc
  startup_test.cc
  nbd_io_test.cc
  counters_test.cc
  zero_test.cc)
# logblock_core is linked only for the record-size constants in logblock/engine.h.
# The tests never call the engine; they talk to the plugin over NBD.
target_link_libraries(integration_tests PRIVATE
  logblock_core PkgConfig::LIBNBD GTest::gtest_main)

# The tests start the plugin as a separate process, so build it first.
add_dependencies(integration_tests nbdkit_logblock_plugin)

# Logs and sockets go under the build directory. /tmp is tmpfs on Fedora, where
# fsync proves little. $<TARGET_FILE:...> is the plugin's real output path.
target_compile_definitions(integration_tests PRIVATE
  LOGBLOCK_TEST_TMP_DIR="${CMAKE_BINARY_DIR}/test-tmp"
  LOGBLOCK_PLUGIN_PATH="$<TARGET_FILE:nbdkit_logblock_plugin>")

# TIMEOUT: a hung nbdkit must fail one test, not hang the whole ctest run.
include(GoogleTest)
gtest_discover_tests(integration_tests PROPERTIES LABELS integration TIMEOUT 30)
