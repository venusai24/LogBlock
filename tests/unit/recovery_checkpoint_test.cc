// Startup recovery from checkpoints (M2, user story 3): the slot table and the startup table in
// specs/002-crash-recovery/data-model.md, one row at a time. Slots are built with the production
// encoder (encode_slot, tested alone in checkpoint_test.cc) and placed in FaultIo's checkpoint
// file; logs are built with LogBuilder.
#include <crc32c/crc32c.h>
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
using test::FaultIo;
using test::kCkptPath;
using test::kLogPath;
using test::LogBuilder;
using test::make_slot;
using test::put_slot;
using Op = FaultIo::Op;

// 25 records: DATA for blocks 0..19 (seq 1..20, block b has seed seq), a TRIM of blocks 3 and 4
// (seq 21), and DATA for blocks 30..33 (seq 22..25).
LogBuilder make_log() {
  LogBuilder log;
  for (uint64_t b = 0; b < 20; ++b) {
    log.data(b, static_cast<uint8_t>(b + 1));
  }
  log.trim(3, 2);
  for (uint64_t b = 30; b < 34; ++b) {
    log.data(b, static_cast<uint8_t>(b));
  }
  return log;
}

class CheckpointRecoveryTest : public test::RecoveryFixture {
 protected:
  void SetUp() override {
    RecoveryFixture::SetUp();
    log_ = make_log();
    io_->bytes() = log_.bytes();
  }

  // The expected contents of the whole device after replaying records 1..upto_seq.
  void expect_state_after(uint64_t upto_seq) {
    test::MapAt at = test::map_at(log_.bytes(), upto_seq);
    for (uint64_t b = 0; b < 40; ++b) {
      if (at.map[b] == BlockMap::kUnmapped) {
        expect_zero_block(b);
      } else {
        // The record at that offset holds the block's data: seed is seq for 0..19, b for 30..33.
        expect_block(b, b < 20 ? static_cast<uint8_t>(b + 1) : static_cast<uint8_t>(b));
      }
    }
  }

  // The ckpt-related file work that a refused start must not have done.
  void expect_nothing_changed(const std::string& log_before, const std::string& ckpt_before) {
    EXPECT_EQ(io_->bytes(), log_before);
    EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
    EXPECT_EQ(io_->count(Op::Pwrite), 0U);
    EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
    EXPECT_EQ(io_->count(Op::Fsync), 0U);
    EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
    EXPECT_EQ(io_->count(Op::CkptFsync), 0U);
  }

  // Breaks a slot in place without fixing its CRC.
  static void flip(std::string& slot, size_t byte) { slot[byte] ^= 0x01; }

  LogBuilder log_;
};

// --- Missing slots are silent -------------------------------------------------------

TEST_F(CheckpointRecoveryTest, AnAbsentCheckpointFileIsSilentAndAFullScanRuns) {
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::FullScan);
  EXPECT_EQ(r.unreadable_checkpoints, 0U);
  EXPECT_TRUE(r.warnings.empty());
  EXPECT_EQ(engine_->checkpoint_counters().unreadable, 0U);
  EXPECT_EQ(r.records_replayed, 25U);
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, ACheckpointFileEndingAtOrBeforeASlotsFirstByteIsMissing) {
  // An empty file: both slots are missing. A file that is exactly slot A long: B is missing.
  io_->set_ckpt_exists(true);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(engine_->recovery_report().unreadable_checkpoints, 0U);
  EXPECT_TRUE(engine_->recovery_report().warnings.empty());

  restart();
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  ASSERT_EQ(io_->ckpt_bytes().size(), kCheckpointSlotSize);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::Checkpoint);
  EXPECT_EQ(r.slot, 'A');
  EXPECT_EQ(r.unreadable_checkpoints, 0U) << "slot B is missing, which is not counted";
  EXPECT_TRUE(r.warnings.empty());
}

// --- Unreadable slots warn, count, and the other slot is used -----------------------

