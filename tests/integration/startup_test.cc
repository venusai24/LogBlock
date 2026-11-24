// Startup rules of the plugin, plus a few self-tests of the harness itself.
//
// Refusal tests never connect. They start nbdkit and expect it to exit with a
// non-zero status and a message that names the problem. How exactly the message
// is worded is the plugin's choice, so the checks look for the key words:
//  - the log path as given on the command line,
//  - the setting name ("log", "cap", or the unknown key),
//  - for a bad cap value, the minimum "4136".

#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "harness.h"
#include "record.h"

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

// Settings that point a second Harness's server at the log of `first`.
std::vector<std::string> second_settings(const Harness& first) {
  return {"log=" + first.log_path(), "cap=1M"};
}

// Writes a log of `records` DATA records (blocks 0, 1, 2, ...) straight to `path`,
// without the engine, so a test controls every byte. Each payload is filled with
// the record's own number (1-based) so the records differ.
void write_log(const std::string& path, uint64_t records) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  for (uint64_t i = 0; i < records; ++i) {
    std::array<uint8_t, kBlockSize> payload{};
    payload.fill(static_cast<uint8_t>(i + 1));
    std::array<uint8_t, kDataRecordSize> record{};
    const Status status = encode_data_record(i + 1, i, payload, record);
    ASSERT_TRUE(status.ok()) << status.message();
    out.write(reinterpret_cast<const char*>(record.data()), record.size());
  }
}

// Appends `count` zero bytes to the file: what a torn write leaves behind when the
// file grew but the data never reached the disk.
void append_zeros(const std::string& path, std::size_t count) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  const std::string zeros(count, '\0');
  out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
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

TEST(Startup, NonEmptyValidLogStartsAndIsLeftUnchanged) {
  // M2 replaces M1's refusal of a non-empty log: a valid log is recovered. A first
  // server writes one block and shuts down; a second one starts on the same log.
  Harness first;
  ASSERT_TRUE(first.start(first.default_settings("1M"))) << first.stop().stderr_text;
  {
    NbdHandle nbd = first.connect();
    ASSERT_TRUE(nbd);
    const std::vector<char> data(4096, 'q');
    ASSERT_EQ(nbd_pwrite(nbd.get(), data.data(), data.size(), 0, 0), 0) << nbd_get_error();
    ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();
  }
  ASSERT_TRUE(first.stop(SIGTERM).clean());
  const std::string before = sha256_file(first.log_path());
  ASSERT_EQ(before.size(), 64u) << "sha256sum failed";

  // A valid log has no torn tail to remove, so recovery must not change a byte.
  Harness second;
  expect_clean_start_and_stop(second, second_settings(first));
  EXPECT_EQ(sha256_file(first.log_path()), before);
}

// --- Refusals -----------------------------------------------------------------

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

TEST(Startup, CheckpointIntervalOf4135IsRefused) {
  // One byte under one DATA record: a single record could never trigger a checkpoint.
  Harness harness;
  std::vector<std::string> settings = harness.default_settings("1M");
  settings.emplace_back("checkpoint_interval=4135");
  const ServerExit result = start_expecting_refusal(harness, settings);
  EXPECT_TRUE(mentions_setting(result.stderr_text, "checkpoint_interval"))
      << "stderr:\n"
      << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("4136"), std::string::npos)
      << "the message should give the minimum; stderr:\n"
      << result.stderr_text;
}

TEST(Startup, InvalidCheckpointIntervalIsRefused) {
  Harness harness;
  std::vector<std::string> settings = harness.default_settings("1M");
  settings.emplace_back("checkpoint_interval=banana");
  const ServerExit result = start_expecting_refusal(harness, settings);
  EXPECT_TRUE(mentions_setting(result.stderr_text, "checkpoint_interval"))
      << "stderr:\n"
      << result.stderr_text;
}

TEST(Startup, CheckpointIntervalOf4136StartsAndSizeSuffixesWork) {
  for (const char* value : {"4136", "64M", "1G"}) {
    Harness harness;
    std::vector<std::string> settings = harness.default_settings("1M");
    settings.push_back(std::string("checkpoint_interval=") + value);
    expect_clean_start_and_stop(harness, settings);
  }
}

// --- D34: a checkpoint file that cannot be opened refuses the start ------------------------------
// ENOENT is a missing checkpoint (silent). Any other open error leaves what the checkpoint
// holds unknown, so the start is refused, naming the path and the OS error, and nothing changes.

TEST(Startup, ACheckpointFileThatCannotBeOpenedRefusesAnExistingLog) {
  if (geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";
  Harness harness;
  write_log(harness.log_path(), 3);
  const std::string ckpt = harness.log_path() + ".ckpt";
  { std::ofstream(ckpt, std::ios::binary) << "x"; }
  std::filesystem::permissions(ckpt, std::filesystem::perms::none);
  const std::string before = sha256_file(harness.log_path());
  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("1M"));
  EXPECT_NE(result.stderr_text.find(ckpt), std::string::npos) << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("Permission denied"), std::string::npos)
      << "the OS error should be in the message:\n"
      << result.stderr_text;
  EXPECT_EQ(sha256_file(harness.log_path()), before) << "a refusal must not change the log";
  std::filesystem::permissions(ckpt, std::filesystem::perms::owner_all);
}

