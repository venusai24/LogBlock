// Tests for reading damaged records: a read that reaches a record which fails the checks returns
// Corrupt and hands back none of that record's data. A write that needs the old contents of a
// damaged block (a partial write) fails the same way before it appends anything.
// Damage is made by changing bytes of the fake log file through FaultIo.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "logblock/engine.h"
#include "record.h"
#include "support/engine_test_base.h"

namespace logblock {
namespace {

using test::Bytes;
using test::pattern;
using test::SameBytes;
using test::zeros;
using Op = test::FaultIo::Op;

// Where the bytes of a record start, relative to the record's start.
constexpr size_t kPayloadStart = kRecordHeaderSize;

class EngineCorruptTest : public test::EngineTestBase {
 protected:
  // The contents written into `block` by write_blocks().
  static Bytes contents(uint64_t block) {
    return pattern(static_cast<uint8_t>(block + 1), kBlockSize);
  }

  // Writes blocks 0 .. count-1 one after another, each with its own contents. Every
  // write is a whole block and the log starts empty, so the record of block b starts
  // at b * kDataRecordSize.
  void write_blocks(uint64_t count) {
    for (uint64_t block = 0; block < count; ++block) {
      ASSERT_TRUE(write(block * kBlockSize, contents(block)).ok());
    }
  }

  static uint64_t record_start(uint64_t block) { return block * kDataRecordSize; }

  // Changes one bit of one byte of the fake log file.
  void flip(uint64_t file_offset) {
    char& byte = io_->bytes().at(file_offset);
    byte = static_cast<char>(byte ^ 0x01);
  }

  // Reads one block into a buffer pre-filled with 0xAA. It must fail with Corrupt and
  // the buffer must still be all 0xAA, so no byte of the bad record leaked out.
  void ExpectBlockReadIsCorrupt(uint64_t block) {
    Bytes out(kBlockSize, 0xAA);
    Status s = engine_->read(block * kBlockSize, kBlockSize, out.data());
    EXPECT_EQ(s.code(), StatusCode::Corrupt) << "block " << block << ": " << s.to_string();
    EXPECT_TRUE(SameBytes(out, Bytes(kBlockSize, 0xAA)))
        << "a Corrupt read must leave the caller's buffer as it was";
  }

  // Blocks whose records are intact must read back exactly, and the engine must
  // still accept new writes (Corrupt does not make it sticky).
  void ExpectOtherBlocksStillWork(const std::vector<uint64_t>& intact_blocks) {
    for (uint64_t block : intact_blocks) {
      EXPECT_TRUE(SameBytes(read(block * kBlockSize, kBlockSize), contents(block)))
          << "block " << block;
    }
    const Bytes fresh = pattern(99, kBlockSize);
    EXPECT_TRUE(write(50 * kBlockSize, fresh).ok());
    EXPECT_TRUE(SameBytes(read(50 * kBlockSize, kBlockSize), fresh));
  }