struct Damage {
  const char* name;
  void (*apply)(std::string& slot);
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): plain literals and captureless lambdas.
const Damage kUnreadableDamage[] = {
    {"wrong magic", [](std::string& s) { s[0] = 'X'; }},
    {"unknown version", [](std::string& s) { s[4] = 9; }},
    {"block_count above capacity",
     [](std::string& s) {
       s[34] = 0x05;  // 0x50000 = 327,680 > 262,144 (byte 2 of the little-endian block_count)
     }},
    {"bad crc", [](std::string& s) { s[kCheckpointHeaderSize + 100] ^= 0x01; }},
    {"torn: a bit changed in the header", [](std::string& s) { s[10] ^= 0x01; }},
};

TEST_F(CheckpointRecoveryTest, AnUnreadableSlotIsIgnoredWithAWarningAndTheOtherSlotIsUsed) {
  for (const Damage& d : kUnreadableDamage) {
    for (int bad = 0; bad < 2; ++bad) {  // the damaged slot is A, then B
      SCOPED_TRACE(std::string(d.name) + (bad == 0 ? " in slot A" : " in slot B"));
      restart();
      io_->bytes() = log_.bytes();
      io_->ckpt_bytes().clear();
      std::string good = make_slot(log_.bytes(), 5, 10);
      std::string damaged = make_slot(log_.bytes(), 6, 20);  // would have been the newest
      d.apply(damaged);
      put_slot(*io_, bad, damaged);
      put_slot(*io_, 1 - bad, good);
      ASSERT_TRUE(start().ok());

      const RecoveryReport& r = engine_->recovery_report();
      EXPECT_EQ(r.source, RecoverySource::Checkpoint);
      EXPECT_EQ(r.slot, bad == 0 ? 'B' : 'A');
      EXPECT_EQ(r.generation, 5U);
      EXPECT_EQ(r.unreadable_checkpoints, 1U);
      EXPECT_EQ(engine_->checkpoint_counters().unreadable, 1U);
      ASSERT_EQ(r.warnings.size(), 1U);
      EXPECT_NE(r.warnings[0].find(std::string("slot=") + (bad == 0 ? "A" : "B")),
                std::string::npos)
          << r.warnings[0];
      EXPECT_NE(r.warnings[0].find(kCkptPath), std::string::npos) << r.warnings[0];
      EXPECT_NE(r.warnings[0].find("unreadable"), std::string::npos) << r.warnings[0];
      // Records 11..25 were replayed after the older checkpoint.
      EXPECT_EQ(r.records_replayed, 15U);
      expect_state_after(25);
    }
  }
}

TEST_F(CheckpointRecoveryTest, AFileEndingPartwayThroughTheSecondSlotLeavesItUnreadable) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  std::string second = make_slot(log_.bytes(), 2, 20);
  put_slot(*io_, 1, second.substr(0, kCheckpointSlotSize / 2));  // a torn write of slot B
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.slot, 'A');
  EXPECT_EQ(r.unreadable_checkpoints, 1U);
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, WithTheNewestSlotUnreadableTheScanIsBoundedByTwoIntervals) {
  // SC-005: checkpoints were taken every 10 records. The newest (record 20) is unreadable, so
  // the older one (record 10) is used, and what is scanned is at most two intervals plus a record.
  const uint64_t interval = test::map_at(log_.bytes(), 10).end;  // 10 records of log growth
  std::string newest = make_slot(log_.bytes(), 2, 20);
  flip(newest, kCheckpointHeaderSize + 7);
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  put_slot(*io_, 1, newest);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.log_bytes_scanned, log_.end() - test::map_at(log_.bytes(), 10).end);
  EXPECT_LE(r.log_bytes_scanned, 2 * interval + kDataRecordSize);
}

TEST_F(CheckpointRecoveryTest, AFailedSlotReadMakesThatSlotUnreadableAndTheOtherIsUsed) {
  // D32: an I/O error reading a slot is not a refusal.
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  put_slot(*io_, 1, make_slot(log_.bytes(), 2, 20));
  // The first ckpt_pread is slot A's (slots are read in order), so slot B becomes the only one.
  io_->fail_next(Op::CkptPread, StatusCode::IoError);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.slot, 'B');
  EXPECT_EQ(r.generation, 2U);
  EXPECT_EQ(r.unreadable_checkpoints, 1U);
  ASSERT_EQ(r.warnings.size(), 1U);
  EXPECT_NE(r.warnings[0].find("slot=A"), std::string::npos) << r.warnings[0];
  expect_state_after(25);
}