TEST(Startup, ACheckpointFileThatCannotBeOpenedRefusesAMissingLogAndCreatesNothing) {
  if (geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";
  Harness harness;
  const std::string ckpt = harness.log_path() + ".ckpt";
  { std::ofstream(ckpt, std::ios::binary) << "x"; }
  std::filesystem::permissions(ckpt, std::filesystem::perms::none);
  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("1M"));
  EXPECT_NE(result.stderr_text.find(ckpt), std::string::npos) << result.stderr_text;
  EXPECT_FALSE(std::filesystem::exists(harness.log_path()))
      << "an unreadable checkpoint beside a missing log must not start a new empty device";
  std::filesystem::permissions(ckpt, std::filesystem::perms::owner_all);
}

TEST(Startup, ADirectoryWhereTheCheckpointFileShouldBeRefusesTheStart) {
  Harness harness;
  write_log(harness.log_path(), 2);
  const std::string ckpt = harness.log_path() + ".ckpt";
  std::filesystem::create_directory(ckpt);
  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("1M"));
  EXPECT_NE(result.stderr_text.find(ckpt), std::string::npos) << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("Is a directory"), std::string::npos) << result.stderr_text;
}

TEST(Startup, AMissingCheckpointFileIsStillSilent) {
  Harness harness;
  write_log(harness.log_path(), 2);
  ASSERT_TRUE(harness.start(harness.default_settings("1M"))) << harness.stop().stderr_text;
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;
  EXPECT_EQ(result.stderr_text.find("[warning]"), std::string::npos) << result.stderr_text;
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

// --- Recovery refusals and the torn-tail warning (US2, FR-006, FR-008, FR-010) ---

TEST(Startup, CorruptMidLogIsRefusedAndLeavesTheLogUnchanged) {
  Harness harness;
  write_log(harness.log_path(), 3);
  // Flip one payload byte of the middle record. Records follow each other, so the
  // second one starts at kDataRecordSize. A bad CRC with a record after it is
  // corruption, never a torn tail.
  {
    std::fstream file(harness.log_path(), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(static_cast<std::streamoff>(kDataRecordSize + kRecordHeaderSize + 10));
    file.put('\x7f');
  }
  const std::string before = sha256_file(harness.log_path());
  ASSERT_EQ(before.size(), 64u);

  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("1M"));
  EXPECT_NE(result.stderr_text.find("corrupt"), std::string::npos) << result.stderr_text;
  EXPECT_NE(result.stderr_text.find(harness.log_path()), std::string::npos) << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("offset " + std::to_string(kDataRecordSize)), std::string::npos)
      << "the message should give the byte offset of the bad record; stderr:\n"
      << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("last good seq 1"), std::string::npos) << result.stderr_text;
  EXPECT_EQ(sha256_file(harness.log_path()), before) << "a refusal must not change the log";
}

TEST(Startup, ValidLogOverTheCapIsRefusedAndLeavesTheLogUnchanged) {
  Harness harness;
  write_log(harness.log_path(), 2);  // 8272 bytes, valid
  const std::string before = sha256_file(harness.log_path());
  ASSERT_EQ(before.size(), 64u);

  // 4136 is the smallest allowed cap: valid for the plugin, but under the log's size.
  const ServerExit result = start_expecting_refusal(harness, harness.default_settings("4136"));
  EXPECT_NE(result.stderr_text.find("8272"), std::string::npos)
      << "the message should name the log's size; stderr:\n"
      << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("4136"), std::string::npos)
      << "the message should name the cap; stderr:\n"
      << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("raise"), std::string::npos)
      << "the message should say to raise the cap; stderr:\n"
      << result.stderr_text;
  EXPECT_EQ(sha256_file(harness.log_path()), before) << "a refusal must not change the log";
}

TEST(Startup, ZeroTailIsRemovedWithAWarningAndTheServerStarts) {
  Harness harness;
  write_log(harness.log_path(), 2);
  const uint64_t good_end = 2 * kDataRecordSize;
  const std::string good_hash = sha256_file(harness.log_path());
  append_zeros(harness.log_path(), 100);
  ASSERT_EQ(fs::file_size(harness.log_path()), good_end + 100);

  ASSERT_TRUE(harness.start(harness.default_settings("1M"))) << harness.stop().stderr_text;
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;

  // The contract line: [warning] torn tail removed log=<path> offset=<n> bytes=<n>
  const std::string expected = "[warning] torn tail removed log=" + harness.log_path() +
                               " offset=" + std::to_string(good_end) + " bytes=100";
  EXPECT_NE(result.stderr_text.find(expected), std::string::npos)
      << "expected: " << expected << "\nstderr:\n"
      << result.stderr_text;
  EXPECT_EQ(fs::file_size(harness.log_path()), good_end) << "the tail must be truncated";
  EXPECT_EQ(sha256_file(harness.log_path()), good_hash) << "the good records must be untouched";
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
