#include "log_file.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "record.h"
#include "support/fault_io.h"

namespace logblock {
namespace {

using test::FaultIo;
using Op = FaultIo::Op;
using Bytes = std::vector<uint8_t>;

// A payload that depends on the block number, so records for different blocks
// differ, and each position holds a predictable value.
std::array<uint8_t, kBlockSize> payload_for(uint64_t block) {
  std::array<uint8_t, kBlockSize> p{};
  for (size_t i = 0; i < p.size(); ++i) {
    p[i] = static_cast<uint8_t>((block * 7 + i) % 251);
  }
  return p;
}

std::span<const uint8_t> as_bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// One record found by scanning the bytes in the fake file.
struct Found {
  uint64_t offset;
  RecordHeader header;
  Bytes payload;
};

// Decodes the whole file from the front, checking rules 1 to 8 for each record
// and rule 9 (seq goes 1, 2, 3, ... with no gaps). Fails the test if any record is
// invalid or if the records do not exactly fill the file.
std::vector<Found> scan(const std::string& file) {
  std::vector<Found> found;
  uint64_t offset = 0;
  uint64_t expected_seq = 1;
  while (offset < file.size()) {
    Record r{};
    Status s = decode_record(as_bytes(file).subspan(offset), &r);
    EXPECT_TRUE(s.ok()) << "record at offset " << offset << ": " << s.to_string();
    if (!s.ok()) {
      return found;
    }
    EXPECT_EQ(r.header.seq, expected_seq) << "at offset " << offset;
    ++expected_seq;
    found.push_back({offset, r.header, Bytes(r.payload.begin(), r.payload.end())});
    offset += kRecordHeaderSize + r.header.data_len;
  }
  EXPECT_EQ(offset, file.size()) << "the records must exactly fill the file";
  return found;
}

class LogFileTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(io_.open_log("fake.log").ok()); }

  // Appends a DATA record for `block` and returns the offset it landed at.
  uint64_t append_data(uint64_t block) {
    uint64_t offset = ~0ULL;
    auto payload = payload_for(block);
    Status s = log_.append_data(block, payload, &offset);
    EXPECT_TRUE(s.ok()) << s.to_string();
    return offset;
  }

  FaultIo io_;
  LogFile log_{io_};
};

TEST_F(LogFileTest, ANewLogStartsAtOffsetZeroWithSeqOne) {
  EXPECT_EQ(log_.end(), 0U);
  EXPECT_EQ(log_.next_seq(), 1U);
  EXPECT_TRUE(io_.bytes().empty());
  EXPECT_TRUE(io_.calls().size() == 1U);  // only the open_log from SetUp
}

TEST_F(LogFileTest, FirstDataRecordGoesAtOffsetZeroWithSeqOne) {
  uint64_t offset = append_data(7);
  EXPECT_EQ(offset, 0U);
  EXPECT_EQ(io_.bytes().size(), kDataRecordSize);

  auto records = scan(io_.bytes());
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].header.type, RecordType::Data);
  EXPECT_EQ(records[0].header.seq, 1U);
  EXPECT_EQ(records[0].header.block_no, 7U);
  auto expected = payload_for(7);
  EXPECT_EQ(records[0].payload, Bytes(expected.begin(), expected.end()));
}

TEST_F(LogFileTest, EndAndNextSeqAdvanceByOneWholeRecord) {
  append_data(1);
  EXPECT_EQ(log_.end(), kDataRecordSize);
  EXPECT_EQ(log_.next_seq(), 2U);

  ASSERT_TRUE(log_.append_trim(10, 3).ok());
  EXPECT_EQ(log_.end(), kDataRecordSize + kTrimRecordSize);
  EXPECT_EQ(log_.next_seq(), 3U);
}

