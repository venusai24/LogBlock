// Startup rules of the plugin, plus a few self-tests of the harness itself.
//
// Refusal tests never connect. They start nbdkit and expect it to exit with a
// non-zero status and a message that names the problem. How exactly the message
// is worded is the plugin's choice, so the checks look for the key words:
//  - the log path as given on the command line, and the words "no recovery",
//  - the setting name ("log", "cap", or the unknown key),
//  - for a bad cap value, the minimum "4136".

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "harness.h"

namespace logblock::itest {
namespace {

namespace fs = std::filesystem;

// Starts nbdkit with `settings` and checks that it refuses: it must not start
// serving, and must exit with a non-zero status. Returns the result so the
// caller can check the message.
ServerExit start_expecting_refusal(Harness& harness, const std::vector<std::string>& settings) {
  EXPECT_FALSE(harness.start(settings)) << "nbdkit should have refused to start";
  const ServerExit result = harness.stop();
  EXPECT_TRUE(result.exited) << "nbdkit was killed by signal " << result.term_signal;
  EXPECT_NE(result.exit_code, 0) << "stderr:\n" << result.stderr_text;
  return result;
}

// Starts nbdkit with `settings`, expects it to serve, then shuts it down with
// SIGTERM and expects a clean exit.
void expect_clean_start_and_stop(Harness& harness, const std::vector<std::string>& settings) {
  ASSERT_TRUE(harness.start(settings)) << harness.stop().stderr_text;
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << "exit code " << result.exit_code << ", stderr:\n"
                              << result.stderr_text;
}

// --- Valid starts -------------------------------------------------------------

TEST(Startup, MissingLogStarts) {
  Harness harness;
  ASSERT_FALSE(fs::exists(harness.log_path()));
  expect_clean_start_and_stop(harness, harness.default_settings("1M"));
  // The contract says "created if missing".
  ASSERT_TRUE(fs::exists(harness.log_path()));
  EXPECT_EQ(fs::file_size(harness.log_path()), 0u);
}

TEST(Startup, EmptyLogStarts) {
  Harness harness;
  std::ofstream(harness.log_path()).close();  // create an empty file
  expect_clean_start_and_stop(harness, harness.default_settings("1M"));
}

TEST(Startup, MinimumCapStarts) {
  // 4136 bytes hold exactly one DATA record (40-byte header + 4096 payload).
  Harness harness;
  expect_clean_start_and_stop(harness, harness.default_settings("4136"));
}

// --- Refusals -----------------------------------------------------------------

TEST(Startup, NonEmptyLogIsRefusedAndLeftUnchanged) {
  Harness harness;
  {
    // Any content will do: the plugin must refuse a non-empty file without even
    // trying to read it, because M1 has no recovery.
    std::ofstream log(harness.log_path(), std::ios::binary);
    log << "not an empty log";
  }
  const std::string before = sha256_file(harness.log_path());
  ASSERT_EQ(before.size(), 64u) << "sha256sum failed";

  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("1M"));

  EXPECT_NE(result.stderr_text.find(harness.log_path()), std::string::npos)
      << "the message should name the log; stderr:\n"
      << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("no recovery"), std::string::npos)
      << "the message should say M1 has no recovery; stderr:\n"
      << result.stderr_text;
  // The refusal must not touch the file.
  EXPECT_EQ(sha256_file(harness.log_path()), before);
}

TEST(Startup, MissingLogSettingIsRefused) {
  Harness harness;
  const ServerExit result = start_expecting_refusal(harness, {"cap=1M"});
  EXPECT_TRUE(mentions_setting(result.stderr_text, "log")) << "stderr:\n" << result.stderr_text;
}

TEST(Startup, MissingCapSettingIsRefused) {
  Harness harness;
  const ServerExit result = start_expecting_refusal(harness, {"log=" + harness.log_path()});
  EXPECT_TRUE(mentions_setting(result.stderr_text, "cap")) << "stderr:\n" << result.stderr_text;
}

