// The counters the plugin reports at clean shutdown.
//
// Assumed report format:
// on SIGTERM the plugin writes ONE line to STDERR that contains these five
// key=value pairs, in any order, with unsigned decimal values:
//
//   [info] counters reads=3 writes=3 trims=2 flushes=2 bytes_logged=16584
//
// Anything else on the line (spdlog's time stamp, level, a prefix) is ignored.
// The parser is parse_counters() below; if the plugin author picks another
// format, that is the one function to change. The line must go to stderr, not
// stdout: spdlog's default logger writes to stdout, so the plugin needs a
// stderr logger.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "harness.h"
#include "logblock/engine.h"

namespace logblock::itest {
namespace {

using Bytes = std::vector<uint8_t>;

struct Counters {
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t trims = 0;
  uint64_t flushes = 0;
  uint64_t bytes_logged = 0;

  bool operator==(const Counters&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Counters& c) {
  return out << "{reads=" << c.reads << " writes=" << c.writes << " trims=" << c.trims
             << " flushes=" << c.flushes << " bytes_logged=" << c.bytes_logged << "}";
}

// Reads "<key>=<digits>" from `line`. \b stops "reads" from matching inside a
// longer word.
std::optional<uint64_t> find_value(const std::string& line, const std::string& key) {
  const std::regex pattern("\\b" + key + "=([0-9]+)");
  std::smatch match;
  if (!std::regex_search(line, match, pattern)) return std::nullopt;
  return std::stoull(match[1].str());
}

// Finds the counter line in the stderr text. Returns nothing if there is no line
// with all five keys.
std::optional<Counters> parse_counters(const std::string& stderr_text) {
  std::optional<Counters> found;
  std::size_t start = 0;
  while (start < stderr_text.size()) {
    std::size_t end = stderr_text.find('\n', start);
    if (end == std::string::npos) end = stderr_text.size();
    const std::string line = stderr_text.substr(start, end - start);
    start = end + 1;

    const auto reads = find_value(line, "reads");
    const auto writes = find_value(line, "writes");
    const auto trims = find_value(line, "trims");
    const auto flushes = find_value(line, "flushes");
    const auto bytes = find_value(line, "bytes_logged");
    if (reads && writes && trims && flushes && bytes) {
      found = Counters{*reads, *writes, *trims, *flushes, *bytes};
    }
  }
  return found;
}

// Stops the server with SIGTERM and returns the counters it reported. The
// client is closed first so that nbdkit has no open connection to wait for.
std::optional<Counters> shut_down_and_read_counters(Harness& harness, NbdHandle& nbd) {
  nbd.reset();
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << "exit code " << result.exit_code << ", stderr:\n"
                              << result.stderr_text;
  const auto counters = parse_counters(result.stderr_text);
  EXPECT_TRUE(counters.has_value()) << "no line with reads, writes, trims, flushes and "
                                       "bytes_logged in stderr:\n"
                                    << result.stderr_text;
  return counters;
}

void write_ok(nbd_handle* nbd, std::size_t size, uint64_t offset) {
  const Bytes data(size, 0x5A);
  ASSERT_EQ(nbd_pwrite(nbd, data.data(), data.size(), offset, 0), 0) << nbd_get_error();
}

void read_ok(nbd_handle* nbd, std::size_t size, uint64_t offset) {
  Bytes data(size);
  ASSERT_EQ(nbd_pread(nbd, data.data(), data.size(), offset, 0), 0) << nbd_get_error();
}

TEST(Counters, ScriptedWorkloadIsCountedExactly) {
  Harness harness;
  ASSERT_TRUE(harness.start(harness.default_settings("1M"))) << "stderr:\n"
                                                             << harness.stop().stderr_text;
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);
  nbd_handle* h = nbd.get();

  // 3 writes, which append 4 DATA records:
  write_ok(h, kBlockSize, 0);               // 1 record: block 0
  write_ok(h, 2 * kBlockSize, kBlockSize);  // 2 records: blocks 1 and 2, still ONE request
  write_ok(h, 100, 10);                     // 1 record: a partial write rewrites all of block 0
  // 3 reads (reads append nothing):
  read_ok(h, kBlockSize, 0);
  read_ok(h, 2 * kBlockSize, kBlockSize);
  read_ok(h, kBlockSize, 100 * kBlockSize);  // a block never written
  // 2 trims: one TRIM record, and one that covers no full block and appends nothing.
  ASSERT_EQ(nbd_trim(h, kBlockSize, kBlockSize, 0), 0) << nbd_get_error();  // block 1
  ASSERT_EQ(nbd_trim(h, 100, 0, 0), 0) << nbd_get_error();                  // part of block 0
  // 2 flushes:
  ASSERT_EQ(nbd_flush(h, 0), 0) << nbd_get_error();
  ASSERT_EQ(nbd_flush(h, 0), 0) << nbd_get_error();

