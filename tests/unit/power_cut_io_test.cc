#include "support/power_cut_io.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "support/io_helpers.h"

namespace logblock::test {
namespace {

using Op = PowerCutIo::Op;

// Small helpers so each test reads as the story it tells.
Status write_log(PowerCutIo& io, uint64_t offset, const std::string& data) {
  return io.pwrite(offset, data.data(), data.size());
}
Status write_ckpt(PowerCutIo& io, uint64_t offset, const std::string& data) {
  return io.ckpt_pwrite(offset, data.data(), data.size());
}
std::string read_log(PowerCutIo& io, uint64_t offset, size_t len) {
  std::string out(len, '?');
  size_t got = 0;
  EXPECT_TRUE(io.pread(offset, out.data(), len, &got).ok());
  out.resize(got);
  return out;
}

// Drop-all is the T003 outcome: only the durable image survives.
std::unique_ptr<PowerCutIo> reboot_drop_all(PowerCutIo& io) {
  return io.reboot(PowerCutIo::Outcome{}, PowerCutIo::Outcome{});
}

// A device whose log and checkpoint file both exist durably, with known contents.
std::unique_ptr<PowerCutIo> make_io(const std::string& log = "old-log",
                                    const std::string& ckpt = "old-ckpt") {
  auto io = std::make_unique<PowerCutIo>();
  io->set_log_image(log);
  io->set_ckpt_image(ckpt);
  bool opened = false;
  bool created = false;
  EXPECT_TRUE(test::open_or_create_log(*io, "p.log").ok());
  EXPECT_TRUE(io->open_ckpt("p.log.ckpt", true, &opened, &created).ok());
  EXPECT_TRUE(opened);
  EXPECT_FALSE(created);
  return io;
}

// --- Reads see what a running process would see ----------------------------------

TEST(PowerCutIo, ReadsSeeTheDurableImageWithPendingOperationsApplied) {
  auto io = make_io("0123456789", "");
  ASSERT_TRUE(write_log(*io, 2, "AB").ok());
  EXPECT_EQ(read_log(*io, 0, 10), "01AB456789");
  ASSERT_TRUE(io->ftruncate(4).ok());
  EXPECT_EQ(read_log(*io, 0, 10), "01AB");
  ASSERT_TRUE(write_log(*io, 6, "Z").ok());  // writing past the end leaves a zero gap
  EXPECT_EQ(read_log(*io, 0, 10), std::string("01AB\0\0Z", 7));
  uint64_t size = 0;
  ASSERT_TRUE(io->file_size(&size).ok());
  EXPECT_EQ(size, 7U);
}

TEST(PowerCutIo, CheckpointFileReadsAndWritesLikeTheLog) {
  auto io = make_io("", "slot");
  ASSERT_TRUE(write_ckpt(*io, 2, "XYZ").ok());
  uint64_t size = 0;
  ASSERT_TRUE(io->ckpt_size(&size).ok());
  EXPECT_EQ(size, 5U);
  std::string out(10, '?');
  size_t got = 0;
  ASSERT_TRUE(io->ckpt_pread(0, out.data(), out.size(), &got).ok());
  EXPECT_EQ(out.substr(0, got), "slXYZ");
}

// --- Drop-all: what a cut loses --------------------------------------------------

TEST(PowerCutIo, AnUnsyncedWriteVanishes) {
  auto io = make_io("old-log", "old-ckpt");
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  ASSERT_TRUE(write_ckpt(*io, 0, "NEW").ok());
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "old-log");
  EXPECT_EQ(after->ckpt_bytes(), "old-ckpt");
}

TEST(PowerCutIo, ASyncedWriteSurvives) {
  auto io = make_io("old-log", "old-ckpt");
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  ASSERT_TRUE(io->fsync().ok());
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "NEW-log");
}

TEST(PowerCutIo, OnlyOperationsBeforeTheFsyncSurvive) {
  auto io = make_io("........", "");
  ASSERT_TRUE(write_log(*io, 0, "AA").ok());
  ASSERT_TRUE(io->fsync().ok());
  ASSERT_TRUE(write_log(*io, 2, "BB").ok());  // after the fsync: still pending
  ASSERT_TRUE(io->ftruncate(3).ok());         // a pending truncate is lost too
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "AA......");
}

TEST(PowerCutIo, ASyncedTruncateSurvivesAndKeepsItsOrderWithWrites) {
  auto io = make_io("0123456789", "");
  ASSERT_TRUE(io->ftruncate(4).ok());
  ASSERT_TRUE(write_log(*io, 6, "Z").ok());  // truncate, then write: the gap is zeros
  ASSERT_TRUE(io->fsync().ok());
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), std::string("0123\0\0Z", 7));
}

