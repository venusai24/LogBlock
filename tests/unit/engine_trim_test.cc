// Tests for Engine::trim. A trim logs one header-only TRIM record
// for the blocks it covers completely; blocks it covers only partly are kept.
// Errors, the cap and failed I/O are covered by later tasks.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "logblock/engine.h"
#include "record.h"
#include "support/engine_test_base.h"

namespace logblock {
namespace {

using test::Bytes;
using test::join;
using test::pattern;
using test::SameBytes;
using test::slice;
using test::zeros;
using Op = test::FaultIo::Op;

class EngineTrimTest : public test::EngineTestBase {
 protected:
  Status trim(uint64_t offset, uint64_t len) { return engine_->trim(offset, len); }

  // Decodes the record that starts at `offset` in the fake log file.
  Record decode_at(uint64_t offset) {
    const std::string& file = io_->bytes();
    Record record;
    Status s = decode_record(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(file.data()) + offset,
                                 file.size() - offset),
        &record);
    EXPECT_TRUE(s.ok()) << s.message();
    return record;
  }
};

// --- Whole blocks -------------------------------------------------------------

TEST_F(EngineTrimTest, ATrimmedBlockReadsAsZeros) {
  ASSERT_TRUE(write(3 * kBlockSize, pattern(1, kBlockSize)).ok());
  ASSERT_TRUE(trim(3 * kBlockSize, kBlockSize).ok());
  EXPECT_TRUE(SameBytes(read(3 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineTrimTest, TrimmingOneBlockLeavesItsNeighboursUntouched) {
  const Bytes data = pattern(2, 5 * kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  ASSERT_TRUE(trim(2 * kBlockSize, kBlockSize).ok());
  EXPECT_TRUE(
      SameBytes(read(0, 5 * kBlockSize), join({slice(data, 0, 2 * kBlockSize), zeros(kBlockSize),
                                               slice(data, 3 * kBlockSize, 2 * kBlockSize)})));
}

TEST_F(EngineTrimTest, ATrimIsOneTrimRecordOf40Bytes) {
  ASSERT_TRUE(write(0, pattern(3, 4 * kBlockSize)).ok());
  const uint64_t before = engine_->bytes_logged();
  io_->clear_calls();

  ASSERT_TRUE(trim(kBlockSize, 2 * kBlockSize).ok());  // blocks 1 and 2

  EXPECT_EQ(engine_->bytes_logged(), before + kTrimRecordSize);
  EXPECT_EQ(io_->bytes().size(), before + kTrimRecordSize);
  EXPECT_EQ(io_->count(Op::Pwrite), 1U) << "one pwrite for the whole TRIM record";

  const Record record = decode_at(before);
  EXPECT_EQ(record.header.type, RecordType::Trim);
  EXPECT_EQ(record.header.block_no, 1U);
  EXPECT_EQ(record.header.block_count, 2U);
}

TEST_F(EngineTrimTest, WritingAfterATrimStoresTheNewData) {
  ASSERT_TRUE(write(0, pattern(4, kBlockSize)).ok());
  ASSERT_TRUE(trim(0, kBlockSize).ok());
  const Bytes again = pattern(5, kBlockSize);
  ASSERT_TRUE(write(0, again).ok());
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), again));
}

TEST_F(EngineTrimTest, APartialWriteAfterATrimKeepsZerosAroundIt) {
  ASSERT_TRUE(write(0, pattern(6, kBlockSize)).ok());
  ASSERT_TRUE(trim(0, kBlockSize).ok());
  const Bytes patch = pattern(7, 20);
  ASSERT_TRUE(write(100, patch).ok());
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), join({zeros(100), patch, zeros(kBlockSize - 120)})));
}

TEST_F(EngineTrimTest, TheLastBlockOfTheDeviceCanBeTrimmed) {
  ASSERT_TRUE(write(kDeviceSize - kBlockSize, pattern(8, kBlockSize)).ok());
  ASSERT_TRUE(trim(kDeviceSize - kBlockSize, kBlockSize).ok());
  EXPECT_TRUE(SameBytes(read(kDeviceSize - kBlockSize, kBlockSize), zeros(kBlockSize)));
}

// --- The whole device ---------------------------------------------------------