// --- Open errors refuse; read errors after a successful open do not (D34, D32) ---------------

// The checkpoint file is opened read-write, once. ENOENT means the file is missing (silent). Any
// other error opening it (permissions, a directory in the way) means we cannot tell what the
// checkpoint holds, and starting anyway could serve an older state than a checkpoint proves, so
// the start is refused with the path and the OS error.
TEST_F(CheckpointRecoveryTest, AnOpenErrorOtherThanNotFoundRefusesStartupNamingThePath) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  put_slot(*io_, 1, make_slot(log_.bytes(), 2, 20));
  io_->fail_next(Op::OpenCkpt, StatusCode::IoError);
  const std::string log_before = io_->bytes();
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::IoError) << s.message();
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(kCkptPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("injected failure in open_ckpt"), std::string::npos)
      << "the OS error (here the fake's text) must reach the operator: " << s.message();
  expect_nothing_changed(log_before, ckpt_before);
}

TEST_F(CheckpointRecoveryTest, AnOpenErrorRefusesEvenWhenTheLogIsMissing) {
  // An unwritable <log>.ckpt beside a missing log: we cannot tell whether a valid checkpoint is
  // hiding in it, so a new empty device must not be started (it could silently replace one).
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  io_->bytes().clear();
  io_->set_log_exists(false);
  io_->fail_next(Op::OpenCkpt, StatusCode::IoError);
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(kCkptPath), std::string::npos) << s.message();
  EXPECT_FALSE(io_->log_exists()) << "a refused start must not create the log";
  EXPECT_EQ(io_->count(Op::CreateLog), 0U);
  EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
}

TEST_F(CheckpointRecoveryTest, AnOpenErrorRefusesAnEmptyLogToo) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  io_->bytes().clear();
  io_->set_log_exists(true);
  io_->fail_next(Op::OpenCkpt, StatusCode::IoError);
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
}

TEST_F(CheckpointRecoveryTest, ANotFoundOpenIsSilentAndStartsFromTheLog) {
  // The control for the refusals above: ENOENT (no checkpoint file at all) is not an error.
  ASSERT_FALSE(io_->ckpt_exists());
  ASSERT_TRUE(start().ok());
  EXPECT_TRUE(engine_->recovery_report().warnings.empty());
  EXPECT_EQ(engine_->recovery_report().unreadable_checkpoints, 0U);
  EXPECT_EQ(engine_->recovery_report().source, RecoverySource::FullScan);
}

TEST_F(CheckpointRecoveryTest, AFailedSizeAfterASuccessfulOpenMakesBothSlotsUnreadable) {
  // Errors after the open succeeded stay unreadable slots (D32): the file is there and was
  // opened, so the log can still serve.
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  put_slot(*io_, 1, make_slot(log_.bytes(), 2, 20));
  io_->fail_next(Op::CkptSize, StatusCode::IoError);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::FullScan);
  EXPECT_EQ(r.unreadable_checkpoints, 2U);
  EXPECT_EQ(r.warnings.size(), 2U);
  EXPECT_EQ(r.records_replayed, 25U);
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, UnreadableSlotsWithRecordsFallBackToAFullScanWithAWarning) {
  std::string a = make_slot(log_.bytes(), 1, 10);
  std::string b = make_slot(log_.bytes(), 2, 20);
  flip(a, 100);
  flip(b, 100);
  put_slot(*io_, 0, a);
  put_slot(*io_, 1, b);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::FullScan);
  EXPECT_EQ(r.unreadable_checkpoints, 2U);
  EXPECT_EQ(r.warnings.size(), 2U);
  EXPECT_EQ(r.log_bytes_scanned, log_.end());
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, UnreadableSlotsAndNoRecordsStartANewDevice) {
  io_->bytes().clear();
  std::string a = make_slot(log_.bytes(), 1, 10);
  flip(a, 100);
  put_slot(*io_, 0, a);
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::None);
  EXPECT_EQ(r.unreadable_checkpoints, 1U);
  EXPECT_EQ(r.warnings.size(), 1U);
  expect_zero_block(0);
}