TEST(PowerCutIo, FilesAreTrackedIndependently) {
  auto io = make_io("old-log", "old-ckpt");
  ASSERT_TRUE(write_log(*io, 0, "LOG").ok());
  ASSERT_TRUE(write_ckpt(*io, 0, "CKP").ok());
  ASSERT_TRUE(io->ckpt_fsync().ok());  // only the checkpoint file is synced
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "old-log");
  EXPECT_EQ(after->ckpt_bytes(), "CKP-ckpt");

  // And the other way round: fsync on the log does not sync the checkpoint file.
  auto io2 = make_io("old-log", "old-ckpt");
  ASSERT_TRUE(write_log(*io2, 0, "LOG").ok());
  ASSERT_TRUE(write_ckpt(*io2, 0, "CKP").ok());
  ASSERT_TRUE(io2->fsync().ok());
  auto after2 = reboot_drop_all(*io2);
  EXPECT_EQ(after2->log_bytes(), "LOG-log");
  EXPECT_EQ(after2->ckpt_bytes(), "old-ckpt");
}

// --- Names ----------------------------------------------------------------------

TEST(PowerCutIo, ANewFileWhoseNameIsNotDurableDoesNotSurvive) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(write_log(io, 0, "data").ok());
  ASSERT_TRUE(io.fsync().ok());  // the data is durable, but the directory entry is not
  auto after = reboot_drop_all(io);
  EXPECT_FALSE(after->log_exists());
}

TEST(PowerCutIo, FsyncOfTheDirectoryMakesNewNamesDurable) {
  PowerCutIo io;
  bool opened = false;
  bool created = false;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(io.open_ckpt("p.log.ckpt", true, &opened, &created).ok());
  EXPECT_TRUE(created);
  ASSERT_TRUE(write_log(io, 0, "data").ok());
  ASSERT_TRUE(io.fsync().ok());
  ASSERT_TRUE(io.fsync_parent_dir().ok());
  auto after = reboot_drop_all(io);
  EXPECT_TRUE(after->log_exists());
  EXPECT_EQ(after->log_bytes(), "data");
  // The checkpoint file's name became durable too, but it had no synced data.
  EXPECT_TRUE(after->ckpt_exists());
  EXPECT_EQ(after->ckpt_bytes(), "");
}

TEST(PowerCutIo, ADurableNameWithNoSyncedDataSurvivesAsAnEmptyFile) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(write_log(io, 0, "data").ok());  // never fsynced
  ASSERT_TRUE(io.fsync_parent_dir().ok());
  auto after = reboot_drop_all(io);
  EXPECT_TRUE(after->log_exists());
  EXPECT_EQ(after->log_bytes(), "");
}

TEST(PowerCutIo, ExistenceFollowsTheRunningProcessView) {
  PowerCutIo io;
  EXPECT_FALSE(io.log_exists());
  EXPECT_FALSE(io.ckpt_exists());
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());  // created, name not yet durable
  EXPECT_TRUE(io.log_exists());
  EXPECT_FALSE(io.log_name_durable());
}

TEST(PowerCutIo, OpenCkptWithoutCreateOnAMissingFileCreatesNothing) {
  PowerCutIo io;
  bool opened = true;
  bool created = true;
  ASSERT_TRUE(io.open_ckpt("p.log.ckpt", false, &opened, &created).ok());
  EXPECT_FALSE(opened);
  EXPECT_FALSE(created);
  EXPECT_FALSE(io.ckpt_exists());
}

// --- The cut ----------------------------------------------------------------------

TEST(PowerCutIo, TheCutOperationAndAllLaterOnesFailWithoutEffect) {
  auto io = make_io("old-log", "");
  io->arm_cut(io->operation_count() + 1);  // let one more call through, then cut
  ASSERT_TRUE(write_log(*io, 0, "AAA").ok());
  Status s = write_log(*io, 0, "BBB");  // this is the cut operation
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_TRUE(io->is_cut());
  EXPECT_EQ(write_log(*io, 0, "CCC").code(), StatusCode::IoError);
  EXPECT_EQ(io->fsync().code(), StatusCode::IoError);
  uint64_t size = 0;
  EXPECT_EQ(io->file_size(&size).code(), StatusCode::IoError);
  EXPECT_EQ(io->ckpt_fsync().code(), StatusCode::IoError);
  EXPECT_EQ(io->fsync_parent_dir().code(), StatusCode::IoError);
  // Nothing after the cut took effect, and nothing before it was synced.
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "old-log");
}

TEST(PowerCutIo, AnFsyncAtTheCutSyncsNothing) {
  auto io = make_io("old-log", "old-ckpt");
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  io->arm_cut(io->operation_count());  // the very next call is the cut
  EXPECT_EQ(io->fsync().code(), StatusCode::IoError);
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "old-log");
}

