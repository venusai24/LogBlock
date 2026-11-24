// Startup recovery by full scan (M2, user story 1): a restart on a non-empty log rebuilds the block
// map from the records it holds. Logs are built with LogBuilder (encode_data_record and
// encode_trim_record) and put in a FaultIo, and recovery is exercised through Engine::create.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "logblock/engine.h"
#include "record.h"
#include "support/engine_test_base.h"
#include "support/fault_io.h"
#include "support/forwarding_io.h"
#include "support/log_builder.h"
#include "support/recovery_fixture.h"

namespace logblock {
namespace {

using test::Bytes;
using test::FaultIo;
using test::ForwardingIo;
using test::LogBuilder;
using Op = FaultIo::Op;
using test::kLogPath;

class RecoveryTest : public test::RecoveryFixture {};

// --- What a record means for the map (D29) ----------------------------------------

TEST_F(RecoveryTest, ADataRecordMapsItsBlockToItsOffset) {
  LogBuilder log;
  log.data(0, 1);
  log.data(5, 2);
  load(log);
  ASSERT_TRUE(start().ok());
  expect_block(0, 1);  // offset 0 is a real offset
  expect_block(5, 2);
  expect_zero_block(1);  // never written
  expect_zero_block(kBlockCount - 1);
}

TEST_F(RecoveryTest, ATrimRecordMakesItsBlocksReadZero) {
  LogBuilder log;
  for (uint64_t b = 2; b <= 7; ++b) {
    log.data(b, static_cast<uint8_t>(b));
  }
  log.trim(4, 2);  // blocks 4 and 5
  load(log);
  ASSERT_TRUE(start().ok());
  expect_block(2, 2);
  expect_block(3, 3);
  expect_zero_block(4);
  expect_zero_block(5);
  expect_block(6, 6);
  expect_block(7, 7);
}

TEST_F(RecoveryTest, AWholeDeviceTrimClearsEveryBlock) {
  LogBuilder log;
  log.data(0, 1);
  log.data(kBlockCount - 1, 2);
  log.data(1000, 3);
  log.trim(0, static_cast<uint32_t>(kBlockCount));
  load(log);
  ASSERT_TRUE(start().ok());
  expect_zero_block(0);
  expect_zero_block(1000);
  expect_zero_block(kBlockCount - 1);
}

TEST_F(RecoveryTest, OverwritesApplyInLogOrder) {
  LogBuilder log;
  log.data(3, 10);
  log.data(3, 20);
  log.data(3, 30);  // the last record for block 3 wins
  log.data(4, 40);
  load(log);
  ASSERT_TRUE(start().ok());
  expect_block(3, 30);
  expect_block(4, 40);
}

TEST_F(RecoveryTest, ATrimThenAWriteLeavesTheWrite) {
  LogBuilder log;
  log.data(8, 1);
  log.trim(8, 1);
  log.data(8, 2);
  load(log);
  ASSERT_TRUE(start().ok());
  expect_block(8, 2);
}

TEST_F(RecoveryTest, AWriteThenATrimLeavesZeros) {
  LogBuilder log;
  log.data(8, 1);
  log.trim(8, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  expect_zero_block(8);
}

// --- Where the log continues (FR-003) ----------------------------------------------

TEST_F(RecoveryTest, EndNextSeqAndBytesLoggedContinueFromTheLog) {
  LogBuilder log;
  log.data(1, 1);
  log.data(2, 2);
  log.trim(1, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(engine_->bytes_logged(), log.end());

  // The next append starts at the end of the last valid record, with the next seq.
  const Bytes data = LogBuilder::payload(9);
  io_->clear_calls();
  ASSERT_TRUE(engine_->write(7 * kBlockSize, kBlockSize, data.data()).ok());
  ASSERT_EQ(io_->count(Op::Pwrite), 1U);
  const auto& call = io_->calls()[static_cast<size_t>(first_call(Op::Pwrite))];
  EXPECT_EQ(call.offset, log.end());

  Record record;
  const std::string& bytes = io_->bytes();
  ASSERT_GE(bytes.size(), log.end() + kDataRecordSize);
  Status s = decode_record(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()) + log.end(),
                               bytes.size() - log.end()),
      &record);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(record.header.seq, log.next_seq());
  EXPECT_EQ(record.header.block_no, 7U);
  EXPECT_EQ(engine_->bytes_logged(), log.end() + kDataRecordSize);
}

TEST_F(RecoveryTest, APartialWriteAfterRecoveryKeepsTheRestOfTheOldBlock) {
  LogBuilder log;
  log.data(3, 9);
  load(log);
  ASSERT_TRUE(start().ok());

  const Bytes patch(10, 0xEE);
  ASSERT_TRUE(engine_->write(3 * kBlockSize + 100, patch.size(), patch.data()).ok());

  Bytes want = LogBuilder::payload(9);
  std::copy(patch.begin(), patch.end(), want.begin() + 100);
  EXPECT_TRUE(test::SameBytes(read_block(3), want));
}

TEST_F(RecoveryTest, WritesAfterARestartSurviveAnotherRestart) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  const Bytes data = LogBuilder::payload(7);
  ASSERT_TRUE(engine_->write(2 * kBlockSize, kBlockSize, data.data()).ok());
  ASSERT_TRUE(engine_->trim(1 * kBlockSize, kBlockSize).ok());
  ASSERT_TRUE(engine_->flush().ok());