// --- Inconsistent slots refuse, with no fallback (FR-017) ---------------------------

class InconsistentSlotTest : public CheckpointRecoveryTest {
 protected:
  // Starts with `a` in slot A and `b` in slot B (an empty string leaves it out), expecting a
  // refusal that names the slot and the checkpoint file and changes nothing.
  void expect_refused(const std::string& a, const std::string& b, char bad_slot) {
    if (!a.empty()) {
      put_slot(*io_, 0, a);
    }
    if (!b.empty()) {
      put_slot(*io_, 1, b);
    }
    const std::string log_before = io_->bytes();
    const std::string ckpt_before = io_->ckpt_bytes();
    Status s = start();
    EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.message();
    EXPECT_EQ(engine_, nullptr);
    EXPECT_NE(s.message().find(kCkptPath), std::string::npos) << s.message();
    EXPECT_NE(s.message().find(std::string("slot=") + bad_slot), std::string::npos)
        << s.message();
    EXPECT_NE(s.message().find("inconsistent"), std::string::npos) << s.message();
    EXPECT_EQ(s.message().find("other one looks fine"), std::string::npos)
        << "only true when the other slot is fine: " << s.message();
    expect_nothing_changed(log_before, ckpt_before);
  }

  // A slot whose header fields and map are exactly as given, with a correct CRC, so only the
  // consistency checks can reject it.
  static std::string slot_from(uint64_t generation, uint64_t last_record_offset,
                               uint64_t covered_seq, const std::vector<uint64_t>& map) {
    std::string slot(kCheckpointSlotSize, '\0');
    encode_slot({generation, last_record_offset, covered_seq},
                std::span<const uint64_t, kBlockCount>(map.data(), kBlockCount),
                std::span<uint8_t, kCheckpointSlotSize>(reinterpret_cast<uint8_t*>(slot.data()),
                                                        kCheckpointSlotSize));
    return slot;
  }

  // Rewrites bytes of a good slot and recomputes the CRC (header bytes 0-43, then the map).
  static std::string resealed(std::string slot, size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      slot[offset + i] = static_cast<char>(value >> (8 * i));
    }
    const auto* p = reinterpret_cast<const uint8_t*>(slot.data());
    uint32_t crc = crc32c::Crc32c(p, 44);
    crc = crc32c::Extend(crc, p + kCheckpointHeaderSize, slot.size() - kCheckpointHeaderSize);
    for (int i = 0; i < 4; ++i) {
      slot[44 + i] = static_cast<char>(crc >> (8 * i));
    }
    return slot;
  }
};

TEST_F(InconsistentSlotTest, AGoodCrcWithADifferentBlockSizeRefuses) {
  expect_refused(resealed(make_slot(log_.bytes(), 1, 10), 40, 8192, 4), "", 'A');
}

TEST_F(InconsistentSlotTest, AGoodCrcWithADifferentBlockCountRefuses) {
  // A smaller count: the CRC covers the shorter length, so it passes only if resealed over it.
  std::string slot = make_slot(log_.bytes(), 1, 10);
  slot.resize(kCheckpointHeaderSize + size_t{8} * 1000);
  expect_refused(resealed(std::move(slot), 32, 1000, 8), "", 'A');
}

TEST_F(InconsistentSlotTest, AMapEntryPastLastRecordOffsetRefuses) {
  test::MapAt at = test::map_at(log_.bytes(), 10);
  std::vector<uint64_t> map = at.map;
  map[100] = at.last_record_offset + 1;  // a record that starts after the last covered one
  expect_refused(slot_from(1, at.last_record_offset, 10, map), "", 'A');
}

TEST_F(InconsistentSlotTest, ALastRecordOffsetInTheMiddleOfARecordRefuses) {
  test::MapAt at = test::map_at(log_.bytes(), 10);
  expect_refused(slot_from(1, at.last_record_offset + 1, 10, at.map), "", 'A');
}

TEST_F(InconsistentSlotTest, ACoveredSeqThatTheRecordAtThatOffsetDoesNotHaveRefuses) {
  test::MapAt at = test::map_at(log_.bytes(), 10);
  expect_refused(slot_from(1, at.last_record_offset, 11, at.map), "", 'A');
}