TEST_F(LogFileTest, RecordsFollowEachOtherWithNoGapsAndIncreasingSeq) {
  uint64_t first = append_data(3);
  ASSERT_TRUE(log_.append_trim(10, 3).ok());
  uint64_t third = append_data(4);
  EXPECT_EQ(first, 0U);
  EXPECT_EQ(third, kDataRecordSize + kTrimRecordSize);  // 4,176

  auto records = scan(io_.bytes());  // checks the seqs 1, 2, 3 and that nothing is missing
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records[0].header.type, RecordType::Data);
  EXPECT_EQ(records[1].header.type, RecordType::Trim);
  EXPECT_EQ(records[1].offset, kDataRecordSize);
  EXPECT_EQ(records[1].header.block_no, 10U);
  EXPECT_EQ(records[1].header.block_count, 3U);
  EXPECT_EQ(records[2].offset, third);
  EXPECT_EQ(records[2].header.block_no, 4U);
  EXPECT_EQ(log_.end(), io_.bytes().size());
}

TEST_F(LogFileTest, ManyAppendsStayValid) {
  uint64_t expected_end = 0;
  for (uint64_t i = 0; i < 100; ++i) {
    EXPECT_EQ(append_data(i), expected_end);
    expected_end += kDataRecordSize;
  }
  EXPECT_EQ(log_.end(), 100 * kDataRecordSize);
  EXPECT_EQ(log_.next_seq(), 101U);
  EXPECT_EQ(scan(io_.bytes()).size(), 100U);
}

TEST_F(LogFileTest, EachRecordIsOneExplicitOffsetPwrite) {
  io_.clear_calls();
  append_data(1);
  ASSERT_TRUE(log_.append_trim(0, 1).ok());
  append_data(2);

  // One pwrite per record, at the previous end, with the whole record in it. So a
  // failed append can only leave a prefix of a single record behind.
  ASSERT_EQ(io_.calls().size(), 3U);
  EXPECT_EQ(io_.calls()[0].op, Op::Pwrite);
  EXPECT_EQ(io_.calls()[0].offset, 0U);
  EXPECT_EQ(io_.calls()[0].length, kDataRecordSize);
  EXPECT_EQ(io_.calls()[1].op, Op::Pwrite);
  EXPECT_EQ(io_.calls()[1].offset, kDataRecordSize);
  EXPECT_EQ(io_.calls()[1].length, kTrimRecordSize);
  EXPECT_EQ(io_.calls()[2].op, Op::Pwrite);
  EXPECT_EQ(io_.calls()[2].offset, kDataRecordSize + kTrimRecordSize);
  EXPECT_EQ(io_.calls()[2].length, kDataRecordSize);
}

TEST_F(LogFileTest, AppendingNeverSyncsOrTruncates) {
  // A write is acknowledged once it is appended; only flush makes it durable.
  io_.clear_calls();
  append_data(1);
  ASSERT_TRUE(log_.append_trim(0, 1).ok());
  EXPECT_EQ(io_.count(Op::Fsync), 0U);
  EXPECT_EQ(io_.count(Op::Ftruncate), 0U);
  EXPECT_EQ(io_.count(Op::FsyncParentDir), 0U);
}

TEST_F(LogFileTest, AFailedPwriteDoesNotAdvanceEndOrSeq) {
  append_data(1);
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 100);  // 100 bytes reach the file, then it fails

  uint64_t offset = 12345;
  auto payload = payload_for(2);
  Status s = log_.append_data(2, payload, &offset);
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_EQ(log_.end(), kDataRecordSize);
  EXPECT_EQ(log_.next_seq(), 2U);
  EXPECT_EQ(offset, 12345U) << "*record_offset must be left alone on failure";

  // The same goes for a TRIM record.
  io_.fail_next(Op::Pwrite, StatusCode::NoSpace);
  EXPECT_EQ(log_.append_trim(0, 1).code(), StatusCode::NoSpace);
  EXPECT_EQ(log_.end(), kDataRecordSize);
  EXPECT_EQ(log_.next_seq(), 2U);

  // The next append goes to the same place with the same seq, because writes use
  // explicit offsets. Cleaning up the torn bytes with ftruncate is tested separately.
  uint64_t retry = append_data(2);
  EXPECT_EQ(retry, kDataRecordSize);
  auto records = scan(io_.bytes());
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[1].header.seq, 2U);
}