  restart();
  ASSERT_TRUE(start().ok());
  expect_zero_block(1);
  expect_block(2, 7);
}

// --- New devices (FR-005) ---------------------------------------------------------

TEST_F(RecoveryTest, AnEmptyLogStartsAsANewDevice) {
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(engine_->bytes_logged(), 0U);
  expect_zero_block(0);
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::None);
  EXPECT_EQ(r.records_replayed, 0U);
  EXPECT_EQ(r.log_bytes_scanned, 0U);
  EXPECT_EQ(r.torn_bytes_removed, 0U);
}

TEST_F(RecoveryTest, AnEmptyLogGetsNoLogFsync) {
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
}

TEST_F(RecoveryTest, TheParentDirectoryIsSyncedOnEverySuccessfulStart) {
  ASSERT_TRUE(start().ok());  // missing log: created, then its name is made durable
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 1U);

  LogBuilder log;
  log.data(1, 1);
  load(log);
  io_->clear_calls();
  ASSERT_TRUE(start().ok());  // existing log: synced again, whoever created it (D35)
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 1U);
}

TEST_F(RecoveryTest, AnEmptyExistingLogGetsNoLogFsyncButOneParentDirectorySync) {
  io_->set_log_exists(true);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(io_->count(Op::CreateLog), 0U);
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 1U);
}

TEST_F(RecoveryTest, AFailedParentDirectorySyncRefusesAndTheNextStartSucceeds) {
  io_->fail_next(Op::FsyncParentDir, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  // FR-009: the empty log may be left behind. The next start handles it as an empty log.
  EXPECT_TRUE(io_->bytes().empty());
  ASSERT_TRUE(start().ok());
  EXPECT_NE(engine_, nullptr);
}

// --- Making recovered bytes durable (D28) ------------------------------------------

TEST_F(RecoveryTest, ANonEmptyLogGetsExactlyOneLogFsyncBeforeTheFirstRequest) {
  LogBuilder log;
  log.data(1, 1);
  log.data(2, 2);
  load(log);
  ASSERT_TRUE(start().ok());
  // No request has been made yet.
  EXPECT_EQ(io_->count(Op::Fsync), 1U);
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
  // The directory is synced once, after the log fsync (D35).
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 1U);
  EXPECT_LT(first_call(Op::Fsync), first_call(Op::FsyncParentDir));
}