TEST_F(InconsistentSlotTest, ALastRecordOffsetPastTheEndOfTheLogRefuses) {
  test::MapAt at = test::map_at(log_.bytes(), 10);
  expect_refused(slot_from(1, log_.end() + 4096, 10, at.map), "", 'A');
}

TEST_F(InconsistentSlotTest, AnInconsistentOlderSlotRefusesEvenWhenTheNewerOneIsGood) {
  test::MapAt at = test::map_at(log_.bytes(), 10);
  std::vector<uint64_t> map = at.map;
  map[100] = at.last_record_offset + 1;
  expect_refused(slot_from(1, at.last_record_offset, 10, map), make_slot(log_.bytes(), 2, 20),
                 'A');
}

TEST_F(InconsistentSlotTest, AnInconsistentNewerSlotRefusesEvenWhenTheOlderOneIsGood) {
  test::MapAt at = test::map_at(log_.bytes(), 20);
  expect_refused(make_slot(log_.bytes(), 1, 10),
                 slot_from(2, at.last_record_offset, 19, at.map), 'B');
}

TEST_F(InconsistentSlotTest, ControlAConsistentPairStarts) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  put_slot(*io_, 1, make_slot(log_.bytes(), 2, 20));
  ASSERT_TRUE(start().ok());
}

// --- Valid slots: which one wins and where replay starts ----------------------------

TEST_F(CheckpointRecoveryTest, TheHighestValidGenerationWinsInEitherSlot) {
  for (int newest = 0; newest < 2; ++newest) {
    SCOPED_TRACE(newest == 0 ? "newest in A" : "newest in B");
    restart();
    io_->bytes() = log_.bytes();
    io_->ckpt_bytes().clear();
    put_slot(*io_, newest, make_slot(log_.bytes(), 8, 20));
    put_slot(*io_, 1 - newest, make_slot(log_.bytes(), 7, 10));
    ASSERT_TRUE(start().ok());
    const RecoveryReport& r = engine_->recovery_report();
    EXPECT_EQ(r.source, RecoverySource::Checkpoint);
    EXPECT_EQ(r.slot, newest == 0 ? 'A' : 'B');
    EXPECT_EQ(r.generation, 8U);
    EXPECT_EQ(r.records_replayed, 5U);  // seq 21..25
    EXPECT_EQ(r.unreadable_checkpoints, 0U);
    EXPECT_TRUE(r.warnings.empty());
    expect_state_after(25);
  }
}

TEST_F(CheckpointRecoveryTest, ReplayStartsJustPastTheCoveredRecordAndScansOnlyTheRest) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 20));
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.records_replayed, 5U);
  EXPECT_EQ(r.log_bytes_scanned, log_.end() - test::map_at(log_.bytes(), 20).end);
  // The earlier records were never read: only the 5 later ones, plus nothing else.
  EXPECT_EQ(io_->bytes(), log_.bytes());
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, ACheckpointCoveringTheWholeLogReplaysNothing) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 25));
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.source, RecoverySource::Checkpoint);
  EXPECT_EQ(r.records_replayed, 0U);
  EXPECT_EQ(r.log_bytes_scanned, 0U);
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, ATrimAndAnOverwriteAfterTheCheckpointApplyOverItsMap) {
  // Checkpoint at record 25; then trim block 10 and overwrite block 11 with new data.
  std::string slot = make_slot(log_.bytes(), 1, 25);
  log_.trim(10, 1);
  const uint64_t new_offset = log_.data(11, 0x77);
  (void)new_offset;
  io_->bytes() = log_.bytes();
  put_slot(*io_, 0, slot);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(engine_->recovery_report().records_replayed, 2U);
  expect_zero_block(10);
  expect_block(11, 0x77);
  expect_block(12, 13);  // untouched, from before the checkpoint
}