// ---------------------------------------------------------------------------
// Reading records back.
// ---------------------------------------------------------------------------

TEST_F(LogFileTest, ReadRecordReturnsExactlyWhatWasAppended) {
  uint64_t a = append_data(5);
  ASSERT_TRUE(log_.append_trim(20, 4).ok());
  uint64_t c = append_data(6);

  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  ASSERT_TRUE(log_.read_record(a, &buffer, &r).ok());
  EXPECT_EQ(r.header.type, RecordType::Data);
  EXPECT_EQ(r.header.seq, 1U);
  EXPECT_EQ(r.header.block_no, 5U);
  auto want5 = payload_for(5);
  EXPECT_EQ(Bytes(r.payload.begin(), r.payload.end()), Bytes(want5.begin(), want5.end()));

  ASSERT_TRUE(log_.read_record(c, &buffer, &r).ok());
  EXPECT_EQ(r.header.seq, 3U);
  EXPECT_EQ(r.header.block_no, 6U);
  auto want6 = payload_for(6);
  EXPECT_EQ(Bytes(r.payload.begin(), r.payload.end()), Bytes(want6.begin(), want6.end()));

  // A TRIM record is the last-but-one record and only 40 bytes long.
  ASSERT_TRUE(log_.read_record(kDataRecordSize, &buffer, &r).ok());
  EXPECT_EQ(r.header.type, RecordType::Trim);
  EXPECT_EQ(r.header.block_no, 20U);
  EXPECT_EQ(r.header.block_count, 4U);
  EXPECT_TRUE(r.payload.empty());
}

TEST_F(LogFileTest, ReadRecordWorksOnTheLastRecordInTheFile) {
  // Fewer than kDataRecordSize bytes remain after a TRIM record at the very end.
  append_data(1);
  ASSERT_TRUE(log_.append_trim(2, 2).ok());
  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  ASSERT_TRUE(log_.read_record(kDataRecordSize, &buffer, &r).ok());
  EXPECT_EQ(r.header.type, RecordType::Trim);
}

TEST_F(LogFileTest, ReadRecordDoesNotChangePositionsOrWrite) {
  uint64_t a = append_data(1);
  io_.clear_calls();
  uint64_t end = log_.end();
  uint64_t seq = log_.next_seq();

  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  ASSERT_TRUE(log_.read_record(a, &buffer, &r).ok());
  EXPECT_EQ(log_.end(), end);
  EXPECT_EQ(log_.next_seq(), seq);
  EXPECT_EQ(io_.count(Op::Pwrite), 0U);
  EXPECT_EQ(io_.count(Op::Pread), 1U);
}

TEST_F(LogFileTest, ReadRecordRejectsACorruptRecord) {
  uint64_t a = append_data(1);
  uint64_t b = append_data(2);
  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  ASSERT_TRUE(log_.read_record(a, &buffer, &r).ok());
  ASSERT_TRUE(log_.read_record(b, &buffer, &r).ok());

  io_.bytes()[b + kRecordHeaderSize + 100] ^= 0x01;  // a payload byte of the second record
  EXPECT_EQ(log_.read_record(b, &buffer, &r).code(), StatusCode::Corrupt);
  EXPECT_TRUE(log_.read_record(a, &buffer, &r).ok()) << "the first record is still fine";

  io_.bytes()[a] = 'X';  // the first record's magic
  EXPECT_EQ(log_.read_record(a, &buffer, &r).code(), StatusCode::Corrupt);
}

