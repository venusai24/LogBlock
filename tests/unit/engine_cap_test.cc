// Tests for the log size cap. Every DATA record
// costs 4,136 bytes of the cap and every TRIM record 40, whatever the request looked
// like. A request that does not fit is rejected whole, before anything is read or
// appended, and the engine keeps serving.
#include <gtest/gtest.h>

#include <cstdint>

#include "logblock/engine.h"
#include "support/engine_test_base.h"

namespace logblock {
namespace {

using test::Bytes;
using test::pattern;
using test::SameBytes;
using test::zeros;
using Op = test::FaultIo::Op;

class EngineCapTest : public test::EngineTestBase {
 protected:
  // Writes a whole block with data derived from `seed`.
  Status write_block(uint64_t block, uint8_t seed) {
    return write(block * kBlockSize, pattern(seed, kBlockSize));
  }

  // The fake log file must never be bigger than the cap.
  void ExpectFileWithin(uint64_t cap) {
    EXPECT_LE(io_->bytes().size(), cap) << "the log file must never exceed the cap";
  }
};

// --- DATA records ------------------------------------------------------------

TEST_F(EngineCapTest, ACapOfExactlyNRecordsFitsNAndRejectsTheNextWrite) {
  make_engine(3 * kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write_block(1, 2).ok());
  ASSERT_TRUE(write_block(2, 3).ok());
  EXPECT_EQ(engine_->bytes_logged(), 3 * kDataRecordSize);

  Status s = write_block(3, 4);
  EXPECT_EQ(s.code(), StatusCode::NoSpace) << s.to_string();
  EXPECT_EQ(engine_->bytes_logged(), 3 * kDataRecordSize);
  ExpectFileWithin(3 * kDataRecordSize);
}

TEST_F(EngineCapTest, ACapOneByteShortOfNRecordsFitsOnlyNMinusOne) {
  make_engine(3 * kDataRecordSize - 1);
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write_block(1, 2).ok());
  EXPECT_EQ(write_block(2, 3).code(), StatusCode::NoSpace);
  EXPECT_EQ(engine_->bytes_logged(), 2 * kDataRecordSize);
  ExpectFileWithin(3 * kDataRecordSize - 1);
}

TEST_F(EngineCapTest, ACapOfOneRecordFitsExactlyOneWrite) {
  make_engine(kDataRecordSize);  // the smallest cap create() accepts
  EXPECT_TRUE(write_block(0, 1).ok());
  EXPECT_EQ(write_block(1, 2).code(), StatusCode::NoSpace);
}

TEST_F(EngineCapTest, APartialWriteCostsAWholeRecord) {
  make_engine(kDataRecordSize);
  EXPECT_TRUE(write(100, pattern(1, 10)).ok());  // 10 bytes, but a 4,136-byte record
  EXPECT_EQ(engine_->bytes_logged(), kDataRecordSize);
  EXPECT_EQ(write(200, pattern(2, 10)).code(), StatusCode::NoSpace);
}

TEST_F(EngineCapTest, OverwritesOnAFullLogReturnNoSpaceAndKeepTheOldData) {
  make_engine(2 * kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write_block(1, 2).ok());

  EXPECT_EQ(write_block(0, 9).code(), StatusCode::NoSpace);
  EXPECT_EQ(write(10, pattern(8, 5)).code(), StatusCode::NoSpace);  // partial overwrite too
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), pattern(2, kBlockSize)));
}

// --- A multi-block write is all or nothing -----------------------------------

TEST_F(EngineCapTest, AMultiBlockWriteThatDoesNotFitIsRejectedWholeWithoutAnyIo) {
  make_engine(4 * kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write_block(1, 2).ok());
  ASSERT_TRUE(write_block(2, 3).ok());  // room for exactly one more record
  io_->clear_calls();

  // An unaligned write over blocks 0..2 needs 3 records, and its edge blocks are
  // mapped, so it would have to read them. It must be rejected before any of that.
  Status s = write(100, pattern(9, 2 * kBlockSize));
  EXPECT_EQ(s.code(), StatusCode::NoSpace) << s.to_string();
  EXPECT_TRUE(io_->calls().empty()) << "no pread and no pwrite for a request that cannot fit";
  EXPECT_EQ(engine_->bytes_logged(), 3 * kDataRecordSize);

  // Nothing of it was applied, not even the first block.
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), pattern(2, kBlockSize)));
  // The one record that still fits can be used.
  EXPECT_TRUE(write_block(3, 4).ok());
  ExpectFileWithin(4 * kDataRecordSize);
}