TEST(Startup, UnknownSettingIsRefused) {
  Harness harness;
  std::vector<std::string> settings = harness.default_settings("1M");
  settings.emplace_back("bogus=1");
  const ServerExit result = start_expecting_refusal(harness, settings);
  EXPECT_NE(result.stderr_text.find("bogus"), std::string::npos)
      << "the message should name the unknown setting; stderr:\n"
      << result.stderr_text;
}

TEST(Startup, InvalidCapIsRefused) {
  Harness harness;
  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("banana"));
  EXPECT_TRUE(mentions_setting(result.stderr_text, "cap")) << "stderr:\n" << result.stderr_text;
}

TEST(Startup, CapOf4135IsRefused) {
  // One byte under the minimum: too small for even one DATA record.
  Harness harness;
  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("4135"));
  EXPECT_TRUE(mentions_setting(result.stderr_text, "cap")) << "stderr:\n" << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("4136"), std::string::npos)
      << "the message should give the minimum; stderr:\n"
      << result.stderr_text;
}

TEST(Startup, UnopenableLogIsRefused) {
  Harness harness;
  // The parent directory does not exist, so open() fails with ENOENT.
  const std::string bad_log = harness.dir() + "/no_such_dir/log";
  const ServerExit result = start_expecting_refusal(harness, {"log=" + bad_log, "cap=1M"});
  EXPECT_NE(result.stderr_text.find(bad_log), std::string::npos)
      << "the message should name the log; stderr:\n"
      << result.stderr_text;
  // "Names the log and the reason": the reason is the strerror text.
  EXPECT_NE(result.stderr_text.find(std::strerror(ENOENT)), std::string::npos)
      << "the message should give the reason; stderr:\n"
      << result.stderr_text;
}

// --- Harness self-tests --------------------------------------------------------
// These use nbdkit's built-in "memory" plugin, so they check the harness without
// depending on the logblock plugin, and they should pass at every stage.

TEST(HarnessSelfTest, ServesAndStopsCleanlyOnSigterm) {
  Harness harness;

  // The run directory is private.
  struct stat dir_stat{};
  ASSERT_EQ(::stat(harness.dir().c_str(), &dir_stat), 0);
  EXPECT_EQ(dir_stat.st_mode & 07777, 0700u);

  ASSERT_TRUE(harness.start({"size=1M"}, "memory")) << harness.stop().stderr_text;
  {
    NbdHandle nbd = harness.connect();
    ASSERT_TRUE(nbd);
    EXPECT_EQ(nbd_get_size(nbd.get()), 1 << 20);
    std::vector<char> buf(4096, 'x');
    ASSERT_EQ(nbd_pread(nbd.get(), buf.data(), buf.size(), 0, 0), 0) << nbd_get_error();
    EXPECT_EQ(buf, std::vector<char>(4096, 0)) << "a new memory disk reads as zeros";
  }
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;
}

TEST(HarnessSelfTest, SigkillIsReportedAsSignal) {
  Harness harness;
  ASSERT_TRUE(harness.start({"size=1M"}, "memory"));
  const ServerExit result = harness.stop(SIGKILL);
  EXPECT_FALSE(result.exited);
  EXPECT_EQ(result.term_signal, SIGKILL);
}

TEST(HarnessSelfTest, EarlyExitIsDetectedWithStderr) {
  Harness harness;
  // nbdkit rejects an unknown parameter before it creates the socket. start()
  // must notice the exit and return instead of waiting for the full timeout.
  EXPECT_FALSE(harness.start({"no_such_parameter=1"}, "memory"));
  const ServerExit result = harness.stop();
  EXPECT_TRUE(result.exited);
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stderr_text.find("no_such_parameter"), std::string::npos) << result.stderr_text;
}

TEST(HarnessSelfTest, SocketPathThatIsTooLongIsRejected) {
  // 100 characters in the base directory alone push the socket path over 107.
  const std::string long_base = Harness::default_base_dir() + "/" + std::string(100, 'x');
  EXPECT_THROW({ Harness harness(long_base); }, std::runtime_error);
  std::error_code ignored;
  fs::remove_all(long_base, ignored);
}

}  // namespace
}  // namespace logblock::itest
