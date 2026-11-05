// Tests for Engine::read and Engine::write: the engine must behave
// like a disk at any byte offset and length. Uses FaultIo, so no real file is touched.
// Errors, the cap and failed I/O are covered by later tasks.
#include <gtest/gtest.h>

#include <cstdint>

#include "logblock/engine.h"
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

class EngineReadWriteTest : public test::EngineTestBase {};

// --- Reading what was never written -------------------------------------------

TEST_F(EngineReadWriteTest, ANeverWrittenBlockReadsAsZeros) {
  EXPECT_TRUE(SameBytes(read(7 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineReadWriteTest, ReadingNeverWrittenDataReadsNothingFromTheLog) {
  (void)read(0, kBlockSize);
  EXPECT_EQ(io_->count(Op::Pread), 0U);
}

TEST_F(EngineReadWriteTest, AnUnalignedReadOfNeverWrittenDataIsZeros) {
  EXPECT_TRUE(SameBytes(read(1000, 20000), zeros(20000)));
}

// --- Whole blocks -------------------------------------------------------------

TEST_F(EngineReadWriteTest, AWrittenBlockReadsBack) {
  const Bytes data = pattern(1, kBlockSize);
  ASSERT_TRUE(write(2 * kBlockSize, data).ok());
  EXPECT_TRUE(SameBytes(read(2 * kBlockSize, kBlockSize), data));
}

TEST_F(EngineReadWriteTest, WritingOneBlockLeavesItsNeighboursUntouched) {
  ASSERT_TRUE(write(2 * kBlockSize, pattern(1, kBlockSize)).ok());
  EXPECT_TRUE(SameBytes(read(1 * kBlockSize, kBlockSize), zeros(kBlockSize)));
  EXPECT_TRUE(SameBytes(read(3 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineReadWriteTest, AnOverwriteReturnsTheSecondWritesData) {
  ASSERT_TRUE(write(kBlockSize, pattern(1, kBlockSize)).ok());
  const Bytes second = pattern(2, kBlockSize);
  ASSERT_TRUE(write(kBlockSize, second).ok());
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), second));
}

TEST_F(EngineReadWriteTest, AnAlignedMultiBlockWriteReadsBackBlockByBlock) {
  const Bytes data = pattern(3, 3 * kBlockSize);
  ASSERT_TRUE(write(4 * kBlockSize, data).ok());
  EXPECT_TRUE(SameBytes(read(4 * kBlockSize, 3 * kBlockSize), data));
  EXPECT_TRUE(SameBytes(read(5 * kBlockSize, kBlockSize), slice(data, kBlockSize, kBlockSize)));
}

TEST_F(EngineReadWriteTest, OnlyPartlyCoveredBlocksReadTheirOldContents) {
  ASSERT_TRUE(write(0, pattern(1, 3 * kBlockSize)).ok());
  io_->clear_calls();

  // Fully covers block 1: the old record is not needed, so nothing is read.
  ASSERT_TRUE(write(kBlockSize, pattern(2, kBlockSize)).ok());
  EXPECT_EQ(io_->count(Op::Pread), 0U);

  // Partly covers blocks 0 and 2 (the middle block is fully covered): two old reads.
  ASSERT_TRUE(write(1000, pattern(3, 2 * kBlockSize)).ok());
  EXPECT_EQ(io_->count(Op::Pread), 2U);
}

// --- Partial blocks -----------------------------------------------------------

TEST_F(EngineReadWriteTest, APartialWriteToANeverWrittenBlockKeepsTheRestZero) {
  const Bytes data = pattern(4, 100);
  ASSERT_TRUE(write(5 * kBlockSize + 10, data).ok());
  EXPECT_TRUE(SameBytes(read(5 * kBlockSize, kBlockSize),
                        join({zeros(10), data, zeros(kBlockSize - 110)})));
  // The blocks around it are not affected.
  EXPECT_TRUE(SameBytes(read(4 * kBlockSize, kBlockSize), zeros(kBlockSize)));
  EXPECT_TRUE(SameBytes(read(6 * kBlockSize, kBlockSize), zeros(kBlockSize)));
}

TEST_F(EngineReadWriteTest, APartialWriteToAWrittenBlockKeepsTheOldBytesAroundIt) {
  const Bytes old_block = pattern(5, kBlockSize);
  ASSERT_TRUE(write(3 * kBlockSize, old_block).ok());
  const Bytes patch = pattern(6, 50);
  ASSERT_TRUE(write(3 * kBlockSize + 1000, patch).ok());
  EXPECT_TRUE(SameBytes(
      read(3 * kBlockSize, kBlockSize),
      join({slice(old_block, 0, 1000), patch, slice(old_block, 1050, kBlockSize - 1050)})));
}

TEST_F(EngineReadWriteTest, AOneByteWriteChangesOnlyThatByte) {
  const Bytes old_block = pattern(7, kBlockSize);
  ASSERT_TRUE(write(0, old_block).ok());
  ASSERT_TRUE(write(2047, Bytes{0xEE}).ok());
  Bytes want = old_block;
  want[2047] = 0xEE;
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), want));
}

