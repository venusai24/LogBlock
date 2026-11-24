// The checkpoint write path (M2, user story 3): when a checkpoint is taken, in what order it
// reaches disk, which slot it goes to, and what happens when a step fails. Uses a small
// checkpoint_interval so a handful of records triggers one. Slots are read back with the
// production decoder (decode_slot, tested alone in checkpoint_test.cc).
#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

#include "checkpoint.h"
#include "logblock/engine.h"
#include "support/checkpoint_builder.h"
#include "support/recovery_fixture.h"

namespace logblock {
namespace {

using test::Bytes;
using test::Op;
using test::kCkptPath;
using test::make_slot;
using test::put_slot;
using Ops = std::vector<test::Op>;

// Three DATA records. A checkpoint is due when the log has grown by this much since the last.
constexpr uint64_t kInterval = 3 * kDataRecordSize;

class EngineCheckpointTest : public test::RecoveryFixture {
 protected:
  void SetUp() override {
    RecoveryFixture::SetUp();
    io_->set_log_exists(true);
  }

  Status start_small(uint64_t cap = test::kBigCap) { return start(cap, kInterval); }

  // Writes one whole block, which appends exactly one DATA record.
  Status write_block(uint64_t block, uint8_t seed) {
    Bytes data = test::pattern(seed, kBlockSize);
    return engine_->write(block * kBlockSize, kBlockSize, data.data());
  }

  // Writes `n` blocks starting at block `first`, one record each.
  void write_blocks(uint64_t first, uint64_t n) {
    for (uint64_t b = first; b < first + n; ++b) {
      ASSERT_TRUE(write_block(b, static_cast<uint8_t>(b + 1)).ok()) << "block " << b;
    }
  }

  // The calls this test cares about, from call index `mark` on.
  Ops checkpoint_ops_since(size_t mark) const {
    Ops ops;
    const auto& calls = io_->calls();
    for (size_t i = mark; i < calls.size(); ++i) {
      switch (calls[i].op) {
        case Op::Fsync:
        case Op::OpenCkpt:
        case Op::CkptPwrite:
        case Op::CkptFsync:
        case Op::FsyncParentDir:
          ops.push_back(calls[i].op);
          break;
        default:
          break;
      }
    }
    return ops;
  }

  size_t mark() const { return io_->calls().size(); }

  // Decodes slot `index` of the fake checkpoint file.
  DecodedSlot slot(int index) const {
    const std::string& file = io_->ckpt_bytes();
    const size_t off = test::slot_offset(index);
    if (file.size() <= off) {
      return {};
    }
    const size_t len = std::min<size_t>(file.size() - off, kCheckpointSlotSize);
    return decode_slot(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(file.data()) + off, len));
  }

  // Appends a warning-free log of `n` DATA records for blocks 0..n-1 (seed block + 1).
  test::LogBuilder log_of(uint64_t n) {
    test::LogBuilder log;
    for (uint64_t b = 0; b < n; ++b) {
      log.data(b, static_cast<uint8_t>(b + 1));
    }
    return log;
  }
};

// --- Order of calls (FR-014, D26) -------------------------------------------------------

TEST_F(EngineCheckpointTest,
       TheFirstCheckpointFsyncsTheLogThenWritesThenFsyncsThenSyncsTheDirectory) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  const size_t m = mark();
  write_blocks(2, 1);
  EXPECT_EQ(checkpoint_ops_since(m),
            (Ops{Op::Fsync, Op::OpenCkpt, Op::CkptPwrite, Op::CkptFsync, Op::FsyncParentDir}));
}

TEST_F(EngineCheckpointTest, LaterCheckpointsSkipTheOpenAndTheDirectorySync) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 3);
  const size_t m = mark();
  write_blocks(3, 3);
  EXPECT_EQ(checkpoint_ops_since(m), (Ops{Op::Fsync, Op::CkptPwrite, Op::CkptFsync}));
}

