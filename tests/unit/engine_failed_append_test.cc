// Tests for failed appends and failed reads. When a pwrite fails after K bytes, the engine cuts the
// file back to the last good end with ftruncate, so torn bytes never sit in the middle of the log.
// If that ftruncate fails too, the engine is in the sticky AppendFailed state: writes, trims and
// flushes return Failed, reads still work.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

// FaultIo fails "the next pwrite". To make the third pwrite of a multi-block write
// fail, this wrapper counts pwrite calls and queues the fault on FaultIo just before
// the chosen one. It owns the FaultIo and forwards every call to it; the test keeps
// plain pointers to both.
class NthPwriteFailIo final : public Io {
 public:
  explicit NthPwriteFailIo(std::unique_ptr<test::FaultIo> inner) : inner_(std::move(inner)) {}

  // From now on, the n-th pwrite call (1 = the next one) fails after `partial_bytes`.
  void fail_pwrite_number(size_t n, StatusCode code, size_t partial_bytes) {
    countdown_ = n;
    code_ = code;
    partial_bytes_ = partial_bytes;
  }

  Status open_log(const std::string& path) override { return inner_->open_log(path); }
  Status file_size(uint64_t* size) override { return inner_->file_size(size); }
  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override {
    return inner_->pread(offset, buf, len, bytes_read);
  }
  Status pwrite(uint64_t offset, const void* buf, size_t len) override {
    if (countdown_ > 0 && --countdown_ == 0) {
      inner_->fail_next(Op::Pwrite, code_, partial_bytes_);
    }
    return inner_->pwrite(offset, buf, len);
  }
  Status fsync() override { return inner_->fsync(); }
  Status ftruncate(uint64_t size) override { return inner_->ftruncate(size); }
  Status fsync_parent_dir() override { return inner_->fsync_parent_dir(); }

 private:
  std::unique_ptr<test::FaultIo> inner_;
  size_t countdown_ = 0;  // 0 = no failure armed
  StatusCode code_ = StatusCode::IoError;
  size_t partial_bytes_ = 0;
};

class EngineFailedAppendTest : public test::EngineTestBase {
 protected:
  // Writes one whole block with data derived from `seed`.
  Status write_block(uint64_t block, uint8_t seed) {
    return write(block * kBlockSize, pattern(seed, kBlockSize));
  }

  // Like make_engine(), but the Io is wrapped so a later pwrite (not just the next
  // one) can be made to fail. Returns the wrapper to arm.
  NthPwriteFailIo* make_engine_with_nth_pwrite_fault() {
    engine_.reset();
    auto inner = std::make_unique<test::FaultIo>();
    io_ = inner.get();
    auto wrapper = std::make_unique<NthPwriteFailIo>(std::move(inner));
    NthPwriteFailIo* raw = wrapper.get();
    Status s = Engine::create("engine-test.log", test::kBigCap, std::move(wrapper), &engine_);
    EXPECT_TRUE(s.ok()) << s.message();
    return raw;
  }

  // Walks the fake log file from offset 0. Every record must be complete and valid,
  // seq must be 1, 2, 3, ... and the records must tile the file exactly: no gap, no
  // torn bytes at the end. On success *count is the number of records.
  ::testing::AssertionResult LogIsWhole(size_t* count) const {
    const std::string& file = io_->bytes();
    const auto* base = reinterpret_cast<const uint8_t*>(file.data());
    uint64_t offset = 0;
    uint64_t expected_seq = 1;
    while (offset < file.size()) {
      Record record;
      Status s =
          decode_record(std::span<const uint8_t>(base + offset, file.size() - offset), &record);
      if (!s.ok()) {
        return ::testing::AssertionFailure()
               << "no valid record at offset " << offset << " (" << s.to_string() << ")";
      }
      if (record.header.seq != expected_seq) {
        return ::testing::AssertionFailure() << "record at offset " << offset << " has seq "
                                             << record.header.seq << ", expected " << expected_seq;
      }
      offset += kRecordHeaderSize + record.header.data_len;
      ++expected_seq;
    }
    *count = static_cast<size_t>(expected_seq - 1);
    return ::testing::AssertionSuccess();
  }