TEST_F(EngineTrimTest, AWholeDeviceTrimIsOneTrimRecordAndEveryBlockReadsAsZeros) {
  ASSERT_TRUE(write(0, pattern(9, kBlockSize)).ok());
  ASSERT_TRUE(write(77 * kBlockSize, pattern(10, kBlockSize)).ok());
  ASSERT_TRUE(write(kDeviceSize - kBlockSize, pattern(11, kBlockSize)).ok());
  const uint64_t before = engine_->bytes_logged();

  ASSERT_TRUE(trim(0, kDeviceSize).ok());

  EXPECT_EQ(engine_->bytes_logged(), before + 40) << "exactly one 40-byte record";
  const Record record = decode_at(before);
  EXPECT_EQ(record.header.type, RecordType::Trim);
  EXPECT_EQ(record.header.block_no, 0U);
  EXPECT_EQ(record.header.block_count, kBlockCount);
  EXPECT_EQ(io_->bytes().size(), before + 40) << "and nothing after it";

  // Read the whole device in 1 MiB pieces and check that it is all zeros.
  constexpr uint64_t kPiece = uint64_t{1} << 20;
  const Bytes zero_piece = zeros(kPiece);
  for (uint64_t offset = 0; offset < kDeviceSize; offset += kPiece) {
    ASSERT_TRUE(SameBytes(read(offset, kPiece), zero_piece)) << "at offset " << offset;
  }
}

// --- Partial trims ------------------------------------------------------------

TEST_F(EngineTrimTest, APartialTrimLogsOnlyTheFullyCoveredBlocksAndKeepsBothEdges) {
  // Blocks 2..6 hold data. The trim runs from the middle of block 2 to the middle
  // of block 6, so it covers blocks 3, 4 and 5 completely and 2 and 6 only partly.
  const Bytes data = pattern(12, 5 * kBlockSize);
  ASSERT_TRUE(write(2 * kBlockSize, data).ok());
  const uint64_t before = engine_->bytes_logged();

  ASSERT_TRUE(trim(2 * kBlockSize + 100, 4 * kBlockSize).ok());

  const Record record = decode_at(before);
  EXPECT_EQ(record.header.type, RecordType::Trim);
  EXPECT_EQ(record.header.block_no, 3U);
  EXPECT_EQ(record.header.block_count, 3U);
  EXPECT_EQ(engine_->bytes_logged(), before + kTrimRecordSize);

  // Blocks 2 and 6 keep every byte, even the ones inside the trimmed range.
  EXPECT_TRUE(SameBytes(read(2 * kBlockSize, 5 * kBlockSize),
                        join({slice(data, 0, kBlockSize), zeros(3 * kBlockSize),
                              slice(data, 4 * kBlockSize, kBlockSize)})));
}