TEST_F(EngineCheckpointTest, ACheckpointFileThatAlreadyExistsIsNotCreatedAgain) {
  test::LogBuilder log = log_of(3);
  load(log);
  put_slot(*io_, 0, make_slot(log.bytes(), 1, 3));
  ASSERT_TRUE(start_small().ok());
  const size_t m = mark();
  write_blocks(10, 3);
  EXPECT_EQ(checkpoint_ops_since(m), (Ops{Op::Fsync, Op::CkptPwrite, Op::CkptFsync}));
}

TEST_F(EngineCheckpointTest, TheCheckpointWriteIsOneWholeSlot) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 3);
  for (const auto& call : io_->calls()) {
    if (call.op == Op::CkptPwrite) {
      EXPECT_EQ(call.offset, 0U);
      EXPECT_EQ(call.length, kCheckpointSlotSize);
    }
  }
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
}

// --- Trigger (FR-011) -------------------------------------------------------------------

TEST_F(EngineCheckpointTest, TheTriggerIsExactlyTheIntervalAndNotOneRecordBefore) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U) << "two records are under the interval";
  write_blocks(2, 1);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
  write_blocks(3, 2);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U) << "two records since the last checkpoint";
  write_blocks(5, 1);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 2U);
}

TEST_F(EngineCheckpointTest, ATrimIsAnotherRecordOfItsOwnSizeAndCanBeTheTrigger) {
  // The interval is two DATA records plus one TRIM, so the TRIM is what reaches it.
  ASSERT_TRUE(start(test::kBigCap, 2 * kDataRecordSize + kTrimRecordSize).ok());
  write_blocks(0, 2);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
  ASSERT_TRUE(engine_->trim(10 * kBlockSize, kBlockSize).ok());
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
  DecodedSlot s = slot(0);
  ASSERT_EQ(s.state, SlotState::Valid) << s.reason;
  EXPECT_EQ(s.header.covered_seq, 3U);
  EXPECT_EQ(s.header.last_record_offset, 2 * kDataRecordSize);
}

TEST_F(EngineCheckpointTest, AMultiBlockWriteCrossingTheIntervalCheckpointsAtTheCrossing) {
  ASSERT_TRUE(start_small().ok());
  Bytes data = test::pattern(9, 5 * kBlockSize);
  ASSERT_TRUE(engine_->write(0, data.size(), data.data()).ok());
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
  // The snapshot holds records 1..3 only: blocks 0..2 are mapped, 3 and 4 are not yet.
  DecodedSlot s = slot(0);
  ASSERT_EQ(s.state, SlotState::Valid) << s.reason;
  EXPECT_EQ(s.header.covered_seq, 3U);
  for (uint64_t b = 0; b < 5; ++b) {
    EXPECT_EQ(s.map[b] != BlockMap::kUnmapped, b < 3) << "block " << b;
  }
  // The two records after the crossing are 2 * 4136 bytes: under the interval, so no second one.
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
  EXPECT_EQ(engine_->bytes_logged(), 5 * kDataRecordSize);
}

TEST_F(EngineCheckpointTest, TheSnapshotIncludesTheRecordThatTriggeredItAndNamesIt) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  ASSERT_TRUE(write_block(7, 0x55).ok());  // the third record triggers
  DecodedSlot s = slot(0);
  ASSERT_EQ(s.state, SlotState::Valid) << s.reason;
  EXPECT_EQ(s.header.generation, 1U);
  EXPECT_EQ(s.header.covered_seq, 3U);
  EXPECT_EQ(s.header.last_record_offset, 2 * kDataRecordSize);
  EXPECT_EQ(s.map[7], 2 * kDataRecordSize);  // the triggering record is in the map
  EXPECT_EQ(s.map[0], 0U);
  EXPECT_EQ(s.map[1], kDataRecordSize);
  EXPECT_EQ(s.map[2], BlockMap::kUnmapped);
}