  // The header of the record that starts at `offset`. The offset must be a record start.
  Record record_at(uint64_t offset) const {
    const std::string& file = io_->bytes();
    Record record;
    Status s = decode_record(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(file.data()) + offset,
                                 file.size() - offset),
        &record);
    EXPECT_TRUE(s.ok()) << s.message();
    return record;
  }

  // After a failed append the file must have been cut back to `good_end`: exactly one
  // ftruncate to that size was made, and the file has that size.
  void ExpectCutBackTo(uint64_t good_end) const {
    ASSERT_EQ(io_->count(Op::Ftruncate), 1U);
    for (const auto& call : io_->calls()) {
      if (call.op == Op::Ftruncate) {
        EXPECT_EQ(call.length, good_end) << "ftruncate must go to the last good end";
      }
    }
    EXPECT_EQ(io_->bytes().size(), good_end);
    EXPECT_EQ(engine_->bytes_logged(), good_end);
  }

  // Puts the engine into the AppendFailed state: a write of block 2 whose pwrite fails
  // with `first_code` after 10 bytes, followed by an ftruncate that fails as well.
  // Blocks 0 and 1 hold known data before that. Returns what the failing write returned.
  Status break_the_log(StatusCode first_code) {
    EXPECT_TRUE(write_block(0, 1).ok());
    EXPECT_TRUE(write_block(1, 2).ok());
    good_end_ = engine_->bytes_logged();
    // Queue the ftruncate fault first: the pwrite fault is used up by the write below,
    // and the ftruncate fault by the engine's clean-up right after it.
    io_->fail_next(Op::Ftruncate, StatusCode::IoError);
    io_->fail_next(Op::Pwrite, first_code, 10);
    return write_block(2, 3);
  }

  // What every request must say once the log is broken. Reads are checked separately.
  void ExpectWritesTrimsAndFlushesFail() {
    EXPECT_EQ(write_block(3, 4).code(), StatusCode::Failed);
    EXPECT_EQ(write(100, pattern(5, 10)).code(), StatusCode::Failed);  // partial write too
    EXPECT_EQ(engine_->trim(0, kBlockSize).code(), StatusCode::Failed);
    EXPECT_EQ(engine_->flush().code(), StatusCode::Failed);
    EXPECT_EQ(engine_->flush().code(), StatusCode::Failed) << "the state is sticky";
  }

  uint64_t good_end_ = 0;
};

// --- A pwrite that fails after K bytes ----------------------------------------

TEST_F(EngineFailedAppendTest, AFailedWriteIsCutBackToTheLastGoodEnd) {
  // K = 0 (nothing written), 10 (inside the header) and 1000 (inside the payload).
  for (size_t k : {size_t{0}, size_t{10}, size_t{1000}}) {
    SCOPED_TRACE("pwrite fails after " + std::to_string(k) + " bytes");
    make_engine(test::kBigCap);
    ASSERT_TRUE(write_block(0, 1).ok());  // seq 1
    ASSERT_TRUE(write_block(1, 2).ok());  // seq 2
    const uint64_t good_end = engine_->bytes_logged();
    io_->clear_calls();
    io_->fail_next(Op::Pwrite, StatusCode::IoError, k);

    Status s = write_block(1, 9);  // an overwrite, so the map entry could change

    EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
    ExpectCutBackTo(good_end);
    // The map still points at the old records.
    EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
    EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), pattern(2, kBlockSize)));
  }
}