TEST_F(CheckpointRecoveryTest, ATornTailAfterTheCheckpointIsRemovedAndTheRestIsReplayed) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 20));
  io_->bytes().append(100, '\0');
  const std::string good = log_.bytes();
  ASSERT_TRUE(start().ok());
  const RecoveryReport& r = engine_->recovery_report();
  EXPECT_EQ(r.torn_bytes_removed, 100U);
  EXPECT_EQ(io_->bytes(), good);
  EXPECT_EQ(r.records_replayed, 5U);
  expect_state_after(25);
}

TEST_F(CheckpointRecoveryTest, CorruptionBeforeTheCoveredOffsetIsNotSeenAtStartup) {
  // The checkpoint's CRC vouches for its map; the log before it is not rescanned. A read of a
  // block that points into the damage returns Corrupt rather than wrong bytes.
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 20));
  io_->bytes()[test::map_at(log_.bytes(), 1).last_record_offset + kRecordHeaderSize + 5] ^= 0x01;
  ASSERT_TRUE(start().ok());
  Bytes out(kBlockSize);
  Status s = engine_->read(0, kBlockSize, out.data());
  EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.message();
  expect_block(1, 2);  // other blocks are fine
}

TEST_F(CheckpointRecoveryTest, CorruptionAfterTheCoveredOffsetRefuses) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  io_->bytes()[test::map_at(log_.bytes(), 14).last_record_offset + kRecordHeaderSize + 5] ^= 0x01;
  const std::string damaged = io_->bytes();
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_EQ(s.code(), StatusCode::Corrupt) << s.message();
  expect_nothing_changed(damaged, ckpt_before);
}

TEST_F(CheckpointRecoveryTest, TheCapIsJudgedOnTheWholeLogEvenWithACheckpoint) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 20));
  const std::string log_before = io_->bytes();
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start(log_.end() - 1);
  EXPECT_EQ(s.code(), StatusCode::Failed) << s.message();
  EXPECT_NE(s.message().find("raise the cap"), std::string::npos) << s.message();
  expect_nothing_changed(log_before, ckpt_before);
}

TEST_F(CheckpointRecoveryTest, StartupNeverWritesTheCheckpointFileWhenAValidSlotWasUsed) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 20));
  put_slot(*io_, 1, make_slot(log_.bytes(), 2, 22));
  const std::string ckpt_before = io_->ckpt_bytes();
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
  EXPECT_EQ(io_->count(Op::CkptPwrite), 0U);
}

// --- Missing or empty log -----------------------------------------------------------

TEST_F(CheckpointRecoveryTest, AMissingLogWithAValidCheckpointRefusesNamingBothFilesAndCreatesNothing) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  io_->bytes().clear();
  io_->set_log_exists(false);
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_EQ(engine_, nullptr);
  EXPECT_NE(s.message().find(kLogPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find(kCkptPath), std::string::npos) << s.message();
  EXPECT_NE(s.message().find("remove"), std::string::npos)
      << "the message should say how to start over: " << s.message();
  EXPECT_FALSE(io_->log_exists()) << "a missing log must not be created by a refused start";
  EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
}

TEST_F(CheckpointRecoveryTest, AnEmptyLogWithAValidCheckpointRefuses) {
  put_slot(*io_, 0, make_slot(log_.bytes(), 1, 10));
  io_->bytes().clear();
  io_->set_log_exists(true);
  const std::string ckpt_before = io_->ckpt_bytes();
  Status s = start();
  EXPECT_NE(s.code(), StatusCode::Ok);
  EXPECT_NE(s.message().find(kCkptPath), std::string::npos) << s.message();
  EXPECT_EQ(io_->ckpt_bytes(), ckpt_before);
  EXPECT_EQ(io_->count(Op::Ftruncate), 0U);
}

TEST_F(CheckpointRecoveryTest, AMissingLogWithOnlyUnreadableSlotsStartsANewDevice) {
  std::string a = make_slot(log_.bytes(), 1, 10);
  flip(a, 100);
  put_slot(*io_, 0, a);
  io_->bytes().clear();
  io_->set_log_exists(false);
  ASSERT_TRUE(start().ok());
  EXPECT_EQ(engine_->recovery_report().source, RecoverySource::None);
  EXPECT_EQ(engine_->recovery_report().unreadable_checkpoints, 1U);
  expect_zero_block(0);
}

}  // namespace
}  // namespace logblock