TEST_F(EngineCheckpointTest, ACheckpointDoesNotCountAgainstTheCap) {
  // Room for exactly six records. Two checkpoints happen on the way, and neither uses any.
  ASSERT_TRUE(start_small(6 * kDataRecordSize).ok());
  write_blocks(0, 6);
  EXPECT_EQ(engine_->checkpoint_counters().written, 2U);
  EXPECT_EQ(engine_->bytes_logged(), 6 * kDataRecordSize);
  Status s = write_block(6, 1);
  EXPECT_EQ(s.code(), StatusCode::NoSpace) << s.message();
}

// --- Slot choice and generations (D24) ---------------------------------------------------

TEST_F(EngineCheckpointTest, SlotAFirstThenAlwaysTheOlderSlot) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 3);
  EXPECT_EQ(slot(0).header.generation, 1U);
  EXPECT_EQ(slot(1).state, SlotState::Missing);
  write_blocks(3, 3);
  EXPECT_EQ(slot(0).header.generation, 1U);
  EXPECT_EQ(slot(1).header.generation, 2U);
  write_blocks(6, 3);
  EXPECT_EQ(slot(0).header.generation, 3U) << "the third checkpoint overwrites the older, A";
  EXPECT_EQ(slot(1).header.generation, 2U);
  write_blocks(9, 3);
  EXPECT_EQ(slot(0).header.generation, 3U);
  EXPECT_EQ(slot(1).header.generation, 4U);
  EXPECT_EQ(slot(1).header.covered_seq, 12U);
}

TEST_F(EngineCheckpointTest, GenerationContinuesAfterARestartAndTheOlderSlotIsOverwritten) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 6);  // generation 1 in A, generation 2 in B
  restart();
  ASSERT_TRUE(start_small().ok());
  EXPECT_EQ(engine_->recovery_report().generation, 2U);
  EXPECT_EQ(engine_->recovery_report().slot, 'B');
  write_blocks(10, 3);
  EXPECT_EQ(slot(0).header.generation, 3U) << "the older slot, A, takes the next checkpoint";
  EXPECT_EQ(slot(1).header.generation, 2U);
}

TEST_F(EngineCheckpointTest, LastCheckpointEndIsRestoredAfterARestart) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 7);  // checkpoints after records 3 and 6; one record since
  EXPECT_EQ(io_->count(Op::CkptPwrite), 2U);
  restart();
  ASSERT_TRUE(start_small().ok());
  write_blocks(10, 1);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U) << "two records since the checkpoint: not yet";
  write_blocks(11, 1);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U) << "three records since the checkpoint at record 6";
  EXPECT_EQ(slot(0).header.covered_seq, 9U);
}

struct SlotCase {
  const char* name;
  int expected_slot;  // where the next checkpoint goes
  bool a_valid;
  bool b_valid;
  bool a_newer;       // when both are valid
  bool damage_other;  // the slot that is not valid is damaged instead of missing
};