TEST_F(EngineCapTest, ARejectedWriteNeedingOneRecordTooManyAppendsNone) {
  make_engine(2 * kDataRecordSize);
  Status s = write(0, pattern(1, 3 * kBlockSize));  // 3 records, room for 2
  EXPECT_EQ(s.code(), StatusCode::NoSpace);
  EXPECT_EQ(engine_->bytes_logged(), 0U);
  EXPECT_TRUE(io_->bytes().empty());
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
}

// --- TRIM records ------------------------------------------------------------

TEST_F(EngineCapTest, ATrimWithExactly40BytesLeftSucceeds) {
  make_engine(kDataRecordSize + kTrimRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  EXPECT_TRUE(engine_->trim(0, kBlockSize).ok());
  EXPECT_EQ(engine_->bytes_logged(), kDataRecordSize + kTrimRecordSize);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), zeros(kBlockSize)));
  ExpectFileWithin(kDataRecordSize + kTrimRecordSize);
}

TEST_F(EngineCapTest, ATrimWith39BytesLeftFailsWhateverRangeItCovers) {
  make_engine(kDataRecordSize + kTrimRecordSize - 1);
  ASSERT_TRUE(write_block(0, 1).ok());

  // One block and the whole device cost the same 40 bytes, and neither fits.
  EXPECT_EQ(engine_->trim(0, kBlockSize).code(), StatusCode::NoSpace);
  EXPECT_EQ(engine_->trim(0, kDeviceSize).code(), StatusCode::NoSpace);
  EXPECT_EQ(engine_->bytes_logged(), kDataRecordSize);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize))) << "the block is kept";
}