TEST(PowerCutIo, AnFsyncOfTheDirectoryAtTheCutMakesNothingDurable) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(write_log(io, 0, "data").ok());
  ASSERT_TRUE(io.fsync().ok());
  io.arm_cut(io.operation_count());
  EXPECT_EQ(io.fsync_parent_dir().code(), StatusCode::IoError);
  auto after = reboot_drop_all(io);
  EXPECT_FALSE(after->log_exists());
}

TEST(PowerCutIo, OperationsAreCountedAndRecordedInOrderAcrossBothFiles) {
  auto io = make_io("", "");
  size_t start = io->operation_count();
  ASSERT_TRUE(write_log(*io, 0, "a").ok());
  ASSERT_TRUE(io->fsync().ok());
  ASSERT_TRUE(write_ckpt(*io, 0, "b").ok());
  ASSERT_TRUE(io->ckpt_fsync().ok());
  ASSERT_TRUE(io->fsync_parent_dir().ok());
  EXPECT_EQ(io->operation_count(), start + 5);
  std::vector<Op> tail(io->operations().begin() + static_cast<long>(start), io->operations().end());
  EXPECT_EQ(tail, (std::vector<Op>{Op::Pwrite, Op::Fsync, Op::CkptPwrite, Op::CkptFsync,
                                   Op::FsyncParentDir}));
}

TEST(PowerCutIo, ACutIsNotArmedUnlessAsked) {
  auto io = make_io();
  EXPECT_FALSE(io->is_cut());
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(io->fsync().ok());
  }
}

// --- Restart ----------------------------------------------------------------------

TEST(PowerCutIo, RebootGivesANewObjectWithNothingOpenPendingOrArmed) {
  auto io = make_io("old-log", "old-ckpt");
  ASSERT_TRUE(write_log(*io, 0, "AAA").ok());
  ASSERT_TRUE(io->fsync().ok());
  io->arm_cut(io->operation_count());
  EXPECT_FALSE(io->fsync().ok());  // cut

  auto after = reboot_drop_all(*io);
  EXPECT_FALSE(after->is_cut());
  EXPECT_EQ(after->operation_count(), 0U);
  char buf[4];
  size_t got = 0;
  EXPECT_EQ(after->pread(0, buf, 4, &got).code(), StatusCode::IoError) << "the log is not open";
  ASSERT_TRUE(test::open_or_create_log(*after, "p.log").ok());
  EXPECT_EQ(read_log(*after, 0, 10), "AAA-log");

  // A cut can be armed again on the rebooted object, and its own reboot works the same way.
  ASSERT_TRUE(write_log(*after, 0, "ZZZ").ok());
  after->arm_cut(after->operation_count());
  EXPECT_FALSE(after->fsync().ok());
  auto third = reboot_drop_all(*after);
  EXPECT_EQ(third->log_bytes(), "AAA-log");
}

TEST(PowerCutIo, RebootWithoutACutStillLosesUnsyncedWrites) {
  auto io = make_io("old-log", "");
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "old-log");
}

TEST(PowerCutIo, RebootDoesNotChangeTheOriginal) {
  auto io = make_io("old-log", "");
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(read_log(*io, 0, 7), "NEW-log") << "the old object still shows its running view";
}

// --- Misc contract ---------------------------------------------------------------

TEST(PowerCutIo, EveryCallBeforeItsOpenIsAnError) {
  PowerCutIo io;
  char buf[4] = {};
  size_t got = 0;
  uint64_t n = 0;
  EXPECT_EQ(io.file_size(&n).code(), StatusCode::IoError);
  EXPECT_EQ(io.pread(0, buf, 4, &got).code(), StatusCode::IoError);
  EXPECT_EQ(io.pwrite(0, buf, 4).code(), StatusCode::IoError);
  EXPECT_EQ(io.fsync().code(), StatusCode::IoError);
  EXPECT_EQ(io.ftruncate(0).code(), StatusCode::IoError);
  EXPECT_EQ(io.fsync_parent_dir().code(), StatusCode::IoError);
  EXPECT_EQ(io.lock_log().code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_size(&n).code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_pread(0, buf, 4, &got).code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_pwrite(0, buf, 4).code(), StatusCode::IoError);
  EXPECT_EQ(io.ckpt_fsync().code(), StatusCode::IoError);
}

TEST(PowerCutIo, OpeningEitherFileTwiceIsAnError) {
  auto io = make_io();
  bool opened = false;
  bool created = false;
  EXPECT_EQ(test::open_or_create_log(*io, "p.log").code(), StatusCode::IoError);
  EXPECT_EQ(io->open_ckpt("p.log.ckpt", true, &opened, &created).code(), StatusCode::IoError);
}