  // A full-block write needs no old contents, so it must succeed on a corrupt block
  // and from then on the block reads as the new data.
  void ExpectFullBlockWriteHeals(uint64_t block) {
    const Bytes fresh = pattern(77, kBlockSize);
    Status s = write(block * kBlockSize, fresh);
    EXPECT_TRUE(s.ok()) << s.to_string();
    EXPECT_TRUE(SameBytes(read(block * kBlockSize, kBlockSize), fresh));
  }
};

// --- Damage inside the record: the CRC catches it -----------------------------

TEST_F(EngineCorruptTest, AFlippedPayloadByteIsCorrupt) {
  // First, middle and last payload byte: the CRC covers the whole payload.
  for (size_t position : {size_t{0}, size_t{kBlockSize / 2}, size_t{kBlockSize - 1}}) {
    SCOPED_TRACE("payload byte " + std::to_string(position));
    make_engine(test::kBigCap);
    write_blocks(3);
    flip(record_start(1) + kPayloadStart + position);

    ExpectBlockReadIsCorrupt(1);
    ExpectOtherBlocksStillWork({0, 2});
    ExpectFullBlockWriteHeals(1);
  }
}

TEST_F(EngineCorruptTest, AFlippedHeaderByteIsCorrupt) {
  // One byte from several header fields: magic (0), type (6), seq (8), block_no (16),
  // and the stored CRC itself (36 and 39). Some fail a field check, some only the CRC.
  for (size_t position : {size_t{0}, size_t{6}, size_t{8}, size_t{16}, size_t{36}, size_t{39}}) {
    SCOPED_TRACE("header byte " + std::to_string(position));
    make_engine(test::kBigCap);
    write_blocks(3);
    flip(record_start(1) + position);

    ExpectBlockReadIsCorrupt(1);
    ExpectOtherBlocksStillWork({0, 2});
    ExpectFullBlockWriteHeals(1);
  }
}

TEST_F(EngineCorruptTest, AFlippedByteInTheLastRecordOfTheLogIsCorrupt) {
  // The record at the very end has nothing after it, so the read is cut short by the
  // end of the log. It must still be reported as Corrupt, not accepted.
  write_blocks(3);
  flip(record_start(2) + kPayloadStart + kBlockSize - 1);

  ExpectBlockReadIsCorrupt(2);
  ExpectOtherBlocksStillWork({0, 1});
}

TEST_F(EngineCorruptTest, AMultiBlockReadThatReachesACorruptBlockFailsAndLeavesItsRangeUntouched) {
  write_blocks(3);
  flip(record_start(1) + kPayloadStart + 100);

  // Blocks 0 .. 2 in one read. Block 0 may already have been delivered when block 1
  // fails, so only block 1's range (and the untouched block 2 after it) is checked.
  Bytes out(3 * kBlockSize, 0xAA);
  Status s = engine_->read(0, out.size(), out.data());
  EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.to_string();
  EXPECT_TRUE(SameBytes(test::slice(out, kBlockSize, kBlockSize), Bytes(kBlockSize, 0xAA)))
      << "the corrupt block's range must not receive any data";
  EXPECT_TRUE(SameBytes(test::slice(out, 2 * kBlockSize, kBlockSize), Bytes(kBlockSize, 0xAA)))
      << "nothing after the failing block may be filled in";

  ExpectOtherBlocksStillWork({0, 2});
}

// --- A valid record that is the wrong one --------------------------------------

TEST_F(EngineCorruptTest, AValidTrimRecordAtTheMappedOffsetIsCorrupt) {
  write_blocks(3);
  // A correct TRIM record for block 1 (so its CRC and range are fine), placed where
  // block 1's DATA record starts. Only the type is wrong.
  std::array<uint8_t, kTrimRecordSize> trim_record{};
  ASSERT_TRUE(encode_trim_record(2, 1, 1, trim_record).ok());
  io_->bytes().replace(record_start(1), kTrimRecordSize,
                       reinterpret_cast<const char*>(trim_record.data()), trim_record.size());

  ExpectBlockReadIsCorrupt(1);
  ExpectOtherBlocksStillWork({0, 2});
  ExpectFullBlockWriteHeals(1);
}

TEST_F(EngineCorruptTest, AValidDataRecordForADifferentBlockIsCorrupt) {
  // Copy a whole intact record over block 1's record. It passes every check except
  // "this record is for the block I asked for". Try a record from before and one
  // from after, so a check that only looks at one direction would not pass.
  for (uint64_t source : {uint64_t{0}, uint64_t{2}}) {
    SCOPED_TRACE("record of block " + std::to_string(source) + " copied over block 1");
    make_engine(test::kBigCap);
    write_blocks(3);
    const std::string copy = io_->bytes().substr(record_start(source), kDataRecordSize);
    io_->bytes().replace(record_start(1), kDataRecordSize, copy);

    ExpectBlockReadIsCorrupt(1);
    // The source block still reads correctly: the record there was not touched.
    EXPECT_TRUE(SameBytes(read(source * kBlockSize, kBlockSize), contents(source)));
    ExpectFullBlockWriteHeals(1);
  }
}

TEST_F(EngineCorruptTest, AValidDataRecordForADifferentBlockMadeWithTheEncoderIsCorrupt) {
  // The same idea with a record built by the encoder: a correct seq (2) and CRC, but
  // for block 7 instead of block 1.
  write_blocks(3);
  const Bytes payload = pattern(5, kBlockSize);
  std::array<uint8_t, kDataRecordSize> record{};
  ASSERT_TRUE(encode_data_record(2, 7, std::span<const uint8_t, kBlockSize>(payload), record).ok());
  io_->bytes().replace(record_start(1), kDataRecordSize,
                       reinterpret_cast<const char*>(record.data()), record.size());

  ExpectBlockReadIsCorrupt(1);
  ExpectOtherBlocksStillWork({0, 2});
}

// --- Partial writes need the old block ----------------------------------------

class EngineCorruptPartialWriteTest : public EngineCorruptTest {
 protected:
  // Runs `request` and checks that it failed with Corrupt, appended nothing and
  // asked for no write at all.
  void ExpectRejectedWithoutAppending(uint64_t offset, uint64_t len) {
    const uint64_t logged_before = engine_->bytes_logged();
    const size_t file_before = io_->bytes().size();
    io_->clear_calls();

    const Bytes data = pattern(33, len);
    Status s = write(offset, data);

    EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.to_string();
    EXPECT_EQ(io_->count(Op::Pwrite), 0U) << "nothing may be appended";
    EXPECT_EQ(io_->count(Op::Ftruncate), 0U) << "there was nothing to cut off";
    EXPECT_EQ(engine_->bytes_logged(), logged_before);
    EXPECT_EQ(io_->bytes().size(), file_before);
  }
};

TEST_F(EngineCorruptPartialWriteTest, APartialWriteOverACorruptBlockIsCorruptAndAppendsNothing) {
  write_blocks(3);
  flip(record_start(1) + kPayloadStart + 5);

  ExpectRejectedWithoutAppending(kBlockSize + 100, 10);  // 10 bytes inside block 1

  ExpectBlockReadIsCorrupt(1);
  ExpectOtherBlocksStillWork({0, 2});
}

TEST_F(EngineCorruptPartialWriteTest, APartialWriteOverAWrongBlockRecordIsCorruptToo) {
  write_blocks(3);
  const std::string copy = io_->bytes().substr(record_start(0), kDataRecordSize);
  io_->bytes().replace(record_start(1), kDataRecordSize, copy);

  ExpectRejectedWithoutAppending(kBlockSize + 100, 10);
  ExpectOtherBlocksStillWork({0, 2});
}

TEST_F(EngineCorruptPartialWriteTest, ACorruptFirstEdgeBlockRejectsTheWholeMultiBlockWrite) {
  write_blocks(4);
  flip(record_start(0) + kPayloadStart + 1);

  // From the middle of block 0 to the middle of block 3: both edges are partial, so
  // both are read before the first append. Block 0 is the bad one.
  ExpectRejectedWithoutAppending(100, 3 * kBlockSize);

  // Blocks 1 .. 3 were inside the range but nothing was appended, so they are as before.
  ExpectOtherBlocksStillWork({1, 2, 3});
  ExpectFullBlockWriteHeals(0);
}

TEST_F(EngineCorruptPartialWriteTest, ACorruptLastEdgeBlockRejectsTheWholeMultiBlockWrite) {
  write_blocks(4);
  flip(record_start(3) + kPayloadStart + kBlockSize - 1);

  // Same range, but now the bad block is the last edge. The first edge reads fine,
  // and still no record may be appended for it or for the middle blocks.
  ExpectRejectedWithoutAppending(100, 3 * kBlockSize);

  ExpectOtherBlocksStillWork({0, 1, 2});
  ExpectFullBlockWriteHeals(3);
}

TEST_F(EngineCorruptPartialWriteTest, ACorruptMiddleBlockDoesNotStopAWriteThatCoversItWhole) {
  write_blocks(4);
  flip(record_start(1) + kPayloadStart + 7);
  flip(record_start(2) + 8);  // seq byte of the other middle block

  // Blocks 1 and 2 are fully covered, so their old contents are never read. Only the
  // two intact edge blocks are read, so the write must succeed and replace both.
  const Bytes data = pattern(44, 3 * kBlockSize);
  Status s = write(100, data);
  ASSERT_TRUE(s.ok()) << s.to_string();

  EXPECT_TRUE(SameBytes(read(100, 3 * kBlockSize), data));
  // The edges keep the bytes outside the written range.
  EXPECT_TRUE(SameBytes(read(0, 100), test::slice(contents(0), 0, 100)));
  EXPECT_TRUE(SameBytes(read(100 + 3 * kBlockSize, kBlockSize - 100),
                        test::slice(contents(3), 100, kBlockSize - 100)));
}

TEST_F(EngineCorruptPartialWriteTest, AFullBlockWriteToACorruptBlockNeedsNoOldRead) {
  write_blocks(2);
  flip(record_start(1) + kPayloadStart);
  io_->clear_calls();

  ExpectFullBlockWriteHeals(1);
  // The write itself must not have read the bad record: the only pread is the one the
  // check at the end of the helper makes after the write.
  EXPECT_EQ(io_->count(Op::Pread), 1U);
}

}  // namespace
}  // namespace logblock