TEST_F(EngineFailedAppendTest, TheNextAppendAfterAFailedOneLandsAtTheLastGoodEndWithTheNextSeq) {
  for (size_t k : {size_t{0}, size_t{10}, size_t{1000}}) {
    SCOPED_TRACE("pwrite fails after " + std::to_string(k) + " bytes");
    make_engine(test::kBigCap);
    ASSERT_TRUE(write_block(0, 1).ok());  // seq 1
    ASSERT_TRUE(write_block(1, 2).ok());  // seq 2
    const uint64_t good_end = engine_->bytes_logged();
    io_->fail_next(Op::Pwrite, StatusCode::IoError, k);
    ASSERT_EQ(write_block(2, 3).code(), StatusCode::IoError);

    const Bytes fresh = pattern(4, kBlockSize);
    ASSERT_TRUE(write(2 * kBlockSize, fresh).ok());

    // The failed attempt did not use up seq 3, and left no gap in the file.
    const Record record = record_at(good_end);
    EXPECT_EQ(record.header.type, RecordType::Data);
    EXPECT_EQ(record.header.seq, 3U);
    EXPECT_EQ(record.header.block_no, 2U);
    EXPECT_EQ(engine_->bytes_logged(), good_end + kDataRecordSize);
    size_t records = 0;
    EXPECT_TRUE(LogIsWhole(&records));
    EXPECT_EQ(records, 3U);
    EXPECT_TRUE(SameBytes(read(2 * kBlockSize, kBlockSize), fresh));
    EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  }
}

TEST_F(EngineFailedAppendTest, TheVeryFirstAppendCanFailAndBeRetried) {
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 10);
  EXPECT_EQ(write_block(0, 1).code(), StatusCode::IoError);
  ExpectCutBackTo(0);

  ASSERT_TRUE(write_block(0, 1).ok());
  EXPECT_EQ(record_at(0).header.seq, 1U) << "the first record still gets seq 1";
  size_t records = 0;
  EXPECT_TRUE(LogIsWhole(&records));
  EXPECT_EQ(records, 1U);
}

TEST_F(EngineFailedAppendTest, AFailedAppendDoesNotMakeTheEngineSticky) {
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 500);
  ASSERT_EQ(write_block(0, 1).code(), StatusCode::IoError);  // the ftruncate works

  // Every kind of request still works: nothing is in a failed state.
  EXPECT_TRUE(write_block(1, 2).ok());
  EXPECT_TRUE(engine_->trim(0, kBlockSize).ok());
  EXPECT_TRUE(engine_->flush().ok());
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), pattern(2, kBlockSize)));
}

TEST_F(EngineFailedAppendTest, AMultiBlockWriteFailingPartWayLeavesOnlyWholeRecords) {
  NthPwriteFailIo* faulty = make_engine_with_nth_pwrite_fault();
  const Bytes old_data = pattern(1, 6 * kBlockSize);  // blocks 0 .. 5
  ASSERT_TRUE(write(0, old_data).ok());               // seq 1 .. 6
  const uint64_t before = engine_->bytes_logged();
  io_->clear_calls();

  // Write blocks 1 .. 4. The first two appends (blocks 1 and 2) succeed, the third
  // (block 3) is cut off after 1000 bytes.
  faulty->fail_pwrite_number(3, StatusCode::IoError, 1000);
  Status s = write(kBlockSize, pattern(2, 4 * kBlockSize));

  EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
  // The two finished records stay, the torn third one is cut off.
  const uint64_t good_end = before + 2 * kDataRecordSize;
  ExpectCutBackTo(good_end);
  size_t records = 0;
  EXPECT_TRUE(LogIsWhole(&records));
  EXPECT_EQ(records, 8U);

  // Blocks outside the range keep their data. Blocks inside it may be old or new (the
  // spec does not say), so only that they can still be read is checked.
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), test::slice(old_data, 0, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(5 * kBlockSize, kBlockSize),
                        test::slice(old_data, 5 * kBlockSize, kBlockSize)));
  Bytes inside(4 * kBlockSize);
  EXPECT_TRUE(engine_->read(kBlockSize, inside.size(), inside.data()).ok());

  // The engine goes on, and the next record continues the sequence.
  ASSERT_TRUE(write_block(7, 3).ok());
  EXPECT_EQ(record_at(good_end).header.seq, 9U);
  EXPECT_TRUE(LogIsWhole(&records));
  EXPECT_EQ(records, 9U);
}

// --- A TRIM whose pwrite fails ------------------------------------------------