TEST(PowerCutIo, LockLogSucceedsOnAnOpenLog) {
  auto io = make_io();
  EXPECT_TRUE(io->lock_log().ok());  // flock is not modelled: a cut ends every holder
}


// =============================================================================
// T004: the other outcomes, zero-fill, the lying modes and the seed
// =============================================================================

using Kind = PowerCutIo::Kind;
using Outcome = PowerCutIo::Outcome;

Outcome outcome(Kind kind, std::optional<size_t> prefix_ops = std::nullopt,
                std::optional<size_t> tear_bytes = std::nullopt, bool zero_fill = false) {
  Outcome o;
  o.kind = kind;
  o.prefix_ops = prefix_ops;
  o.tear_bytes = tear_bytes;
  o.zero_fill = zero_fill;
  return o;
}

// Durable log "0123456789" with three unsynced writes:
//   w1 "AA" @0, w2 "BB" @4, w3 "CCCC" @8 (grows the file to 12 bytes).
// Running view: "AA23BB67CCCC".
std::unique_ptr<PowerCutIo> make_pending_io(uint64_t seed = 0) {
  auto io = std::make_unique<PowerCutIo>(seed);
  io->set_log_image("0123456789");
  EXPECT_TRUE(test::open_or_create_log(*io, "p.log").ok());
  EXPECT_TRUE(write_log(*io, 0, "AA").ok());
  EXPECT_TRUE(write_log(*io, 4, "BB").ok());
  EXPECT_TRUE(write_log(*io, 8, "CCCC").ok());
  EXPECT_EQ(io->log_bytes(), "AA23BB67CCCC");
  return io;
}

std::string log_after(PowerCutIo& io, const Outcome& o) {
  return io.reboot(o, Outcome{})->log_bytes();
}

// --- Random prefix ---------------------------------------------------------------

TEST(PowerCutIoOutcomes, RandomPrefixAppliesTheFirstKPendingOperations) {
  auto io = make_pending_io();
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 0)), "0123456789");
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 1)), "AA23456789");
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 2)), "AA23BB6789");
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 3)), "AA23BB67CCCC");
}

TEST(PowerCutIoOutcomes, AnExplicitPrefixLongerThanThePendingListIsClamped) {
  auto io = make_pending_io();
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 99)), "AA23BB67CCCC");
}

TEST(PowerCutIoOutcomes, TruncatesAreKeptOrDroppedInOrder) {
  PowerCutIo io;
  io.set_log_image("0123456789");
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(io.ftruncate(4).ok());         // op 0
  ASSERT_TRUE(write_log(io, 6, "Z").ok());  // op 1: after the truncate, so a zero gap
  ASSERT_TRUE(io.ftruncate(2).ok());         // op 2
  EXPECT_EQ(log_after(io, outcome(Kind::RandomPrefix, 0)), "0123456789");
  EXPECT_EQ(log_after(io, outcome(Kind::RandomPrefix, 1)), "0123");
  EXPECT_EQ(log_after(io, outcome(Kind::RandomPrefix, 2)), std::string("0123\0\0Z", 7));
  EXPECT_EQ(log_after(io, outcome(Kind::RandomPrefix, 3)), "01");
}

// --- Tear last -------------------------------------------------------------------

TEST(PowerCutIoOutcomes, TearLastAppliesKThenAShorterPrefixOfTheNextWrite) {
  auto io = make_pending_io();
  // K = 1 (w1 whole), then 1 of w2's 2 bytes: new byte, then the old one that was there.
  EXPECT_EQ(log_after(*io, outcome(Kind::TearLast, 1, 1)), "AA23B56789");
  // K = 2, then 2 of w3's 4 bytes. The file ends where the torn write ends, not at 12.
  EXPECT_EQ(log_after(*io, outcome(Kind::TearLast, 2, 2)), "AA23BB67CC");
  // K = 0 and nothing of w1: identical to drop-all.
  EXPECT_EQ(log_after(*io, outcome(Kind::TearLast, 0, 0)), "0123456789");
}

TEST(PowerCutIoOutcomes, ATearIsAlwaysShorterThanTheWholeWrite) {
  auto io = make_pending_io();
  // Asking for 100 bytes of w3 gives 3: a tear never contributes the full write.
  EXPECT_EQ(log_after(*io, outcome(Kind::TearLast, 2, 100)), "AA23BB67CCC");
  // K cannot be the whole list either, since a write must remain to be torn.
  EXPECT_EQ(log_after(*io, outcome(Kind::TearLast, 99, 2)), "AA23BB67CC");
}

TEST(PowerCutIoOutcomes, TearLastBeforeATruncateAppliesOnlyThePrefix) {
  PowerCutIo io;
  io.set_log_image("0123456789");
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(write_log(io, 0, "AA").ok());
  ASSERT_TRUE(io.ftruncate(4).ok());  // the op that would be torn: truncates do not tear
  EXPECT_EQ(log_after(io, outcome(Kind::TearLast, 1, 1)), "AA23456789");
}