  // Failed requests, which must not be counted. libnbd's own bounds check
  // is off in the harness, so these reach nbdkit, which rejects them.
  Bytes buf(kBlockSize);
  EXPECT_EQ(nbd_pread(h, buf.data(), buf.size(), kDeviceSize, 0), -1);
  EXPECT_EQ(nbd_pwrite(h, buf.data(), buf.size(), kDeviceSize, 0), -1);
  EXPECT_EQ(nbd_trim(h, kBlockSize, kDeviceSize, 0), -1);
  ASSERT_TRUE(nbd_aio_is_ready(h)) << "the server dropped the connection";

  const auto counters = shut_down_and_read_counters(harness, nbd);
  ASSERT_TRUE(counters.has_value());

  // bytes_logged: 4 DATA records of 4,136 bytes and 1 TRIM record of 40 bytes.
  // Record headers count, and so does the full-block rewrite of the partial write.
  const Counters expected{.reads = 3,
                          .writes = 3,
                          .trims = 2,
                          .flushes = 2,
                          .bytes_logged = 4 * kDataRecordSize + 1 * kTrimRecordSize};
  EXPECT_EQ(*counters, expected);
}

TEST(Counters, FailedAppendIsNotCounted) {
  // A cap of 8192 fits one DATA record. The second write fails with ENOSPC, and
  // neither the request nor its bytes may show in the counters.
  Harness harness;
  ASSERT_TRUE(harness.start(harness.default_settings("8192"))) << "stderr:\n"
                                                               << harness.stop().stderr_text;
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);
  nbd_handle* h = nbd.get();

  write_ok(h, kBlockSize, 0);
  const Bytes data(kBlockSize, 0x77);
  EXPECT_EQ(nbd_pwrite(h, data.data(), data.size(), kBlockSize, 0), -1);
  EXPECT_EQ(nbd_get_errno(), ENOSPC);
  read_ok(h, kBlockSize, 0);

  const auto counters = shut_down_and_read_counters(harness, nbd);
  ASSERT_TRUE(counters.has_value());
  const Counters expected{
      .reads = 1, .writes = 1, .trims = 0, .flushes = 0, .bytes_logged = kDataRecordSize};
  EXPECT_EQ(*counters, expected);
}

TEST(Counters, IdleServerReportsZeros) {
  // Connect and disconnect without any request: the line is still written, and
  // every value is 0.
  Harness harness;
  ASSERT_TRUE(harness.start(harness.default_settings("1M"))) << "stderr:\n"
                                                             << harness.stop().stderr_text;
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);

  const auto counters = shut_down_and_read_counters(harness, nbd);
  ASSERT_TRUE(counters.has_value());
  EXPECT_EQ(*counters, Counters{});
}

// The six checkpoint fields of the counters line (contracts/plugin-cli.md, Shutdown).
struct CheckpointCounters {
  uint64_t written = 0;
  uint64_t failed = 0;
  uint64_t unreadable = 0;
  uint64_t duration_count = 0;
  uint64_t duration_max_us = 0;
  uint64_t duration_sum_us = 0;
};

std::optional<CheckpointCounters> parse_checkpoint_counters(const std::string& stderr_text) {
  const std::size_t at = stderr_text.find("counters ");
  if (at == std::string::npos) return std::nullopt;
  const std::string line = stderr_text.substr(at, stderr_text.find('\n', at) - at);
  const auto written = find_value(line, "checkpoints_written");
  const auto failed = find_value(line, "checkpoints_failed");
  const auto unreadable = find_value(line, "checkpoints_unreadable");
  const auto count = find_value(line, "checkpoint_duration_count");
  const auto max = find_value(line, "checkpoint_duration_max_us");
  const auto sum = find_value(line, "checkpoint_duration_sum_us");
  if (!written || !failed || !unreadable || !count || !max || !sum) return std::nullopt;
  return CheckpointCounters{*written, *failed, *unreadable, *count, *max, *sum};
}

