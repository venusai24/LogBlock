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

#include <cstdint>
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

}  // namespace
}  // namespace logblock::itest