TEST(PowerCutIoOutcomes, TearLastWithNothingPendingIsTheDurableImage) {
  PowerCutIo io;
  io.set_log_image("0123");
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  EXPECT_EQ(log_after(io, outcome(Kind::TearLast)), "0123");
}

// --- Zero-fill -------------------------------------------------------------------

TEST(PowerCutIoOutcomes, ZeroFillDropAllKeepsTheWouldBeLengthWithZeros) {
  auto io = make_pending_io();
  EXPECT_EQ(log_after(*io, outcome(Kind::DropAll, std::nullopt, std::nullopt, true)),
            std::string("0123456789\0\0", 12));
}

TEST(PowerCutIoOutcomes, ZeroFillRandomPrefixZeroesWhatWasNotWritten) {
  auto io = make_pending_io();
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 1, std::nullopt, true)),
            std::string("AA23456789\0\0", 12));
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 2, std::nullopt, true)),
            std::string("AA23BB6789\0\0", 12));
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 3, std::nullopt, true)),
            "AA23BB67CCCC");
}

TEST(PowerCutIoOutcomes, ZeroFillFollowsAPendingTruncateToTheShorterLength) {
  PowerCutIo io;
  io.set_log_image("0123456789");
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(io.ftruncate(4).ok());
  EXPECT_EQ(log_after(io, outcome(Kind::DropAll, std::nullopt, std::nullopt, true)), "0123");
}

TEST(PowerCutIoOutcomes, WithoutZeroFillTheFileKeepsOnlyWhatWasApplied) {
  auto io = make_pending_io();
  EXPECT_EQ(log_after(*io, outcome(Kind::RandomPrefix, 1)).size(), 10U);
}

// --- Per file --------------------------------------------------------------------

TEST(PowerCutIoOutcomes, EachFileGetsItsOwnOutcome) {
  PowerCutIo io;
  io.set_log_image("old-log");
  io.set_ckpt_image("old-ckpt");
  bool opened = false;
  bool created = false;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(io.open_ckpt("p.log.ckpt", true, &opened, &created).ok());
  ASSERT_TRUE(write_log(io, 0, "LOG").ok());
  ASSERT_TRUE(write_ckpt(io, 0, "CKP").ok());

  auto a = io.reboot(outcome(Kind::DropAll), outcome(Kind::RandomPrefix, 1));
  EXPECT_EQ(a->log_bytes(), "old-log");
  EXPECT_EQ(a->ckpt_bytes(), "CKP-ckpt");
  auto b = io.reboot(outcome(Kind::RandomPrefix, 1), outcome(Kind::DropAll));
  EXPECT_EQ(b->log_bytes(), "LOG-log");
  EXPECT_EQ(b->ckpt_bytes(), "old-ckpt");
}

TEST(PowerCutIoOutcomes, SyncedDataSurvivesEveryOutcome) {
  PowerCutIo io;
  io.set_log_image("old");
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(write_log(io, 0, "NEW").ok());
  ASSERT_TRUE(io.fsync().ok());
  for (Kind k : {Kind::DropAll, Kind::RandomPrefix, Kind::TearLast}) {
    for (bool zero : {false, true}) {
      EXPECT_EQ(log_after(io, outcome(k, std::nullopt, std::nullopt, zero)), "NEW");
    }
  }
}

TEST(PowerCutIoOutcomes, AFileWithANonDurableNameStillDoesNotSurviveAnyOutcome) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(write_log(io, 0, "data").ok());
  ASSERT_TRUE(io.fsync().ok());
  EXPECT_FALSE(io.reboot(outcome(Kind::RandomPrefix, 1), Outcome{})->log_exists());
}

// --- The seed --------------------------------------------------------------------

struct Snapshot {
  std::string log;
  std::string ckpt;
  PowerCutIo::Outcomes outcomes;
};

// A device with several pending operations in both files, rebooted by the seed.
Snapshot seeded_reboot(uint64_t seed) {
  PowerCutIo io(seed);
  io.set_log_image("0123456789");
  io.set_ckpt_image("abcdefghij");
  bool opened = false;
  bool created = false;
  EXPECT_TRUE(test::open_or_create_log(io, "p.log").ok());
  EXPECT_TRUE(io.open_ckpt("p.log.ckpt", true, &opened, &created).ok());
  const struct {
    const char* data;
    uint64_t offset;
  } writes[] = {{"AA", 3}, {"BB", 6}, {"CCCC", 0}, {"DD", 8}};
  for (const auto& w : writes) {
    EXPECT_TRUE(write_log(io, w.offset, w.data).ok());
    EXPECT_TRUE(write_ckpt(io, w.offset + 1, w.data).ok());
  }
  EXPECT_TRUE(io.ftruncate(8).ok());
  auto after = io.reboot();
  return {after->log_bytes(), after->ckpt_bytes(), after->boot_outcomes()};
}

