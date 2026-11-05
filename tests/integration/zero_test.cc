// Confirms the nbdkit zeroing fallback. The plugin has no
// .zero callback, so nbdkit should turn a write-zeroes request into ordinary writes,
// and should refuse a "fast zero" request, because a fallback to writes is not fast.
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

// Reads "<key>=<digits>" from the counters line of the plugin's stderr.
std::optional<uint64_t> counter(const std::string& stderr_text, const std::string& key) {
  const std::regex pattern("\\b" + key + "=([0-9]+)");
  std::smatch match;
  if (!std::regex_search(stderr_text, match, pattern)) return std::nullopt;
  return std::stoull(match[1].str());
}

TEST(Zero, WriteZeroesReadsBackAsZerosAndArrivesAsOrdinaryWrites) {
  Harness harness;
  ASSERT_TRUE(harness.start(harness.default_settings("1M"))) << harness.stop().stderr_text;
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);
  nbd_handle* h = nbd.get();

  // Three blocks of 0x5A, then zero the middle one plus half of each neighbour
  // (an unaligned range: bytes 2048 .. 10240 touch all three blocks).
  const Bytes data(3 * kBlockSize, 0x5A);
  ASSERT_EQ(nbd_pwrite(h, data.data(), data.size(), 0, 0), 0) << nbd_get_error();
  ASSERT_EQ(nbd_zero(h, 8192, 2048, 0), 0) << nbd_get_error();

  Bytes back(3 * kBlockSize);
  ASSERT_EQ(nbd_pread(h, back.data(), back.size(), 0, 0), 0) << nbd_get_error();
  Bytes want = data;
  std::fill(want.begin() + 2048, want.begin() + 2048 + 8192, uint8_t{0});
  EXPECT_EQ(back, want);

  nbd.reset();
  const ServerExit result = harness.stop();
  ASSERT_TRUE(result.clean()) << result.stderr_text;

  // 1 write for the data (3 records) + the zeroing, as however many writes nbdkit
  // made of it. nbdkit may split a big request, so only require "at least one" and
  // check bytes_logged: every touched block costs one DATA record. The zeroed range
  // touches blocks 0, 1 and 2 => at least 3 more records.
  const auto writes = counter(result.stderr_text, "writes");
  const auto bytes = counter(result.stderr_text, "bytes_logged");
  ASSERT_TRUE(writes.has_value() && bytes.has_value()) << result.stderr_text;
  EXPECT_GE(*writes, 2U) << "the zeroing must have arrived as at least one ordinary write";
  EXPECT_EQ(*bytes % kDataRecordSize, 0U) << "only DATA records are expected here";
  EXPECT_GE(*bytes, 6 * kDataRecordSize);
}

TEST(Zero, FastZeroFailsAndChangesNoData) {
  Harness harness;
  ASSERT_TRUE(harness.start(harness.default_settings("1M"))) << harness.stop().stderr_text;
  NbdHandle nbd = harness.connect();
  ASSERT_TRUE(nbd);
  nbd_handle* h = nbd.get();

  const Bytes data(kBlockSize, 0x77);
  ASSERT_EQ(nbd_pwrite(h, data.data(), data.size(), 0, 0), 0) << nbd_get_error();

  // The server must advertise fast-zero support for the flag to be sendable at all;
  // if it does not, libnbd itself refuses the call, which is also "fails, no change".
  const int rc = nbd_zero(h, 0, kBlockSize, LIBNBD_CMD_FLAG_FAST_ZERO);
  EXPECT_EQ(rc, -1) << "a fast zero must fail: a fallback to writes is not fast";

  Bytes back(kBlockSize);
  ASSERT_EQ(nbd_pread(h, back.data(), back.size(), 0, 0), 0) << nbd_get_error();
  EXPECT_EQ(back, data) << "a failed fast zero must not change the data";
}

}  // namespace
}  // namespace logblock::itest