TEST_F(EngineTrimTest, ATrimStartingMidBlockAndEndingOnABoundaryRoundsTheStartUp) {
  const Bytes data = pattern(13, 3 * kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  const uint64_t before = engine_->bytes_logged();

  ASSERT_TRUE(trim(1, 3 * kBlockSize - 1).ok());  // from byte 1 to the end of block 2

  const Record record = decode_at(before);
  EXPECT_EQ(record.header.block_no, 1U);
  EXPECT_EQ(record.header.block_count, 2U);
  EXPECT_TRUE(SameBytes(read(0, 3 * kBlockSize),
                        join({slice(data, 0, kBlockSize), zeros(2 * kBlockSize)})));
}

TEST_F(EngineTrimTest, ATrimStartingOnABoundaryAndEndingMidBlockRoundsTheEndDown) {
  const Bytes data = pattern(14, 3 * kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  const uint64_t before = engine_->bytes_logged();

  ASSERT_TRUE(trim(0, 2 * kBlockSize + kBlockSize - 1).ok());  // blocks 0 and 1 fully

  const Record record = decode_at(before);
  EXPECT_EQ(record.header.block_no, 0U);
  EXPECT_EQ(record.header.block_count, 2U);
  EXPECT_TRUE(SameBytes(read(0, 3 * kBlockSize),
                        join({zeros(2 * kBlockSize), slice(data, 2 * kBlockSize, kBlockSize)})));
}

// --- Trims that cover no full block -------------------------------------------

TEST_F(EngineTrimTest, ATrimCoveringNoFullBlockSucceedsAndAppendsNothing) {
  const Bytes data = pattern(15, 2 * kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  const uint64_t before = engine_->bytes_logged();
  io_->clear_calls();

  EXPECT_TRUE(trim(100, 200).ok());               // inside one block
  EXPECT_TRUE(trim(kBlockSize - 100, 200).ok());  // across a boundary, no full block
  EXPECT_TRUE(trim(0, kBlockSize - 1).ok());      // one byte short of block 0
  EXPECT_TRUE(trim(1, kBlockSize).ok());          // one byte late for block 0
  EXPECT_TRUE(trim(kBlockSize, 0).ok());          // zero length
  EXPECT_TRUE(trim(kDeviceSize, 0).ok());         // zero length at the end

  EXPECT_EQ(engine_->bytes_logged(), before);
  EXPECT_TRUE(io_->calls().empty()) << "a trim that logs nothing must not call the Io";
  EXPECT_TRUE(SameBytes(read(0, 2 * kBlockSize), data));
}

// --- Trims are always logged -----------------------------------------

TEST_F(EngineTrimTest, TrimmingANeverWrittenBlockStillAppendsARecordAndReadsZeros) {
  ASSERT_TRUE(trim(9 * kBlockSize, kBlockSize).ok());
  EXPECT_EQ(engine_->bytes_logged(), kTrimRecordSize);
  EXPECT_TRUE(SameBytes(read(9 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineTrimTest, TrimmingTheSameBlockTwiceAppendsARecordEachTime) {
  ASSERT_TRUE(write(0, pattern(16, kBlockSize)).ok());
  const uint64_t before = engine_->bytes_logged();
  ASSERT_TRUE(trim(0, kBlockSize).ok());
  ASSERT_TRUE(trim(0, kBlockSize).ok());
  EXPECT_EQ(engine_->bytes_logged(), before + 2 * kTrimRecordSize);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineTrimTest, EachTrimRecordGetsTheNextSequenceNumber) {
  ASSERT_TRUE(write(0, pattern(17, kBlockSize)).ok());  // seq 1
  const uint64_t first = engine_->bytes_logged();
  ASSERT_TRUE(trim(0, kBlockSize).ok());  // seq 2
  const uint64_t second = engine_->bytes_logged();
  ASSERT_TRUE(trim(0, kBlockSize).ok());  // seq 3
  EXPECT_EQ(decode_at(first).header.seq, 2U);
  EXPECT_EQ(decode_at(second).header.seq, 3U);
}

// --- Randomized check against a model -----------------------------------------

// A plain byte image of a small region of the device. Every engine call is applied
// to it in the simplest possible way, and every read must agree with it.
struct Model {
  uint64_t base;  // where the region starts on the device
  Bytes image;    // the region's expected contents

  void write(uint64_t offset, const Bytes& data) {
    std::copy(data.begin(), data.end(), image.begin() + static_cast<std::ptrdiff_t>(offset - base));
  }
  // Zeroes the blocks that lie completely inside [offset, offset + len).
  void trim(uint64_t offset, uint64_t len) {
    for (uint64_t block_start = (offset / kBlockSize) * kBlockSize;
         block_start + kBlockSize <= offset + len; block_start += kBlockSize) {
      if (block_start >= offset) {
        std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(block_start - base), kBlockSize,
                    uint8_t{0});
      }
    }
  }
  [[nodiscard]] Bytes read(uint64_t offset, uint64_t len) const {
    return slice(image, offset - base, len);
  }
};

TEST_F(EngineTrimTest, RandomWritesReadsAndTrimsMatchAModel) {
  constexpr uint64_t kRegionBlocks = 24;
  constexpr uint64_t kRegionBytes = kRegionBlocks * kBlockSize;
  // One region at the start of the device and one at its end.
  std::vector<Model> regions = {{0, zeros(kRegionBytes)},
                                {kDeviceSize - kRegionBytes, zeros(kRegionBytes)}};

  std::mt19937_64 rng(20240607);                  // fixed seed: a failure reproduces exactly
  auto pick = [&rng](uint64_t lo, uint64_t hi) {  // uniform in [lo, hi]
    return std::uniform_int_distribution<uint64_t>(lo, hi)(rng);
  };

  for (int step = 0; step < 800; ++step) {
    Model& region = regions[pick(0, 1)];
    // A random range inside the region, from 1 byte to 4 blocks long.
    const uint64_t len = pick(1, std::min<uint64_t>(4 * kBlockSize, kRegionBytes));
    const uint64_t offset = region.base + pick(0, kRegionBytes - len);

    const uint64_t op = pick(0, 2);
    if (op == 0) {
      const Bytes data = pattern(static_cast<uint8_t>(step), len);
      ASSERT_TRUE(write(offset, data).ok()) << "step " << step;
      region.write(offset, data);
    } else if (op == 1) {
      ASSERT_TRUE(trim(offset, len).ok()) << "step " << step;
      region.trim(offset, len);
    } else {
      ASSERT_TRUE(SameBytes(read(offset, len), region.read(offset, len)))
          << "read at step " << step << " (offset " << offset << ", length " << len << ")";
    }
  }

  // At the end, both whole regions must agree.
  for (const Model& region : regions) {
    EXPECT_TRUE(SameBytes(read(region.base, kRegionBytes), region.image))
        << "region at " << region.base;
  }
}

}  // namespace
}  // namespace logblock