TEST(PowerCutIoSeed, TheSameSeedGivesIdenticalSurvivorsAndOutcomes) {
  for (uint64_t seed = 0; seed < 50; ++seed) {
    PowerCutIo a(seed);
    PowerCutIo b(seed);
    for (PowerCutIo* io : {&a, &b}) {
      io->set_log_image("0123456789");
      io->set_ckpt_image("abcdefghij");
      bool opened = false;
      bool created = false;
      ASSERT_TRUE(test::open_or_create_log(*io, "p.log").ok());
      ASSERT_TRUE(io->open_ckpt("p.log.ckpt", true, &opened, &created).ok());
      ASSERT_TRUE(write_log(*io, 1, "AA").ok());
      ASSERT_TRUE(write_log(*io, 5, "BBBB").ok());
      ASSERT_TRUE(write_ckpt(*io, 2, "CC").ok());
      ASSERT_TRUE(io->ftruncate(7).ok());
      ASSERT_TRUE(write_ckpt(*io, 6, "DDDD").ok());
    }
    auto ra = a.reboot();
    auto rb = b.reboot();
    EXPECT_EQ(ra->log_bytes(), rb->log_bytes()) << "seed " << seed;
    EXPECT_EQ(ra->ckpt_bytes(), rb->ckpt_bytes()) << "seed " << seed;
    EXPECT_EQ(ra->boot_outcomes().log.kind, rb->boot_outcomes().log.kind);
    EXPECT_EQ(ra->boot_outcomes().log.prefix_ops, rb->boot_outcomes().log.prefix_ops);
    // Chained reboots are deterministic too: the next object's seed comes from this one's.
    EXPECT_EQ(ra->reboot()->log_bytes(), rb->reboot()->log_bytes()) << "seed " << seed;
  }
}

TEST(PowerCutIoSeed, TheSeedChoosesEveryOutcomeAndFilesIndependently) {
  std::set<Kind> log_kinds;
  std::set<Kind> ckpt_kinds;
  std::set<bool> zero_fills;
  std::set<std::string> distinct_logs;
  bool differ = false;
  for (uint64_t seed = 0; seed < 200; ++seed) {
    Snapshot s = seeded_reboot(seed);
    log_kinds.insert(s.outcomes.log.kind);
    ckpt_kinds.insert(s.outcomes.ckpt.kind);
    zero_fills.insert(s.outcomes.log.zero_fill);
    distinct_logs.insert(s.log);
    differ = differ || s.outcomes.log.kind != s.outcomes.ckpt.kind;
  }
  EXPECT_EQ(log_kinds.size(), 3U);
  EXPECT_EQ(ckpt_kinds.size(), 3U);
  EXPECT_EQ(zero_fills.size(), 2U);
  EXPECT_GT(distinct_logs.size(), 5U) << "different seeds should give different survivors";
  EXPECT_TRUE(differ) << "the two files should not always share one outcome";
}

TEST(PowerCutIoSeed, ResolvedOutcomesReplayToTheSameBytes) {
  // boot_outcomes() records the K and tear length the seed picked. Feeding them back through
  // the explicit reboot must reproduce the survivors, so a failing seed can be reported and
  // replayed without the rng.
  for (uint64_t seed = 0; seed < 100; ++seed) {
    PowerCutIo io(seed);
    io.set_log_image("0123456789");
    ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
    ASSERT_TRUE(write_log(io, 0, "AAAA").ok());
    ASSERT_TRUE(write_log(io, 6, "BBBB").ok());
    ASSERT_TRUE(io.ftruncate(9).ok());
    ASSERT_TRUE(write_log(io, 2, "CCCCCC").ok());
    auto seeded = io.reboot();
    const auto& o = seeded->boot_outcomes();
    ASSERT_TRUE(o.log.prefix_ops.has_value());
    EXPECT_LE(*o.log.prefix_ops, 4U);
    if (o.log.kind == Kind::TearLast) {
      EXPECT_LE(*o.log.prefix_ops, 3U);  // a write must remain to be torn
      // Pending op 2 is the truncate, and a truncate is not torn.
      EXPECT_EQ(o.log.tear_bytes.has_value(), *o.log.prefix_ops != 2U) << "seed " << seed;
    }
    // io.reboot() consumed rng draws, but explicit outcomes do not use the rng.
    auto replay = io.reboot(o.log, o.ckpt);
    EXPECT_EQ(replay->log_bytes(), seeded->log_bytes()) << "seed " << seed;
  }
}