TEST_F(EngineCapTest, ATrimOnAFullLogReturnsNoSpace) {
  make_engine(kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  EXPECT_EQ(engine_->trim(0, kBlockSize).code(), StatusCode::NoSpace);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
}

TEST_F(EngineCapTest, ARejectedTrimDoesNoIo) {
  make_engine(kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  io_->clear_calls();
  EXPECT_EQ(engine_->trim(0, kDeviceSize).code(), StatusCode::NoSpace);
  EXPECT_TRUE(io_->calls().empty());
}

TEST_F(EngineCapTest, SeveralTrimsFillTheCapExactlyThenTheNextOneFails) {
  make_engine(kDataRecordSize + 3 * kTrimRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  EXPECT_TRUE(engine_->trim(0, kBlockSize).ok());
  EXPECT_TRUE(engine_->trim(0, kBlockSize).ok());
  EXPECT_TRUE(engine_->trim(0, kBlockSize).ok());
  EXPECT_EQ(engine_->bytes_logged(), kDataRecordSize + 3 * kTrimRecordSize);
  EXPECT_EQ(engine_->trim(0, kBlockSize).code(), StatusCode::NoSpace);
}

// --- Requests that append nothing need no room -------------------------------

TEST_F(EngineCapTest, ReadsStillWorkOnAFullLog) {
  make_engine(2 * kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  ASSERT_TRUE(write_block(1, 2).ok());
  EXPECT_TRUE(SameBytes(read(0, 2 * kBlockSize),
                        test::join({pattern(1, kBlockSize), pattern(2, kBlockSize)})));
  EXPECT_TRUE(SameBytes(read(5 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineCapTest, ZeroLengthWritesAndTrimsCoveringNoFullBlockSucceedOnAFullLog) {
  make_engine(kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  Bytes buf(1, 0);

  EXPECT_TRUE(engine_->write(0, 0, buf.data()).ok());
  EXPECT_TRUE(engine_->trim(kBlockSize, 0).ok());
  EXPECT_TRUE(engine_->trim(100, 200).ok());           // inside one block
  EXPECT_TRUE(engine_->trim(1, kBlockSize).ok());      // no full block
  EXPECT_TRUE(engine_->trim(kBlockSize - 1, 2).ok());  // across a boundary
  EXPECT_EQ(engine_->bytes_logged(), kDataRecordSize);
}

// --- Order of checks ----------------------------------------------------

TEST_F(EngineCapTest, ARangeErrorWinsOverNoSpaceOnAFullLog) {
  make_engine(kDataRecordSize);
  ASSERT_TRUE(write_block(0, 1).ok());
  Bytes buf(16, 0);
  EXPECT_EQ(engine_->write(kDeviceSize, 1, buf.data()).code(), StatusCode::OutOfRange);
  EXPECT_EQ(engine_->trim(kDeviceSize - 1, 2).code(), StatusCode::OutOfRange);
}

// --- NoSpace from the file system --------------------------------------------

TEST_F(EngineCapTest, NoSpaceFromPwriteIsReturnedAndLeavesTheBlocksUnchanged) {
  const Bytes old_block = pattern(1, kBlockSize);
  ASSERT_TRUE(write(0, old_block).ok());

  io_->fail_next(Op::Pwrite, StatusCode::NoSpace);
  Status s = write_block(0, 9);
  EXPECT_EQ(s.code(), StatusCode::NoSpace) << s.to_string();
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), old_block));
}

TEST_F(EngineCapTest, ATornNoSpaceWriteIsStillNoSpaceAndLeavesTheBlocksUnchanged) {
  const Bytes old_block = pattern(1, kBlockSize);
  ASSERT_TRUE(write(0, old_block).ok());
  const uint64_t logged = engine_->bytes_logged();

  io_->fail_next(Op::Pwrite, StatusCode::NoSpace, /*partial_bytes=*/1000);
  EXPECT_EQ(write_block(0, 9).code(), StatusCode::NoSpace);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), old_block));
  EXPECT_EQ(engine_->bytes_logged(), logged) << "bytes of a failed append are not counted";

  // Not fatal: the next write works, and it lands where the failed one started.
  const Bytes newer = pattern(5, kBlockSize);
  EXPECT_TRUE(write(0, newer).ok());
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), newer));
}

TEST_F(EngineCapTest, NoSpaceFromPwriteOnATrimLeavesTheBlockMapped) {
  const Bytes data = pattern(1, kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  io_->fail_next(Op::Pwrite, StatusCode::NoSpace);
  EXPECT_EQ(engine_->trim(0, kBlockSize).code(), StatusCode::NoSpace);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), data));
}

// --- The file never exceeds the cap -----------------------

TEST_F(EngineCapTest, TheLogFileNeverExceedsTheCapWhateverIsRequested) {
  constexpr uint64_t kCap = 5 * kDataRecordSize + 3 * kTrimRecordSize + 17;
  make_engine(kCap);

  // A mixed run, with many more requests than can fit. Count the rejections so the
  // test fails if the cap was never reached and nothing was really checked.
  int rejected = 0;
  auto note = [&rejected](const Status& s) {
    if (s.code() == StatusCode::NoSpace) {
      ++rejected;
    } else {
      EXPECT_TRUE(s.ok()) << s.to_string();
    }
  };
  for (uint64_t i = 0; i < 40; ++i) {
    if (i % 4 == 3) {
      note(engine_->trim((i % 7) * kBlockSize, 2 * kBlockSize));
    } else if (i % 4 == 2) {
      note(write((i % 7) * kBlockSize + 5, pattern(static_cast<uint8_t>(i), 3 * kBlockSize)));
    } else {
      note(write_block(i % 9, static_cast<uint8_t>(i)));
    }
    ASSERT_LE(io_->bytes().size(), kCap) << "after request " << i;
    ASSERT_LE(engine_->bytes_logged(), kCap) << "after request " << i;
  }
  EXPECT_GT(rejected, 0) << "the cap was never reached";
}

}  // namespace
}  // namespace logblock