TEST_F(RecoveryTest, RecoveryDoesNotChangeTheBytesOfAValidLog) {
  LogBuilder log;
  log.data(1, 1);
  log.trim(1, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(io_->bytes(), log.bytes());
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
}

// --- Restart clears the sticky states (FR-021) -------------------------------------

TEST_F(RecoveryTest, ARestartClearsTheFailedFlushState) {
  ASSERT_TRUE(start().ok());
  const Bytes data = LogBuilder::payload(1);
  ASSERT_TRUE(engine_->write(0, kBlockSize, data.data()).ok());
  io_->fail_next(Op::Fsync, StatusCode::IoError);
  EXPECT_FALSE(engine_->flush().ok());
  EXPECT_EQ(engine_->flush().code(), StatusCode::Failed) << "sticky until restart";

  restart();
  ASSERT_TRUE(start().ok());
  expect_block(0, 1);
  EXPECT_TRUE(engine_->flush().ok());
}

TEST_F(RecoveryTest, ARestartClearsTheFailedAppendState) {
  ASSERT_TRUE(start().ok());
  const Bytes data = LogBuilder::payload(1);
  // The append fails with nothing written, and the clean-up truncate fails too: the log is stuck.
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 0);
  io_->fail_next(Op::Ftruncate, StatusCode::IoError);
  EXPECT_FALSE(engine_->write(0, kBlockSize, data.data()).ok());
  EXPECT_EQ(engine_->write(0, kBlockSize, data.data()).code(), StatusCode::Failed);

  restart();
  ASSERT_TRUE(start().ok());
  EXPECT_TRUE(engine_->write(0, kBlockSize, data.data()).ok());
  expect_block(0, 1);
}

// --- The report (FR-023) ----------------------------------------------------------

TEST_F(RecoveryTest, TheReportSaysFullScanWithCountsAndTime) {
  LogBuilder log;
  log.data(1, 1);
  log.data(2, 2);
  log.trim(1, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::FullScan);
  EXPECT_EQ(r.slot, '-');
  EXPECT_EQ(r.generation, 0U);
  EXPECT_EQ(r.records_replayed, 3U);
  EXPECT_EQ(r.log_bytes_scanned, log.end());
  EXPECT_EQ(r.torn_bytes_removed, 0U);
  EXPECT_EQ(r.unreadable_checkpoints, 0U);
  EXPECT_TRUE(r.warnings.empty());
  EXPECT_LT(r.time_us, uint64_t{60} * 1000 * 1000) << "a few records do not take a minute";
}

TEST_F(RecoveryTest, CheckpointCountersStartAtZero) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  CheckpointCounters c = engine_->checkpoint_counters();
  EXPECT_EQ(c.written, 0U);
  EXPECT_EQ(c.failed, 0U);
  EXPECT_EQ(c.unreadable, 0U);
  EXPECT_EQ(c.duration_count, 0U);
  EXPECT_EQ(c.duration_max_us, 0U);
  EXPECT_EQ(c.duration_sum_us, 0U);
}

// --- Io errors during recovery are returned, not classified ------------------------

TEST_F(RecoveryTest, AFailedReadDuringTheScanIsTheIosStatus) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  io_->fail_next(Op::Pread, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->bytes(), log.bytes()) << "a refused start changes nothing";
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
}

TEST_F(RecoveryTest, AFailedLogFsyncRefusesTheStart) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  io_->fail_next(Op::Fsync, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 0U) << "stop at the first failure";
}

// --- The log lock (FR-030, D20, D28) ----------------------------------------------

TEST_F(RecoveryTest, TheLockIsTakenAfterOpeningTheLogAndBeforeReadingIt) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  ASSERT_TRUE(start().ok());
  const int opened = first_call(Op::OpenLogExisting);
  const int locked = first_call(Op::LockLog);
  ASSERT_GE(locked, 0) << "a start must take the lock";
  EXPECT_LT(opened, locked);
  EXPECT_LT(locked, first_call(Op::FileSize));
  EXPECT_LT(locked, first_call(Op::Pread));
  EXPECT_EQ(io_->count(Op::LockLog), 1U);
}

TEST_F(RecoveryTest, AHeldLockRefusesAndNamesTheLog) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  io_->set_lock_held(true);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::Failed) << s.message();
  EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
  EXPECT_EQ(engine_, nullptr);
}

TEST_F(RecoveryTest, AHeldLockLeavesTheFilesAloneAndDoesNoOtherFileWork) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  io_->set_lock_held(true);
  EXPECT_FALSE(start().ok());
  EXPECT_EQ(io_->bytes(), log.bytes());
  // Only the open and the lock attempt: nothing read, written, synced or truncated.
  EXPECT_EQ(io_->count(Op::FileSize), 0U);
  EXPECT_EQ(io_->count(Op::Pread), 0U);
  EXPECT_EQ(io_->count(Op::Pwrite), 0U);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 0U);
}