TEST(PowerCutIoSeed, AFixedSeedPinsItsSurvivors) {
  // Pins the exact bytes for one seed. The rng is std::mt19937_64 and every pick is `% n` rather
  // than a std distribution, whose results vary between standard libraries, so this holds on any
  // platform. If it changes, the seeded runs have changed what they simulate.
  // Seed 20 tears both files, so every part of the model shows. Worked by hand from the pending
  // list in seeded_reboot(): log writes AA@3, BB@6, CCCC@0, DD@8, truncate 8, over "0123456789".
  //   log:  K = 2 keeps AA and BB, then 1 byte of CCCC lands at 0       -> "C12AA5BB89"
  //   ckpt: the same writes shifted by 1 over "abcdefghij": K = 1 keeps AA@4, then 1 byte of
  //         BB lands at 7                                                -> "abcdAAgBij"
  Snapshot s = seeded_reboot(20);
  EXPECT_EQ(s.outcomes.log.kind, Kind::TearLast);
  EXPECT_EQ(s.outcomes.log.prefix_ops, 2U);
  EXPECT_EQ(s.outcomes.log.tear_bytes, 1U);
  EXPECT_EQ(s.outcomes.ckpt.kind, Kind::TearLast);
  EXPECT_EQ(s.outcomes.ckpt.prefix_ops, 1U);
  EXPECT_EQ(s.outcomes.ckpt.tear_bytes, 1U);
  EXPECT_EQ(s.log, "C12AA5BB89");
  EXPECT_EQ(s.ckpt, "abcdAAgBij");
}

// --- Lying modes (FR-035) ---------------------------------------------------------

TEST(PowerCutIoLying, LyingLogFsyncReportsSuccessButSyncsNothing) {
  auto io = make_io("old-log", "old-ckpt");
  io->set_lying_log_fsync(true);
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  ASSERT_TRUE(write_ckpt(*io, 0, "NEW").ok());
  EXPECT_TRUE(io->fsync().ok());  // claims success
  EXPECT_EQ(read_log(*io, 0, 7), "NEW-log") << "the running process still sees its write";
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "old-log");
}

TEST(PowerCutIoLying, LyingLogFsyncLeavesTheCheckpointFsyncHonest) {
  auto io = make_io("old-log", "old-ckpt");
  io->set_lying_log_fsync(true);
  ASSERT_TRUE(write_ckpt(*io, 0, "NEW").ok());
  EXPECT_TRUE(io->ckpt_fsync().ok());
  EXPECT_EQ(reboot_drop_all(*io)->ckpt_bytes(), "NEW-ckpt");
}

TEST(PowerCutIoLying, LyingCkptFsyncReportsSuccessButSyncsNothing) {
  auto io = make_io("old-log", "old-ckpt");
  io->set_lying_ckpt_fsync(true);
  ASSERT_TRUE(write_log(*io, 0, "NEW").ok());
  ASSERT_TRUE(write_ckpt(*io, 0, "NEW").ok());
  EXPECT_TRUE(io->ckpt_fsync().ok());
  EXPECT_TRUE(io->fsync().ok());
  auto after = reboot_drop_all(*io);
  EXPECT_EQ(after->log_bytes(), "NEW-log") << "the log fsync was honest";
  EXPECT_EQ(after->ckpt_bytes(), "old-ckpt");
}

TEST(PowerCutIoLying, LyingDirFsyncReportsSuccessButLosesOnlyTheCheckpointEntry) {
  PowerCutIo io;
  io.set_lying_dir_fsync(true);
  bool opened = false;
  bool created = false;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(io.open_ckpt("p.log.ckpt", true, &opened, &created).ok());
  ASSERT_TRUE(write_log(io, 0, "data").ok());
  ASSERT_TRUE(write_ckpt(io, 0, "slot").ok());
  ASSERT_TRUE(io.fsync().ok());
  ASSERT_TRUE(io.ckpt_fsync().ok());
  EXPECT_FALSE(io.log_name_durable());
  EXPECT_FALSE(io.ckpt_name_durable());
  EXPECT_TRUE(io.fsync_parent_dir().ok());  // claims success
  EXPECT_TRUE(io.log_name_durable());
  EXPECT_FALSE(io.ckpt_name_durable()) << "the lie leaves this one entry unsynced";
  auto after = reboot_drop_all(io);
  EXPECT_TRUE(after->log_exists());
  EXPECT_EQ(after->log_bytes(), "data") << "the log was fsynced and its name is durable";
  EXPECT_FALSE(after->ckpt_exists());
}

TEST(PowerCutIoLying, AnHonestDirFsyncSyncsBothEntries) {
  PowerCutIo io;
  bool opened = false;
  bool created = false;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  ASSERT_TRUE(io.open_ckpt("p.log.ckpt", true, &opened, &created).ok());
  ASSERT_TRUE(io.fsync_parent_dir().ok());
  EXPECT_TRUE(io.log_name_durable());
  EXPECT_TRUE(io.ckpt_name_durable());
}