TEST_F(EngineFailedAppendTest, AFailedTrimIsCutBackAndTheBlocksKeepTheirContents) {
  // A TRIM record is 40 bytes: K = 0, 10 (inside the header) and 39 (one byte short).
  for (size_t k : {size_t{0}, size_t{10}, size_t{39}}) {
    SCOPED_TRACE("pwrite fails after " + std::to_string(k) + " bytes");
    make_engine(test::kBigCap);
    const Bytes data = pattern(1, 4 * kBlockSize);
    ASSERT_TRUE(write(0, data).ok());  // seq 1 .. 4
    const uint64_t good_end = engine_->bytes_logged();
    io_->clear_calls();
    io_->fail_next(Op::Pwrite, StatusCode::IoError, k);

    Status s = engine_->trim(0, 4 * kBlockSize);

    EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
    ExpectCutBackTo(good_end);
    EXPECT_TRUE(SameBytes(read(0, 4 * kBlockSize), data)) << "a failed trim must trim nothing";

    // And the next trim goes through at the last good end with seq 5.
    ASSERT_TRUE(engine_->trim(kBlockSize, kBlockSize).ok());
    const Record record = record_at(good_end);
    EXPECT_EQ(record.header.type, RecordType::Trim);
    EXPECT_EQ(record.header.seq, 5U);
    size_t records = 0;
    EXPECT_TRUE(LogIsWhole(&records));
    EXPECT_EQ(records, 5U);
    EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), zeros(kBlockSize)));
  }
}

// --- The ftruncate fails too: AppendFailed ------------------------------------

TEST_F(EngineFailedAppendTest, WhenTheCutBackFailsTheRequestStillReturnsThePwriteError) {
  Status s = break_the_log(StatusCode::IoError);
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
  EXPECT_EQ(engine_->bytes_logged(), good_end_) << "torn bytes are not counted";
}

TEST_F(EngineFailedAppendTest, AfterAFailedCutBackWritesTrimsAndFlushesReturnFailed) {
  ASSERT_EQ(break_the_log(StatusCode::IoError).code(), StatusCode::IoError);
  io_->clear_calls();

  ExpectWritesTrimsAndFlushesFail();

  EXPECT_EQ(io_->count(Op::Pwrite), 0U) << "a rejected request must not touch the log";
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
  EXPECT_EQ(engine_->bytes_logged(), good_end_);
}

TEST_F(EngineFailedAppendTest, AfterAFailedCutBackReadsStillWork) {
  ASSERT_EQ(break_the_log(StatusCode::IoError).code(), StatusCode::IoError);

  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(kBlockSize, kBlockSize), pattern(2, kBlockSize)));
  EXPECT_TRUE(SameBytes(read(2 * kBlockSize, kBlockSize), zeros(kBlockSize)))
      << "the failed write must not have changed block 2";
  Bytes across(3 * kBlockSize);
  EXPECT_TRUE(engine_->read(0, across.size(), across.data()).ok());

  // Failing requests in between do not change that.
  ExpectWritesTrimsAndFlushesFail();
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
}

TEST_F(EngineFailedAppendTest, AfterAFailedCutBackRequestsThatAppendNothingStillReturnFailed) {
  ASSERT_EQ(break_the_log(StatusCode::IoError).code(), StatusCode::IoError);

  // All later writes and trims fail, even when they would append nothing.
  EXPECT_EQ(engine_->write(0, 0, nullptr).code(), StatusCode::Failed);
  EXPECT_EQ(engine_->write(100, 0, nullptr).code(), StatusCode::Failed);
  EXPECT_EQ(engine_->trim(100, 200).code(), StatusCode::Failed);  // covers no full block
  EXPECT_EQ(engine_->trim(1, kBlockSize).code(), StatusCode::Failed);
  EXPECT_EQ(engine_->trim(0, 0).code(), StatusCode::Failed);
}