TEST_F(EngineCheckpointTest, AMissingOrUnreadableSlotCountsAsOlder) {
  const SlotCase cases[] = {
      {"only A valid, B missing", 1, true, false, false, false},
      {"only A valid, B unreadable", 1, true, false, false, true},
      {"only B valid, A missing", 0, false, true, false, false},
      {"only B valid, A unreadable", 0, false, true, false, true},
      {"both valid, A newer", 1, true, true, true, false},
      {"both valid, B newer", 0, true, true, false, false},
  };
  for (const SlotCase& c : cases) {
    SCOPED_TRACE(c.name);
    restart();
    io_->ckpt_bytes().clear();
    test::LogBuilder log = log_of(10);
    load(log);
    std::string older = make_slot(log.bytes(), 4, 5);
    std::string newer = make_slot(log.bytes(), 5, 10);
    std::string damaged = make_slot(log.bytes(), 9, 10);
    damaged[100] ^= 0x01;
    if (c.a_valid && c.b_valid) {
      put_slot(*io_, 0, c.a_newer ? newer : older);
      put_slot(*io_, 1, c.a_newer ? older : newer);
    } else if (c.a_valid) {
      put_slot(*io_, 0, newer);
      if (c.damage_other) {
        put_slot(*io_, 1, damaged);
      }
    } else {
      put_slot(*io_, 1, newer);
      if (c.damage_other) {
        put_slot(*io_, 0, damaged);
      }
    }
    ASSERT_TRUE(start_small().ok());
    write_blocks(20, 3);  // three records since the checkpoint that covers record 10
    ASSERT_EQ(io_->count(Op::CkptPwrite), 1U);
    const int written = c.expected_slot;
    EXPECT_EQ(slot(written).state, SlotState::Valid);
    EXPECT_EQ(slot(written).header.covered_seq, 13U)
        << "the new checkpoint is in the expected slot";
    EXPECT_EQ(slot(written).header.generation, 6U);
    EXPECT_EQ(slot(1 - written).header.covered_seq == 13U, false);
  }
}

// --- Checkpoints after a full scan (D25) --------------------------------------------------

TEST_F(EngineCheckpointTest, ALogPastTheIntervalWithNoSlotsCheckpointsOnItsFirstWrite) {
  load(log_of(10));
  ASSERT_TRUE(start_small().ok());
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U) << "both slots missing: silent, no fresh checkpoint";
  write_blocks(20, 1);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
  EXPECT_EQ(slot(0).header.covered_seq, 11U);
}

TEST_F(EngineCheckpointTest, ALogUnderTheIntervalWithNoSlotsDoesNotCheckpointUntilItReachesIt) {
  load(log_of(1));
  ASSERT_TRUE(start_small().ok());
  ASSERT_TRUE(write_block(20, 1).ok());  // the log now holds two records: under three
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
  ASSERT_TRUE(write_block(21, 1).ok());  // the third record reaches the interval
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
}

// --- Failures (D27, D10) ------------------------------------------------------------------

class CheckpointFailureTest : public EngineCheckpointTest {
 protected:
  // Writes records until the one that triggers the first checkpoint. Every request must succeed.
  void write_until_checkpoint_due() {
    ASSERT_TRUE(start_small().ok());
    write_blocks(0, 3);
  }

  void expect_checkpoints_stopped_and_flush_works() {
    EXPECT_EQ(engine_->checkpoint_counters().failed, 1U);
    EXPECT_EQ(engine_->checkpoint_counters().written, 0U);
    const size_t m = mark();
    write_blocks(3, 6);  // two more intervals' worth
    EXPECT_EQ(checkpoint_ops_since(m).size(), 0U) << "no further checkpoint attempt";
    EXPECT_TRUE(engine_->flush().ok());
    EXPECT_EQ(engine_->checkpoint_counters().failed, 1U);
    std::vector<std::string> warnings = engine_->take_warnings();
    // Taking the warnings empties the queue, so a second call returns nothing.
    EXPECT_TRUE(engine_->take_warnings().empty());
    (void)warnings;
  }
};

TEST_F(CheckpointFailureTest,
       AFailedCheckpointPwriteLeavesTheRequestSuccessfulAndStopsCheckpoints) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::CkptPwrite);
  ASSERT_TRUE(write_block(2, 3).ok());
  expect_checkpoints_stopped_and_flush_works();
}

TEST_F(CheckpointFailureTest, AFailedCheckpointFsyncLeavesTheRequestSuccessfulAndStopsCheckpoints) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::CkptFsync);
  ASSERT_TRUE(write_block(2, 3).ok());
  expect_checkpoints_stopped_and_flush_works();
}

TEST_F(CheckpointFailureTest, AFailedCheckpointFileCreationLeavesTheRequestSuccessful) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::OpenCkpt);
  ASSERT_TRUE(write_block(2, 3).ok());
  expect_checkpoints_stopped_and_flush_works();
}