TEST(Counters, CheckpointCountersAreReportedAtShutdown) {
  // An interval of two DATA records, and six one-block writes: checkpoints after records 2, 4
  // and 6. Nothing fails, and no slot was ignored at startup.
  Harness harness;
  std::vector<std::string> settings = harness.default_settings("1M");
  settings.emplace_back("checkpoint_interval=8272");
  ASSERT_TRUE(harness.start(settings)) << "stderr:\n" << harness.stop().stderr_text;
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);
  for (uint64_t block = 0; block < 6; ++block) {
    write_ok(nbd.get(), kBlockSize, block * kBlockSize);
  }

  nbd.reset();
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;
  const auto counters = parse_checkpoint_counters(result.stderr_text);
  ASSERT_TRUE(counters.has_value()) << "stderr:\n" << result.stderr_text;
  EXPECT_EQ(counters->written, 3U);
  EXPECT_EQ(counters->failed, 0U);
  EXPECT_EQ(counters->unreadable, 0U);
  EXPECT_EQ(counters->duration_count, 3U);
  EXPECT_LE(counters->duration_max_us, counters->duration_sum_us);
  EXPECT_EQ(result.stderr_text.find("[warning]"), std::string::npos) << result.stderr_text;
}

namespace {
// RAII: restores owner_all on a directory when it goes out of scope.
class PermissionRestorer {
 public:
  explicit PermissionRestorer(std::filesystem::path dir) : dir_(std::move(dir)) {}
  PermissionRestorer(const PermissionRestorer&) = delete;
  PermissionRestorer& operator=(const PermissionRestorer&) = delete;
  ~PermissionRestorer() {
    std::error_code ec;  // a destructor must not throw
    std::filesystem::permissions(dir_, std::filesystem::perms::owner_all, ec);
  }

 private:
  std::filesystem::path dir_;
};
}  // namespace

TEST(Counters, ACheckpointFileThatCannotBeCreatedWarnsOnceAndTheServerKeepsServing) {
  // The server starts normally, then its directory is made read-only, so creating "<log>.ckpt"
  // fails at the first checkpoint. Requests keep succeeding (D27), the failure is warned about
  // once, and it shows in the counters. (A .ckpt that cannot be opened at STARTUP is a refusal
  // instead: D34, tested in startup_test.cc.)
  if (geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  Harness harness;
  std::vector<std::string> settings = harness.default_settings("1M");
  settings.emplace_back("checkpoint_interval=8272");
  ASSERT_TRUE(harness.start(settings)) << "stderr:\n" << harness.stop().stderr_text;
  // r-x: the socket and the open log keep working; a new file cannot be made.
  // The guard puts the permissions back when the test ends, even on an early ASSERT return,
  // so the harness can delete its directory.
  const PermissionRestorer restore_permissions(harness.dir());
  std::filesystem::permissions(harness.dir(), std::filesystem::perms::owner_read |
                                                  std::filesystem::perms::owner_exec);
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);
  for (uint64_t block = 0; block < 6; ++block) {
    write_ok(nbd.get(), kBlockSize, block * kBlockSize);
  }
  read_ok(nbd.get(), kBlockSize, 0);
  ASSERT_EQ(nbd_flush(nbd.get(), 0), 0) << nbd_get_error();

  nbd.reset();
  const ServerExit result = harness.stop(SIGTERM);
  EXPECT_TRUE(result.clean()) << result.stderr_text;
  const std::string text = result.stderr_text;
  const std::size_t first = text.find("[warning] checkpoint failed");
  ASSERT_NE(first, std::string::npos) << "stderr:\n" << text;
  EXPECT_EQ(text.find("[warning] checkpoint failed", first + 1), std::string::npos)
      << "checkpoints are off after the first failure, so it is warned about once:\n"
      << text;
  EXPECT_NE(text.find("checkpoints are off until restart"), std::string::npos) << text;
  const auto counters = parse_checkpoint_counters(text);
  ASSERT_TRUE(counters.has_value()) << "stderr:\n" << text;
  EXPECT_EQ(counters->failed, 1U);
  EXPECT_EQ(counters->written, 0U);
  EXPECT_EQ(counters->duration_count, 0U);
}

}  // namespace
}  // namespace logblock::itest