TEST_F(RecoveryTest, AFailedLockCallIsTheIosStatus) {
  io_->fail_next(Op::LockLog, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(io_->count(Op::FileSize), 0U);
}

TEST_F(RecoveryTest, ReleasingTheLockLetsTheNextStartThrough) {
  io_->set_lock_held(true);
  EXPECT_FALSE(start().ok());
  io_->set_lock_held(false);
  EXPECT_TRUE(start().ok());
}

// =============================================================================
// Damage is classified and never hidden (T020, user story 2)
// =============================================================================

// --- An unopenable checkpoint file: the advice depends on whether the log exists ------

TEST_F(RecoveryTest, AnUnopenableCheckpointWithTheLogPresentSaysRemovingItIsSafe) {
  LogBuilder log;
  log.data(1, 1);
  load(log);
  io_->fail_next(Op::OpenCkpt);
  Status s = start();
  ASSERT_FALSE(s.ok());
  EXPECT_NE(s.message().find(test::kCkptPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("safe to remove"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("full scan"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("nothing was changed"), std::string::npos) << s.message();
  EXPECT_EQ(io_->bytes(), log.bytes());
}

TEST_F(RecoveryTest, AnUnopenableCheckpointWithTheLogMissingWarnsItMayBeTheOnlySign) {
  io_->fail_next(Op::OpenCkpt);
  Status s = start();
  ASSERT_FALSE(s.ok());
  EXPECT_NE(s.message().find(test::kCkptPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("log is missing"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("only sign"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("empty device"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("nothing was changed"), std::string::npos) << s.message();
  EXPECT_EQ(s.message().find("safe to remove"), std::string::npos) << s.message();
  EXPECT_EQ(io_->count(Op::CreateLog), 0U);
}

// Four valid records, then `tail`: the good log and the bytes a crash left after it.
struct Damaged {
  LogBuilder good;
  std::string tail;
};

// The first `n` bytes of a real DATA record that would have been the next one.
std::string partial_next_data(LogBuilder& log, size_t n) {
  LogBuilder scratch = log;  // the record is built after the good ones, then cut
  const uint64_t start = scratch.end();
  scratch.data(77, 5);
  return scratch.bytes().substr(start, n);
}

LogBuilder four_records() {
  LogBuilder log;
  log.data(1, 1);
  log.data(2, 2);
  log.trim(1, 1);
  log.data(3, 3);
  return log;
}

class DamageTest : public RecoveryTest {
 protected:
  // Starts on `good` + `tail` and expects the tail to be removed.
  void expect_torn_tail_removed(const LogBuilder& good, const std::string& tail) {
    SCOPED_TRACE("tail of " + std::to_string(tail.size()) + " bytes");
    io_ = std::make_unique<FaultIo>();
    io_->bytes() = good.bytes() + tail;
    ASSERT_TRUE(start().ok());

    // The tail is gone and the good records are untouched.
    EXPECT_EQ(io_->bytes(), good.bytes());
    const RecoveryReport& r = engine_->recovery_report();
    EXPECT_EQ(r.torn_bytes_removed, tail.size());
    EXPECT_EQ(r.torn_offset, good.end());
    ASSERT_FALSE(r.warnings.empty());
    EXPECT_NE(r.warnings.back().find("torn tail removed"), std::string::npos);
    EXPECT_NE(r.warnings.back().find(kLogPath), std::string::npos);
    EXPECT_EQ(engine_->bytes_logged(), good.end());

    // One truncate to the good end, then one fsync of the log, before any request.
    ASSERT_EQ(io_->count(Op::Ftruncate), 1U);
    EXPECT_EQ(io_->calls()[static_cast<size_t>(first_call(Op::Ftruncate))].length, good.end());
    EXPECT_EQ(io_->count(Op::Fsync), 1U);
    EXPECT_LT(first_call(Op::Ftruncate), first_call(Op::Fsync));
    EXPECT_EQ(io_->count(Op::Pwrite), 0U);

    // FR-006: the next append lands at the good end.
    io_->clear_calls();
    const Bytes data = LogBuilder::payload(9);
    ASSERT_TRUE(engine_->write(100 * kBlockSize, kBlockSize, data.data()).ok());
    ASSERT_EQ(io_->count(Op::Pwrite), 1U);
    EXPECT_EQ(io_->calls()[static_cast<size_t>(first_call(Op::Pwrite))].offset, good.end());
    // And the data of the good records still reads back (four_records() has data in block 2).
    if (good.end() > 0) {
      expect_block(2, 2);
      expect_zero_block(1);
    }
  }

  // Starts on `bytes` and expects a refusal that changed nothing and says where and why.
  void expect_refused(const std::string& bytes, uint64_t offset, uint64_t last_good_seq) {
    io_ = std::make_unique<FaultIo>();
    io_->bytes() = bytes;
    Status s = start();
    EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.message();
    EXPECT_EQ(engine_, nullptr);
    EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
    EXPECT_NE(s.message().find("offset " + std::to_string(offset)), std::string::npos)
        << s.message();
    EXPECT_NE(s.message().find("last good seq " + std::to_string(last_good_seq)),
              std::string::npos)
        << s.message();
    // FR-009: nothing changed. No write, truncate or sync of any kind, and the bytes are as found.
    EXPECT_EQ(io_->bytes(), bytes);
    EXPECT_EQ(io_->count(Op::Pwrite), 0U);
    EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
    EXPECT_EQ(io_->count(Op::Fsync), 0U);
  }
};

// --- Torn tails are removed and reported (FR-006) ---------------------------------

TEST_F(DamageTest, AShortHeaderIsATornTail) {
  LogBuilder good = four_records();
  for (size_t n : {size_t{1}, size_t{39}}) {
    expect_torn_tail_removed(good, partial_next_data(good, n));
  }
}

TEST_F(DamageTest, ARecordCutOffPastItsHeaderIsATornTail) {
  LogBuilder good = four_records();
  for (size_t n : {size_t{40}, size_t{41}, size_t{2000}, size_t{kDataRecordSize - 1}}) {
    expect_torn_tail_removed(good, partial_next_data(good, n));
  }
}

TEST_F(DamageTest, ATrimCutShortIsATornTail) {
  LogBuilder good = four_records();
  LogBuilder scratch = good;
  const uint64_t start_of_trim = scratch.end();
  scratch.trim(5, 3);
  expect_torn_tail_removed(good, scratch.bytes().substr(start_of_trim, kTrimRecordSize - 1));
}

TEST_F(DamageTest, AnAllZeroTailFromARecordBoundaryIsATornTail) {
  LogBuilder good = four_records();
  for (size_t n : {size_t{40}, size_t{41}, size_t{100}, size_t{kDataRecordSize},
                   size_t{3 * kDataRecordSize + 17}}) {
    expect_torn_tail_removed(good, std::string(n, '\0'));
  }
}

TEST_F(DamageTest, AnAllZeroLogIsATornTailOfTheWholeFile) {
  expect_torn_tail_removed(LogBuilder(), std::string(kDataRecordSize, '\0'));
  // And the device starts as a new one.
  EXPECT_EQ(engine_->recovery_report().source, RecoverySource::None);
}

TEST_F(DamageTest, AGoodLogWithATornTailKeepsTheCountsOfWhatWasReplayed) {
  LogBuilder good = four_records();
  io_->bytes() = good.bytes() + partial_next_data(good, 100);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(engine_->recovery_report().records_replayed, 4U);
  EXPECT_EQ(engine_->recovery_report().source, RecoverySource::FullScan);
}

// --- Corruption refuses and changes nothing (FR-008, FR-009) -----------------------

TEST_F(DamageTest, CorruptionInTheMiddleOfTheLogIsRefused) {
  LogBuilder log;
  log.data(1, 1);
  const uint64_t second = log.data(2, 2);
  log.data(3, 3);
  log.data(4, 4);
  std::string bytes = log.bytes();
  bytes[second + 500] ^= 0x01;  // a payload byte of record 2: the CRC fails
  expect_refused(bytes, second, 1);
}

TEST_F(DamageTest, ACompleteNonZeroFinalRecordWithABadCrcIsCorruptionNotATornTail) {
  LogBuilder log = four_records();
  const uint64_t last = log.data(9, 9);
  std::string bytes = log.bytes();
  bytes[last + 700] ^= static_cast<char>(0x80);
  expect_refused(bytes, last, 4);
}

TEST_F(DamageTest, ASeqGapIsRefused) {
  LogBuilder log;
  log.data(1, 1);
  log.data(2, 2);
  log.set_next_seq(5);  // 3 and 4 are missing
  const uint64_t gap = log.data(3, 3);
  expect_refused(log.bytes(), gap, 2);
}

TEST_F(DamageTest, ARepeatedSeqIsRefused) {
  LogBuilder log;
  log.data(1, 1);
  log.data(2, 2);
  log.set_next_seq(2);  // seq 2 again
  const uint64_t repeat = log.data(3, 3);
  expect_refused(log.bytes(), repeat, 2);
}

TEST_F(DamageTest, ZerosFollowedByANonZeroByteAreRefused) {
  LogBuilder log = four_records();
  std::string bytes = log.bytes() + std::string(200, '\0') + "x" + std::string(50, '\0');
  expect_refused(bytes, log.end(), 4);
}

TEST_F(DamageTest, AFirstRecordWithTheWrongSeqIsRefused) {
  LogBuilder log;
  log.set_next_seq(2);
  const uint64_t first = log.data(1, 1);
  expect_refused(log.bytes(), first, 0);
}

TEST_F(DamageTest, CorruptionAfterATornLookingTailStillWins) {
  // Garbage that fails the header checks is corruption even when it is short.
  LogBuilder log = four_records();
  std::string bytes = log.bytes() + std::string(60, 'z');
  expect_refused(bytes, log.end(), 4);
}

// --- The cap (FR-010) -------------------------------------------------------------

TEST_F(DamageTest, AValidLogOverTheCapIsRefusedNamingBothSizes) {
  LogBuilder log = four_records();  // 3 DATA + 1 TRIM
  load(log);
  const uint64_t cap = log.end() - 1;
  Status s = start(cap);
  EXPECT_EQ(s.code(), StatusCode::Failed) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(std::to_string(log.end())), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(std::to_string(cap)), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("raise"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
  EXPECT_EQ(io_->bytes(), log.bytes());
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
}

TEST_F(DamageTest, AValidLogExactlyAtTheCapStarts) {
  LogBuilder log = four_records();
  load(log);
  EXPECT_TRUE(start(log.end()).ok());
}

TEST_F(DamageTest, ATornTailPastTheCapDoesNotCount) {
  LogBuilder log = four_records();
  io_->bytes() = log.bytes() + std::string(5000, '\0');  // valid size is exactly the cap
  ASSERT_TRUE(start(log.end()).ok());
  EXPECT_EQ(io_->bytes(), log.bytes());
}

TEST_F(DamageTest, ACapOverrunWithATornTailRefusesAndRemovesNothing) {
  LogBuilder log = four_records();
  const std::string bytes = log.bytes() + std::string(100, '\0');
  io_->bytes() = bytes;
  EXPECT_EQ(start(log.end() - 1).code(), StatusCode::Failed);
  EXPECT_EQ(io_->bytes(), bytes) << "the tail is removed only after every refusal has passed";
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
}

TEST_F(DamageTest, CorruptionIsReportedBeforeTheCapIsJudged) {
  // The valid part (three records) is over the smallest legal cap, and the fourth is damaged.
  LogBuilder log = four_records();
  std::string bytes = log.bytes();
  bytes[bytes.size() - 10] ^= 0x01;
  io_->bytes() = bytes;
  Status s = start(kDataRecordSize);
  EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.message();
}

TEST_F(DamageTest, AFullLogAtRestartStartsReadsAndFlushesButWritesGetNoSpace) {
  LogBuilder log = four_records();
  load(log);
  ASSERT_TRUE(start(log.end()).ok());
  expect_block(2, 2);
  EXPECT_TRUE(engine_->flush().ok());
  const Bytes data = LogBuilder::payload(1);
  EXPECT_EQ(engine_->write(0, kBlockSize, data.data()).code(), StatusCode::NoSpace);
}

TEST_F(DamageTest, ARaisedCapAppliesAfterARestart) {
  LogBuilder log = four_records();
  load(log);
  ASSERT_TRUE(start(log.end()).ok());
  const Bytes data = LogBuilder::payload(1);
  EXPECT_EQ(engine_->write(0, kBlockSize, data.data()).code(), StatusCode::NoSpace);

  restart();
  ASSERT_TRUE(start(log.end() + kDataRecordSize).ok());
  EXPECT_TRUE(engine_->write(0, kBlockSize, data.data()).ok());
}

// --- Settings checked before any file work ----------------------------------------

TEST_F(DamageTest, ACheckpointIntervalUnderOneRecordIsOutOfRangeBeforeAnyFileWork) {
  Status s = start(test::kBigCap, kMinCheckpointInterval - 1);
  EXPECT_EQ(s.code(), StatusCode::OutOfRange) << s.message();
  EXPECT_NE(s.message().find("checkpoint_interval"), std::string::npos) << s.message();
  EXPECT_TRUE(io_->calls().empty()) << "no Io call at all";
  EXPECT_TRUE(start(test::kBigCap, kMinCheckpointInterval).ok());
}

// --- Tail removal can fail (FR-007) -----------------------------------------------

TEST_F(DamageTest, AFailedTruncateRefusesAndLeavesTheBytesUnchanged) {
  LogBuilder good = four_records();
  const std::string bytes = good.bytes() + std::string(100, '\0');
  io_->bytes() = bytes;
  io_->fail_next(Op::Ftruncate, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->bytes(), bytes);
  EXPECT_EQ(io_->count(Op::Fsync), 0U);
  // The next start handles the same tail normally.
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(io_->bytes(), good.bytes());
}

TEST_F(DamageTest, AFailedFsyncAfterTheTruncateRefusesAndTheNextStartSucceeds) {
  LogBuilder good = four_records();
  io_->bytes() = good.bytes() + partial_next_data(good, 300);
  io_->fail_next(Op::Fsync, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->bytes(), good.bytes()) << "FR-009 exception: the bytes are already gone";
  EXPECT_EQ(io_->count(Op::FsyncParentDir), 0U) << "stop at the first failure";
  ASSERT_TRUE(start().ok());
  expect_block(3, 3);
}

TEST_F(RecoveryTest, ATornTailThenAFailedParentDirectorySyncSaysTheTailWasAlreadyRemoved) {
  LogBuilder good;
  good.data(1, 1);
  good.data(2, 2);
  io_->bytes() = good.bytes() + partial_next_data(good, 300);
  io_->fail_next(Op::FsyncParentDir, StatusCode::IoError);
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->bytes(), good.bytes()) << "FR-009 exception: the bytes are already gone";
  EXPECT_NE(s.message().find("the torn tail was already removed, 300 bytes"), std::string::npos)
      << s.message();
  ASSERT_TRUE(start().ok());
}

TEST_F(DamageTest, AFailedFsyncAfterTheTruncateSaysTheTailWasAlreadyRemovedAndHowMany) {
  LogBuilder good = four_records();
  io_->bytes() = good.bytes() + partial_next_data(good, 300);
  io_->fail_next(Op::Fsync, StatusCode::IoError);
  Status s = start();
  ASSERT_EQ(s.code(), StatusCode::IoError) << s.message();
  // FR-007, FR-009: the operator learns the file already differs from before the start.
  EXPECT_NE(s.message().find("already removed"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("300 bytes"), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
}

TEST_F(DamageTest, AFailedFsyncWithNoTornTailDoesNotClaimAnyRemoval) {
  LogBuilder good = four_records();
  io_->bytes() = good.bytes();
  io_->fail_next(Op::Fsync, StatusCode::IoError);
  Status s = start();
  ASSERT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(s.message().find("already removed"), std::string::npos) << s.message();
}

}  // namespace
}  // namespace logblock