TEST_F(EngineFailedAppendTest, AfterAFailedCutBackAnOutOfRangeRequestStillReturnsOutOfRange) {
  ASSERT_EQ(break_the_log(StatusCode::IoError).code(), StatusCode::IoError);

  // The range check comes before the state check.
  const Bytes one_byte = pattern(1, 1);
  EXPECT_EQ(write(kDeviceSize, one_byte).code(), StatusCode::OutOfRange);
  EXPECT_EQ(write(kDeviceSize - 1, pattern(1, 2)).code(), StatusCode::OutOfRange);
  EXPECT_EQ(write(UINT64_MAX, one_byte).code(), StatusCode::OutOfRange);
  EXPECT_EQ(engine_->trim(kDeviceSize - kBlockSize, 2 * kBlockSize).code(), StatusCode::OutOfRange);
  EXPECT_EQ(engine_->write(kDeviceSize + 1, 0, nullptr).code(), StatusCode::OutOfRange);
  // A request inside the device is still Failed.
  EXPECT_EQ(write(kDeviceSize - 1, one_byte).code(), StatusCode::Failed);
}

TEST_F(EngineFailedAppendTest, AFailedCutBackAfterAFailedTrimAlsoBreaksTheLog) {
  ASSERT_TRUE(write_block(0, 1).ok());
  good_end_ = engine_->bytes_logged();
  io_->fail_next(Op::Ftruncate, StatusCode::IoError);
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 20);

  Status s = engine_->trim(0, kBlockSize);

  EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
  EXPECT_EQ(engine_->bytes_logged(), good_end_);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
  ExpectWritesTrimsAndFlushesFail();
}

// --- NoSpace from the file system, then a failed cut back ---------------------

TEST_F(EngineFailedAppendTest, ANoSpaceFailureWithAFailedCutBackReturnsNoSpaceThenFailed) {
  Status s = break_the_log(StatusCode::NoSpace);

  // The request that hit the full disk says NoSpace. Every later one says Failed (not
  // NoSpace, because the log is now broken and a retry will never help).
  EXPECT_EQ(s.code(), StatusCode::NoSpace) << s.to_string();
  ExpectWritesTrimsAndFlushesFail();
  EXPECT_EQ(engine_->write(0, 0, nullptr).code(), StatusCode::Failed);
  EXPECT_EQ(engine_->trim(100, 200).code(), StatusCode::Failed);
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), pattern(1, kBlockSize)));
}

TEST_F(EngineFailedAppendTest, ANoSpaceFailureWhoseCutBackWorksLeavesTheEngineHealthy) {
  ASSERT_TRUE(write_block(0, 1).ok());
  const uint64_t good_end = engine_->bytes_logged();
  io_->clear_calls();
  io_->fail_next(Op::Pwrite, StatusCode::NoSpace, 1000);

  Status s = write_block(1, 2);

  EXPECT_EQ(s.code(), StatusCode::NoSpace) << s.to_string();
  ExpectCutBackTo(good_end);
  EXPECT_TRUE(write_block(1, 2).ok()) << "once there is room again, writes work";
}

// --- A pread that fails --------------------------------------------------------

TEST_F(EngineFailedAppendTest, AFailedPreadReturnsIoErrorAndTheNextReadSucceeds) {
  const Bytes data = pattern(1, kBlockSize);
  ASSERT_TRUE(write(0, data).ok());
  io_->fail_next(Op::Pread, StatusCode::IoError);

  Bytes out(kBlockSize, 0xAA);
  Status s = engine_->read(0, kBlockSize, out.data());

  EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
  EXPECT_TRUE(SameBytes(out, Bytes(kBlockSize, 0xAA))) << "a failed read must deliver nothing";
  EXPECT_TRUE(SameBytes(read(0, kBlockSize), data)) << "the failure was a one-off";
}

TEST_F(EngineFailedAppendTest, AFailedPreadInAPartialWriteAppendsNothingAndLeavesTheEngineHealthy) {
  ASSERT_TRUE(write_block(0, 1).ok());
  const uint64_t good_end = engine_->bytes_logged();
  io_->clear_calls();
  io_->fail_next(Op::Pread, StatusCode::IoError);

  Status s = write(100, pattern(2, 10));  // partial write: needs the old block 0

  EXPECT_EQ(s.code(), StatusCode::IoError) << s.to_string();
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U) << "nothing was appended, so nothing to cut off";
  EXPECT_EQ(engine_->bytes_logged(), good_end);

  // A read failure is not an append failure: writes still work.
  EXPECT_TRUE(write(100, pattern(2, 10)).ok());
}

}  // namespace
}  // namespace logblock