TEST_F(CheckpointFailureTest, AFailedDirectorySyncAfterCreatingTheFileCountsAsAFailure) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::FsyncParentDir);
  ASSERT_TRUE(write_block(2, 3).ok());
  expect_checkpoints_stopped_and_flush_works();
}

TEST_F(CheckpointFailureTest, AFailedCheckpointWarnsOnceWithTheCause) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::CkptPwrite);
  ASSERT_TRUE(write_block(2, 3).ok());
  std::vector<std::string> warnings = engine_->take_warnings();
  ASSERT_EQ(warnings.size(), 1U);
  EXPECT_NE(warnings[0].find("checkpoint"), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find(kCkptPath), std::string::npos) << warnings[0];
}

TEST_F(CheckpointFailureTest, ACheckpointFailureDoesNotTouchTheLogOrLaterRecords) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::CkptPwrite);
  ASSERT_TRUE(write_block(2, 3).ok());
  write_blocks(3, 2);
  EXPECT_EQ(engine_->bytes_logged(), 5 * kDataRecordSize);
  expect_block(4, 5);
}

TEST_F(CheckpointFailureTest, AFailedLogFsyncInACheckpointSetsFlushFailedAndNoFurtherFsyncIsMade) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 2);
  io_->fail_next(Op::Fsync);
  ASSERT_TRUE(write_block(2, 3).ok()) << "the record was appended; only flushes promise durability";
  EXPECT_EQ(engine_->checkpoint_counters().failed, 1U);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U) << "no slot is written after the log fsync failed";
  const size_t fsyncs = io_->count(Op::Fsync);
  write_blocks(3, 6);
  Status f = engine_->flush();
  EXPECT_EQ(f.code(), StatusCode::Failed) << f.message();
  EXPECT_EQ(io_->count(Op::Fsync), fsyncs) << "D10: never fsync again after a failed fsync";
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
}

TEST_F(CheckpointFailureTest, NoCheckpointIsAttemptedAfterAFailedFlush) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 1);
  io_->fail_next(Op::Fsync);
  EXPECT_FALSE(engine_->flush().ok());
  const size_t m = mark();
  write_blocks(1, 6);
  EXPECT_EQ(checkpoint_ops_since(m).size(), 0U);
  EXPECT_EQ(engine_->checkpoint_counters().failed, 0U) << "not attempted, so not counted";
}

TEST_F(CheckpointFailureTest, NoCheckpointIsAttemptedAfterAnAppendFailedState) {
  ASSERT_TRUE(start_small().ok());
  write_blocks(0, 1);
  // The append is cut short and the cleanup truncate fails too, so the log is broken.
  io_->fail_next(Op::Pwrite, StatusCode::IoError, 100);
  io_->fail_next(Op::Ftruncate);
  EXPECT_FALSE(write_block(1, 2).ok());
  const size_t m = mark();
  for (uint64_t b = 2; b < 8; ++b) {
    EXPECT_FALSE(write_block(b, 3).ok());
  }
  EXPECT_EQ(checkpoint_ops_since(m).size(), 0U);
}

// --- Fallback: a fresh checkpoint when no slot is usable (FR-016) -------------------------

TEST_F(EngineCheckpointTest,
       WithNoValidSlotAndAnUnreadableOneAFreshCheckpointIsWrittenBeforeAnyRequest) {
  test::LogBuilder log = log_of(5);
  load(log);
  std::string a = make_slot(log.bytes(), 1, 3);
  a[100] ^= 0x01;
  put_slot(*io_, 0, a);
  ASSERT_TRUE(start(test::kBigCap, kDefaultCheckpointInterval).ok());
  EXPECT_EQ(io_->count(Op::CkptPwrite), 1U);
  EXPECT_EQ(engine_->checkpoint_counters().written, 1U);
  EXPECT_EQ(engine_->checkpoint_counters().unreadable, 1U);
  DecodedSlot s = slot(0);
  ASSERT_EQ(s.state, SlotState::Valid) << s.reason;
  EXPECT_EQ(s.header.covered_seq, 5U);
  EXPECT_EQ(s.header.last_record_offset, 4 * kDataRecordSize);
  EXPECT_EQ(s.header.generation, 1U);
}