TEST_F(LogFileTest, ReadRecordRejectsOffsetsThatAreNotRecordStarts) {
  append_data(1);
  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  ASSERT_TRUE(log_.read_record(0, &buffer, &r).ok());

  EXPECT_EQ(log_.read_record(1, &buffer, &r).code(), StatusCode::Corrupt);   // inside the header
  EXPECT_EQ(log_.read_record(40, &buffer, &r).code(), StatusCode::Corrupt);  // inside the payload
  EXPECT_EQ(log_.read_record(log_.end(), &buffer, &r).code(), StatusCode::Corrupt);  // at the end
  EXPECT_EQ(log_.read_record(log_.end() + 1000, &buffer, &r).code(), StatusCode::Corrupt);
  EXPECT_EQ(log_.read_record(~0ULL, &buffer, &r).code(), StatusCode::Corrupt);
}

TEST_F(LogFileTest, ReadRecordRejectsARecordCutShort) {
  uint64_t a = append_data(1);
  io_.bytes().resize(a + 1000);  // the file ends in the middle of the payload
  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  Status s = log_.read_record(a, &buffer, &r);
  EXPECT_EQ(s.code(), StatusCode::Corrupt);
  // It must be rule 7 (the record does not fit), not a CRC failure caused by
  // decoding stale bytes after the end of what was read.
  EXPECT_NE(s.message().find("rule 7"), std::string::npos) << s.message();
}

// --- A failed append is cut back -------------------

TEST_F(LogFileTest, AFailedPwriteIsCutBackToTheLastGoodEnd) {
  append_data(1);
  append_data(2);
  io_.clear_calls();
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 1000);  // 1000 torn bytes reach the file

  uint64_t offset = 0;
  auto payload = payload_for(3);
  EXPECT_EQ(log_.append_data(3, payload, &offset).code(), StatusCode::IoError);

  ASSERT_EQ(io_.count(Op::Ftruncate), 1U);
  for (const auto& call : io_.calls()) {
    if (call.op == Op::Ftruncate) {
      EXPECT_EQ(call.length, 2 * kDataRecordSize)
          << "cut back to end(), not to 0 or to the torn end";
    }
  }
  EXPECT_EQ(io_.bytes().size(), 2 * kDataRecordSize);
  EXPECT_EQ(log_.end(), 2 * kDataRecordSize);
  EXPECT_EQ(log_.next_seq(), 3U);
  EXPECT_FALSE(log_.broken());
}

TEST_F(LogFileTest, ATornWriteThatReachedNoBytesIsStillCutBack) {
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 0);
  uint64_t offset = 0;
  auto payload = payload_for(1);
  EXPECT_EQ(log_.append_data(1, payload, &offset).code(), StatusCode::IoError);
  EXPECT_EQ(io_.count(Op::Ftruncate), 1U) << "the cleanup does not depend on how many bytes got in";
}

TEST_F(LogFileTest, AFailedTrimIsCutBackToo) {
  append_data(1);
  io_.fail_next(Op::Pwrite, StatusCode::NoSpace, 20);
  EXPECT_EQ(log_.append_trim(0, 1).code(), StatusCode::NoSpace);
  EXPECT_EQ(io_.bytes().size(), kDataRecordSize);
  EXPECT_EQ(log_.end(), kDataRecordSize);
  EXPECT_FALSE(log_.broken());
}

TEST_F(LogFileTest, ASuccessfulAppendNeverTruncates) {
  append_data(1);
  EXPECT_EQ(io_.count(Op::Ftruncate), 0U);
}

TEST_F(LogFileTest, ARetryAfterACutBackLandsAtTheSamePlaceWithTheSameSeq) {
  append_data(1);
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 500);
  uint64_t offset = 0;
  auto payload = payload_for(2);
  ASSERT_FALSE(log_.append_data(2, payload, &offset).ok());

  ASSERT_TRUE(log_.append_data(2, payload, &offset).ok());
  EXPECT_EQ(offset, kDataRecordSize);
  EXPECT_EQ(io_.bytes().size(), 2 * kDataRecordSize);
  EXPECT_EQ(scan(io_.bytes()).size(), 2U) << "two whole records and nothing else";
}