TEST(PowerCutIoLying, ALyingCallIsStillCountedAndStillFailsAtTheCut) {
  auto io = make_io();
  io->set_lying_log_fsync(true);
  size_t before = io->operation_count();
  EXPECT_TRUE(io->fsync().ok());
  EXPECT_EQ(io->operation_count(), before + 1);
  EXPECT_EQ(io->operations().back(), PowerCutIo::Op::Fsync);
  io->arm_cut(io->operation_count());
  EXPECT_EQ(io->fsync().code(), StatusCode::IoError);
}

TEST(PowerCutIoLying, ModesDoNotCarryOverTheReboot) {
  auto io = make_io();
  io->set_lying_log_fsync(true);
  io->set_lying_ckpt_fsync(true);
  io->set_lying_dir_fsync(true);
  auto after = reboot_drop_all(*io);
  ASSERT_TRUE(test::open_or_create_log(*after, "p.log").ok());
  ASSERT_TRUE(write_log(*after, 0, "NEW").ok());
  ASSERT_TRUE(after->fsync().ok());
  EXPECT_EQ(reboot_drop_all(*after)->log_bytes(), "NEW-log");
}

// --- restart(): the process dies, the power stays on -----------------------------

TEST(PowerCutIoRestart, KeepsUnsyncedDataAndARebootThenLosesIt) {
  auto io = make_io("0123", "");
  ASSERT_TRUE(write_log(*io, 0, "AB").ok());
  auto again = io->restart();
  ASSERT_TRUE(test::open_or_create_log(*again, "p.log").ok());
  EXPECT_EQ(read_log(*again, 0, 4), "AB23");
  EXPECT_EQ(reboot_drop_all(*again)->log_bytes(), "0123");
}

TEST(PowerCutIoRestart, KeepsANonDurableNameNonDurable) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  auto again = io.restart();
  EXPECT_TRUE(again->log_exists());
  EXPECT_FALSE(again->log_name_durable());
  EXPECT_FALSE(reboot_drop_all(*again)->log_exists());
}

TEST(PowerCutIoRestart, ClearsTheCutTheLyingModesAndTheOpenFiles) {
  auto io = make_io();
  io->set_lying_log_fsync(true);
  io->arm_cut(io->operation_count());
  EXPECT_EQ(io->fsync().code(), StatusCode::IoError);
  ASSERT_TRUE(io->is_cut());
  auto again = io->restart();
  EXPECT_FALSE(again->is_cut());
  EXPECT_EQ(again->operation_count(), 0u);
  ASSERT_TRUE(test::open_or_create_log(*again, "p.log").ok());  // lock released: reopens
  ASSERT_TRUE(write_log(*again, 0, "NEW").ok());
  ASSERT_TRUE(again->fsync().ok());  // not lying
  EXPECT_EQ(reboot_drop_all(*again)->log_bytes(), "NEW-log");
}

// --- fail_next -------------------------------------------------------------------

TEST(PowerCutIoFailNext, FailsOneDirectoryFsyncWithNoStateChange) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  io.fail_next(Op::FsyncParentDir, StatusCode::NoSpace);
  EXPECT_EQ(io.fsync_parent_dir().code(), StatusCode::NoSpace);
  EXPECT_FALSE(io.log_name_durable());
  EXPECT_FALSE(reboot_drop_all(io)->log_exists());
  EXPECT_TRUE(io.fsync_parent_dir().ok());
  EXPECT_TRUE(io.log_name_durable());
}

TEST(PowerCutIoFailNext, AnotherOperationNeitherConsumesItNorMovesAnArmedCut) {
  auto io = make_io();
  io->arm_cut(io->operation_count() + 1);
  io->fail_next(Op::FsyncParentDir, StatusCode::NoSpace);
  uint64_t size = 0;
  EXPECT_TRUE(io->file_size(&size).ok());  // operation before the cut: untouched
  EXPECT_FALSE(io->is_cut());
  EXPECT_EQ(io->file_size(&size).code(), StatusCode::IoError);  // the cut, at its number
  EXPECT_TRUE(io->is_cut());
}

TEST(PowerCutIoFailNext, IsNotConsumedByOtherOperations) {
  PowerCutIo io;
  ASSERT_TRUE(test::open_or_create_log(io, "p.log").ok());
  io.fail_next(Op::FsyncParentDir, StatusCode::NoSpace);
  uint64_t size = 0;
  EXPECT_TRUE(io.file_size(&size).ok());
  EXPECT_EQ(io.fsync_parent_dir().code(), StatusCode::NoSpace);
}

}  // namespace
}  // namespace logblock::test