TEST_F(EngineCheckpointTest, TheFreshCheckpointAfterFullScanCoversALogWithATornTailCutOff) {
  test::LogBuilder log = log_of(5);
  load(log);
  std::string a = make_slot(log.bytes(), 1, 3);
  a[100] ^= 0x01;
  put_slot(*io_, 0, a);
  io_->bytes().append(100, '\0');
  ASSERT_TRUE(start(test::kBigCap, kDefaultCheckpointInterval).ok());
  DecodedSlot s = slot(0);
  ASSERT_EQ(s.state, SlotState::Valid) << s.reason;
  EXPECT_EQ(s.header.covered_seq, 5U);
}

TEST_F(EngineCheckpointTest, IfTheFreshCheckpointFailsTheServerStillStartsAndWarns) {
  test::LogBuilder log = log_of(5);
  load(log);
  std::string a = make_slot(log.bytes(), 1, 3);
  a[100] ^= 0x01;
  put_slot(*io_, 0, a);
  io_->fail_next(Op::CkptPwrite);
  ASSERT_TRUE(start(test::kBigCap, kDefaultCheckpointInterval).ok());
  EXPECT_EQ(engine_->checkpoint_counters().failed, 1U);
  EXPECT_EQ(engine_->checkpoint_counters().written, 0U);
  expect_block(2, 3);
  std::vector<std::string> warnings = engine_->take_warnings();
  ASSERT_EQ(warnings.size(), 1U);
  EXPECT_NE(warnings[0].find("checkpoint"), std::string::npos) << warnings[0];
}

TEST_F(EngineCheckpointTest, WithAValidSlotNoFreshCheckpointIsWritten) {
  test::LogBuilder log = log_of(5);
  load(log);
  put_slot(*io_, 0, make_slot(log.bytes(), 1, 3));
  std::string b = make_slot(log.bytes(), 2, 4);
  b[100] ^= 0x01;
  put_slot(*io_, 1, b);
  ASSERT_TRUE(start(test::kBigCap, kDefaultCheckpointInterval).ok());
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
}

TEST_F(EngineCheckpointTest, ANewDeviceWritesNoCheckpointAtStartup) {
  ASSERT_TRUE(start_small().ok());
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
  EXPECT_EQ(io_->count(Op::OpenCkpt), 1U) << "recovery looked; it did not create the file";
  EXPECT_FALSE(io_->ckpt_exists());
}

// --- Durations ------------------------------------------------------------------------------

TEST_F(EngineCheckpointTest, DurationsCoverCompletedCheckpointsOnly) {
  ASSERT_TRUE(start_small().ok());
  CheckpointCounters c = engine_->checkpoint_counters();
  EXPECT_EQ(c.duration_count, 0U);
  EXPECT_EQ(c.duration_sum_us, 0U);
  write_blocks(0, 6);
  c = engine_->checkpoint_counters();
  EXPECT_EQ(c.written, 2U);
  EXPECT_EQ(c.duration_count, 2U);
  EXPECT_LE(c.duration_max_us, c.duration_sum_us);

  io_->fail_next(Op::CkptPwrite);
  write_blocks(6, 3);
  CheckpointCounters after = engine_->checkpoint_counters();
  EXPECT_EQ(after.failed, 1U);
  EXPECT_EQ(after.written, 2U);
  EXPECT_EQ(after.duration_count, 2U) << "a failed checkpoint is not a completed one";
  EXPECT_EQ(after.duration_sum_us, c.duration_sum_us);
  EXPECT_EQ(after.duration_max_us, c.duration_max_us);
}

}  // namespace
}  // namespace logblock