// --- Ranges that are not block aligned ----------------------------------------

TEST_F(EngineReadWriteTest, AnUnalignedMultiBlockWriteLeavesBytesOutsideItUnchanged) {
  // Four blocks of known data, then 10,000 bytes written at 3,000: that starts
  // inside block 0, covers all of blocks 1 and 2, and ends inside block 3.
  const Bytes base = pattern(8, 4 * kBlockSize);
  ASSERT_TRUE(write(0, base).ok());
  const Bytes patch = pattern(9, 10000);
  ASSERT_TRUE(write(3000, patch).ok());

  const Bytes want =
      join({slice(base, 0, 3000), patch, slice(base, 13000, 4 * kBlockSize - 13000)});
  EXPECT_TRUE(SameBytes(read(0, 4 * kBlockSize), want));
}

TEST_F(EngineReadWriteTest, AnUnalignedMultiBlockWriteOverNeverWrittenBlocksKeepsZeros) {
  const Bytes patch = pattern(10, 10000);
  ASSERT_TRUE(write(3000, patch).ok());
  const Bytes want = join({zeros(3000), patch, zeros(4 * kBlockSize - 13000)});
  EXPECT_TRUE(SameBytes(read(0, 4 * kBlockSize), want));
}

TEST_F(EngineReadWriteTest, AnUnalignedReadReturnsExactlyTheRequestedBytes) {
  const Bytes data = pattern(11, 4 * kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  EXPECT_TRUE(SameBytes(read(2500, 9000), slice(data, 2500, 9000)));
  EXPECT_TRUE(SameBytes(read(4095, 2), slice(data, 4095, 2)));  // across a block boundary
  EXPECT_TRUE(SameBytes(read(4096, 1), slice(data, 4096, 1)));
}

TEST_F(EngineReadWriteTest, AnUnalignedReadAcrossWrittenAndNeverWrittenBlocks) {
  const Bytes data = pattern(12, kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  EXPECT_TRUE(SameBytes(read(4000, 200), join({slice(data, 4000, 96), zeros(104)})));
}

// --- The end of the device ----------------------------------------------------

TEST_F(EngineReadWriteTest, TheLastBlockOfTheDeviceWorks) {
  const Bytes data = pattern(13, kBlockSize);
  ASSERT_TRUE(write(kDeviceSize - kBlockSize, data).ok());
  EXPECT_TRUE(SameBytes(read(kDeviceSize - kBlockSize, kBlockSize), data));
}

TEST_F(EngineReadWriteTest, TheLastByteOfTheDeviceWorks) {
  ASSERT_TRUE(write(kDeviceSize - 1, Bytes{0x5A}).ok());
  EXPECT_TRUE(SameBytes(read(kDeviceSize - 1, 1), Bytes{0x5A}));
  // The rest of that block is still zero.
  EXPECT_TRUE(SameBytes(read(kDeviceSize - kBlockSize, kBlockSize - 1), zeros(kBlockSize - 1)));
}

TEST_F(EngineReadWriteTest, AWriteEndingExactlyAtTheDeviceEndWorks) {
  // 5,000 bytes ending at the last byte: the last block fully, and the end of the one before.
  const Bytes data = pattern(14, 5000);
  ASSERT_TRUE(write(kDeviceSize - 5000, data).ok());
  EXPECT_TRUE(SameBytes(read(kDeviceSize - 5000, 5000), data));
}

TEST_F(EngineReadWriteTest, TheFirstBlockOfTheDeviceWorks) {
  const Bytes data = pattern(15, kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), data));
}

