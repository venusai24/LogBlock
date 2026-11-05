// Tests for the range checks of Engine::read, write and trim: a request outside the device returns
// OutOfRange before anything else happens: no log reads, no appends, no change to any block, and
// the engine keeps serving valid requests afterwards.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "logblock/engine.h"
#include "support/engine_test_base.h"

namespace logblock {
namespace {

using test::Bytes;
using test::pattern;
using test::SameBytes;
using test::zeros;

constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

struct BadRange {
  const char* why;
  uint64_t offset;
  uint64_t len;
};

// Every one of these must be rejected by read, by write and by trim.
const std::vector<BadRange>& bad_ranges() {
  static const std::vector<BadRange> ranges = {
      // Starts inside the device and runs past its end.
      {"last byte plus one", kDeviceSize - 1, 2},
      {"last block plus one byte", kDeviceSize - kBlockSize, kBlockSize + 1},
      {"whole device plus one byte", 0, kDeviceSize + 1},
      // Starts at or past the end, with a non-zero length.
      {"starts at the end", kDeviceSize, 1},
      {"starts one byte past the end", kDeviceSize + 1, 1},
      {"starts a block past the end", kDeviceSize + kBlockSize, kBlockSize},
      // offset + len overflows 64 bits and wraps to a small number.
      {"offset UINT64_MAX, len 1", kMax, 1},
      {"offset UINT64_MAX, len 2 (wraps to 1)", kMax, 2},
      {"offset UINT64_MAX - 10, len 100 (wraps to 89)", kMax - 10, 100},
      {"offset 1, len UINT64_MAX (wraps to 0)", 1, kMax},
      {"offset 0, len UINT64_MAX", 0, kMax},
      {"offset 4096, len UINT64_MAX - 4095 (wraps to 0)", kBlockSize, kMax - kBlockSize + 1},
      // Zero length, but the offset itself is outside the device.
      {"zero length one byte past the end", kDeviceSize + 1, 0},
      {"zero length at UINT64_MAX", kMax, 0},
  };
  return ranges;
}

class EngineRangeTest : public test::EngineTestBase {
 protected:
  // Puts known data in a few blocks, including both ends of the device, so a request
  // that wrongly changes a block (or wraps around to block 0) is noticed.
  void SetUp() override {
    test::EngineTestBase::SetUp();
    ASSERT_TRUE(write(0, pattern(1, kBlockSize)).ok());
    ASSERT_TRUE(write(5 * kBlockSize, pattern(2, kBlockSize)).ok());
    ASSERT_TRUE(write(kDeviceSize - kBlockSize, pattern(3, kBlockSize)).ok());
    logged_ = engine_->bytes_logged();
    io_->clear_calls();
  }

  // The three blocks as they were written in SetUp.
  void ExpectBlocksUnchanged() {
    EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
    EXPECT_TRUE(SameBytes(read(5 * kBlockSize, kBlockSize), pattern(2, kBlockSize)));
    EXPECT_TRUE(SameBytes(read(kDeviceSize - kBlockSize, kBlockSize), pattern(3, kBlockSize)));
    EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), zeros(kBlockSize)));
  }

  uint64_t logged_ = 0;
};

TEST_F(EngineRangeTest, ReadRejectsEveryBadRangeAndLeavesTheBufferAlone) {
  for (const BadRange& r : bad_ranges()) {
    SCOPED_TRACE(r.why);
    // The buffer is tiny on purpose: a rejected read must not write to it at all,
    // whatever `len` says. (ASan reports it if the engine does.)
    Bytes buf(16, 0xAA);
    Status s = engine_->read(r.offset, r.len, buf.data());
    EXPECT_EQ(s.code(), StatusCode::OutOfRange) << s.to_string();
    EXPECT_EQ(buf, Bytes(16, 0xAA));
  }
  EXPECT_TRUE(io_->calls().empty()) << "a rejected read must not touch the Io";
  EXPECT_EQ(engine_->bytes_logged(), logged_);
}

TEST_F(EngineRangeTest, WriteRejectsEveryBadRangeAndAppendsNothing) {
  for (const BadRange& r : bad_ranges()) {
    SCOPED_TRACE(r.why);
    const Bytes buf(16, 0x77);  // tiny: a rejected write must not read `len` bytes from it
    Status s = engine_->write(r.offset, r.len, buf.data());
    EXPECT_EQ(s.code(), StatusCode::OutOfRange) << s.to_string();
  }
  EXPECT_TRUE(io_->calls().empty()) << "a rejected write must not read or append";
  EXPECT_EQ(engine_->bytes_logged(), logged_);
  ExpectBlocksUnchanged();
}

TEST_F(EngineRangeTest, TrimRejectsEveryBadRangeAndAppendsNothing) {
  for (const BadRange& r : bad_ranges()) {
    SCOPED_TRACE(r.why);
    Status s = engine_->trim(r.offset, r.len);
    EXPECT_EQ(s.code(), StatusCode::OutOfRange) << s.to_string();
  }
  EXPECT_TRUE(io_->calls().empty()) << "a rejected trim must not append";
  EXPECT_EQ(engine_->bytes_logged(), logged_);
  ExpectBlocksUnchanged();
}

TEST_F(EngineRangeTest, TheMessageNamesTheRequest) {
  Bytes buf(16);
  Status s = engine_->read(kDeviceSize + 1, 1, buf.data());
  ASSERT_EQ(s.code(), StatusCode::OutOfRange);
  EXPECT_NE(s.message().find(std::to_string(kDeviceSize + 1)), std::string::npos)
      << "the message should show the offending offset: " << s.message();
}

TEST_F(EngineRangeTest, TheEngineKeepsServingAfterRejectedRequests) {
  Bytes buf(16);
  for (const BadRange& r : bad_ranges()) {
    EXPECT_EQ(engine_->read(r.offset, r.len, buf.data()).code(), StatusCode::OutOfRange);
    EXPECT_EQ(engine_->write(r.offset, r.len, buf.data()).code(), StatusCode::OutOfRange);
    EXPECT_EQ(engine_->trim(r.offset, r.len).code(), StatusCode::OutOfRange);
  }

  // A valid read, write and trim all still work.
  const Bytes fresh = pattern(9, kBlockSize);
  EXPECT_TRUE(write(2 * kBlockSize, fresh).ok());
  EXPECT_TRUE(SameBytes(read(2 * kBlockSize, kBlockSize), fresh));
  EXPECT_TRUE(engine_->trim(2 * kBlockSize, kBlockSize).ok());
  EXPECT_TRUE(SameBytes(read(2 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

// --- Requests that end exactly at the device end are valid -----------------------

TEST_F(EngineRangeTest, RangesEndingExactlyAtTheDeviceEndAreAccepted) {
  Bytes buf(4, 0);
  EXPECT_TRUE(engine_->read(kDeviceSize - 4, 4, buf.data()).ok());
  EXPECT_TRUE(engine_->write(kDeviceSize - 4, 4, buf.data()).ok());
  EXPECT_TRUE(engine_->trim(kDeviceSize - kBlockSize, kBlockSize).ok());
  EXPECT_TRUE(engine_->trim(0, kDeviceSize).ok());
}

TEST_F(EngineRangeTest, ZeroLengthAtExactlyTheDeviceSizeIsAccepted) {
  Bytes buf(4, 0);
  EXPECT_TRUE(engine_->read(kDeviceSize, 0, buf.data()).ok());
  EXPECT_TRUE(engine_->write(kDeviceSize, 0, buf.data()).ok());
  EXPECT_TRUE(engine_->trim(kDeviceSize, 0).ok());
}

}  // namespace
}  // namespace logblock