TEST_F(LogFileTest, IfTheCutBackFailsTheOriginalErrorIsReturnedAndTheLogIsBroken) {
  append_data(1);
  io_.fail_next(Op::Pwrite, StatusCode::NoSpace, 1000);
  io_.fail_next(Op::Ftruncate, StatusCode::IoError);

  uint64_t offset = 0;
  auto payload = payload_for(2);
  Status s = log_.append_data(2, payload, &offset);
  EXPECT_EQ(s.code(), StatusCode::NoSpace) << "the caller sees the original failure";
  EXPECT_NE(s.message().find("cutting the log back"), std::string::npos) << s.message();
  EXPECT_TRUE(log_.broken());
  EXPECT_EQ(log_.end(), kDataRecordSize) << "torn bytes are never counted";
  EXPECT_EQ(log_.next_seq(), 2U);
}

TEST_F(LogFileTest, ABrokenLogRefusesEveryLaterAppendWithoutTouchingTheFile) {
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 10);
  io_.fail_next(Op::Ftruncate, StatusCode::IoError);
  uint64_t offset = 0;
  auto payload = payload_for(1);
  ASSERT_FALSE(log_.append_data(1, payload, &offset).ok());
  io_.clear_calls();

  EXPECT_EQ(log_.append_data(1, payload, &offset).code(), StatusCode::Failed);
  EXPECT_EQ(log_.append_trim(0, 1).code(), StatusCode::Failed);
  EXPECT_TRUE(io_.calls().empty()) << "no pwrite, and no new ftruncate attempt";
  EXPECT_TRUE(log_.broken()) << "there is no way back";
}

TEST_F(LogFileTest, ABrokenLogStillReadsItsGoodRecords) {
  append_data(1);
  io_.fail_next(Op::Pwrite, StatusCode::IoError, 10);
  io_.fail_next(Op::Ftruncate, StatusCode::IoError);
  uint64_t offset = 0;
  auto payload = payload_for(2);
  ASSERT_FALSE(log_.append_data(2, payload, &offset).ok());

  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  EXPECT_TRUE(log_.read_record(0, &buffer, &r).ok());
}

TEST_F(LogFileTest, ReadRecordIgnoresBytesPastTheEndOfTheLog) {
  // An append can fail after all its bytes reached the file (here: the error
  // comes back after the whole record was written). The log does not include it:
  // end() did not move, so no record starts at that offset.
  append_data(1);
  io_.fail_next(Op::Pwrite, StatusCode::IoError, kDataRecordSize);
  io_.fail_next(Op::Ftruncate, StatusCode::IoError);  // so the leftover stays in the file
  uint64_t offset = 0;
  auto payload = payload_for(2);
  ASSERT_EQ(log_.append_data(2, payload, &offset).code(), StatusCode::IoError);
  ASSERT_EQ(io_.bytes().size(), 2 * kDataRecordSize) << "the leftover record is in the file";
  ASSERT_EQ(log_.end(), kDataRecordSize);

  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  EXPECT_EQ(log_.read_record(kDataRecordSize, &buffer, &r).code(), StatusCode::Corrupt);

  // A valid record well beyond the end is no record of the log either. (This is the
  // case where end() - offset would underflow if the offset were not checked first.)
  Bytes copy(io_.bytes().begin(), io_.bytes().begin() + kDataRecordSize);
  const uint64_t far = 10000;
  io_.bytes().resize(far + kDataRecordSize);
  std::copy(copy.begin(), copy.end(), io_.bytes().begin() + far);
  EXPECT_EQ(log_.read_record(far, &buffer, &r).code(), StatusCode::Corrupt);
}

TEST_F(LogFileTest, ReadRecordReportsAnIoErrorAsIoError) {
  uint64_t a = append_data(1);
  io_.fail_next(Op::Pread);
  std::array<uint8_t, kDataRecordSize> buffer{};
  Record r{};
  EXPECT_EQ(log_.read_record(a, &buffer, &r).code(), StatusCode::IoError);
  EXPECT_TRUE(log_.read_record(a, &buffer, &r).ok()) << "the fault was one-shot";
}

}  // namespace
}  // namespace logblock