// --- Zero-length requests ------------------------------------------------

TEST_F(EngineReadWriteTest, ZeroLengthReadsAndWritesSucceedAndChangeNothing) {
  const Bytes before = pattern(16, kBlockSize);
  ASSERT_TRUE(write(0, before).ok());
  const uint64_t logged = engine_->bytes_logged();
  io_->clear_calls();

  Bytes buf(8, 0xAA);
  // 0, an unaligned offset, a block boundary, and offset == size (allowed).
  for (const uint64_t offset : {uint64_t{0}, uint64_t{12345}, 2 * kBlockSize, kDeviceSize}) {
    EXPECT_TRUE(engine_->write(offset, 0, buf.data()).ok()) << "write at " << offset;
    EXPECT_TRUE(engine_->read(offset, 0, buf.data()).ok()) << "read at " << offset;
  }

  EXPECT_EQ(buf, Bytes(8, 0xAA)) << "a zero-length read must not touch the buffer";
  EXPECT_EQ(engine_->bytes_logged(), logged);
  EXPECT_TRUE(io_->calls().empty()) << "zero-length requests must not call the Io at all";
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), before));
}

// --- bytes_logged -------------------------------------------------------------

TEST_F(EngineReadWriteTest, BytesLoggedGrowsByOneDataRecordPerWholeBlockWritten) {
  ASSERT_TRUE(write(0, pattern(1, 3 * kBlockSize)).ok());
  EXPECT_EQ(engine_->bytes_logged(), 3 * kDataRecordSize);
}

TEST_F(EngineReadWriteTest, BytesLoggedGrowsByAWholeRecordForAPartialWrite) {
  ASSERT_TRUE(write(kBlockSize + 5, pattern(1, 10)).ok());
  EXPECT_EQ(engine_->bytes_logged(), 1 * kDataRecordSize);
}

TEST_F(EngineReadWriteTest, BytesLoggedCountsEveryTouchedBlockOfAnUnalignedWrite) {
  // 10,000 bytes at 3,000 end at byte 12,999: the write starts in block 0 and ends in
  // block 3, so it touches 4 blocks.
  ASSERT_TRUE(write(3000, pattern(1, 10000)).ok());
  EXPECT_EQ(engine_->bytes_logged(), 4 * kDataRecordSize);
}

TEST_F(EngineReadWriteTest, ATwelveByteWriteAcrossABlockBoundaryTouchesTwoBlocks) {
  ASSERT_TRUE(write(kBlockSize - 6, pattern(1, 12)).ok());
  EXPECT_EQ(engine_->bytes_logged(), 2 * kDataRecordSize);
}

TEST_F(EngineReadWriteTest, BytesLoggedMatchesTheFileAndTheNumberOfWrites) {
  ASSERT_TRUE(write(0, pattern(1, 2 * kBlockSize)).ok());
  ASSERT_TRUE(write(100, pattern(2, 10)).ok());  // partial, one record
  const uint64_t records = 3;
  EXPECT_EQ(engine_->bytes_logged(), records * kDataRecordSize);
  EXPECT_EQ(io_->bytes().size(), records * kDataRecordSize);
  EXPECT_EQ(io_->count(Op::Pwrite), records) << "one pwrite per record";
}

TEST_F(EngineReadWriteTest, ReadsNeverChangeBytesLogged) {
  ASSERT_TRUE(write(0, pattern(1, kBlockSize)).ok());
  const uint64_t logged = engine_->bytes_logged();
  (void)read(0, kBlockSize);
  (void)read(100, 5000);
  EXPECT_EQ(engine_->bytes_logged(), logged);
  EXPECT_EQ(io_->count(Op::Pwrite), 1U);
}

}  // namespace
}  // namespace logblock
