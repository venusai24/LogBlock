#pragma once

// One seeded power-cut iteration (contracts/power-cut-io.md, "One iteration"), and the ordering
// check of FR-035 ("The ordering test"). Test-only.
//
// power_cut_run(seed) runs a seeded workload of single-block requests on an Engine over a
// PowerCutIo, cuts the power at a seed-chosen operation, recovers, and checks that the device
// holds the state after some unbroken prefix of the requests, including everything a completed
// flush covered. Everything comes from the seed, so the same seed always gives the same result
// (FR-034). tests/crash/crash_test.cc will call this and count hits per path.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "logblock/engine.h"

namespace logblock::test {

// Where the power went in the workload, or during recovery. The seed cycles through these.
enum class CrashPoint : uint8_t {
  LogWrite,            // a pwrite of a record to the log
  ClientFlushFsync,    // the log fsync inside a client flush
  CheckpointLogFsync,  // the log fsync a checkpoint does first (D26)
  CheckpointWrite,     // the pwrite of a checkpoint slot
  CheckpointFsync,     // the checkpoint file's fsync
  DirectoryFsync,      // the directory fsync after the checkpoint file was created
  DuringRecovery       // a cut inside the first restart; a second restart follows
};
inline constexpr size_t kCrashPointCount = 7;

// What one file kept of its unsynced operations: PowerCutIo::Kind plus its zero-fill variant.
enum class FileOutcome : uint8_t {
  DropAll,
  DropAllZeroFill,
  RandomPrefix,
  RandomPrefixZeroFill,
  TearLast
};
inline constexpr size_t kFileOutcomeCount = 5;

// How the final restart rebuilt the map. The torn tail is reported separately because it can
// happen with either source.
enum class RecoveryPath : uint8_t { None, Checkpoint, FullScan };

const char* to_string(CrashPoint v);
const char* to_string(FileOutcome v);
const char* to_string(RecoveryPath v);

struct PowerCutResult {
  uint64_t seed = 0;
  CrashPoint crash_point = CrashPoint::LogWrite;
  FileOutcome log_outcome = FileOutcome::DropAll;
  FileOutcome ckpt_outcome = FileOutcome::DropAll;
  RecoveryPath recovery_path = RecoveryPath::None;  // of the final restart
  bool torn_tail = false;                           // the final restart removed a torn tail
  bool cut_reached = false;                         // the armed cut actually fired
  size_t requests_issued = 0;                       // requests started before the power went
  size_t flushed_prefix = 0;                        // requests a completed flush covered
  size_t matched_prefix = 0;                        // the prefix the device turned out to hold
  // Empty when the iteration passed. Otherwise one line starting with the seed.
  std::string failure;
  // The two files as the power left them, just before the final recovery. Kept only when the run
  // failed, so a failing seed can be replayed from the files. Empty after a pass.
  std::string pre_recovery_log;
  std::string pre_recovery_ckpt;

  [[nodiscard]] bool ok() const { return failure.empty(); }
  bool operator==(const PowerCutResult&) const = default;
};

PowerCutResult power_cut_run(uint64_t seed);

// --- The workload and the prefix check, public so tests can check the check -----------------

struct Request {
  enum class Kind : uint8_t { Write, Trim, Flush };
  Kind kind = Kind::Write;
  uint64_t block = 0;  // unused by Flush
  bool operator==(const Request&) const = default;
};

// Checkpoint interval the runs use: three DATA records, so a short workload checkpoints often.
inline constexpr uint64_t kPowerCutCheckpointInterval = 3 * kDataRecordSize;

// A seeded list of single-block requests. The first four are writes (so a checkpoint is due) and
// the fifth is a flush when `with_flushes` is true; with it false there is no flush at all.
std::vector<Request> make_workload(uint64_t seed, bool with_flushes);

// The blocks a workload may touch, which a run reads back at the end.
const std::vector<uint64_t>& touched_blocks();

// What the device holds: block -> id of the request that wrote it (request index + 1). A block
// that is absent reads as zeros.
using DeviceState = std::map<uint64_t, uint64_t>;

// True if `state` equals the state after the first P requests for some P in
// [min_prefix, max_prefix]. *matched gets that P.
bool matches_some_prefix(const std::vector<Request>& workload, const DeviceState& state,
                         size_t min_prefix, size_t max_prefix, size_t* matched);

// --- The ordering test (FR-035) -------------------------------------------------------------

enum class OrderingCheck : uint8_t {
  Passed,
  SlotClaimsLostRecord,  // a valid slot names a record the surviving log does not hold
  NewestCheckpointLost   // the newest checkpoint whose steps all returned is not readable
};
const char* to_string(OrderingCheck v);

struct Lies {
  bool log_fsync = false;
  bool ckpt_fsync = false;
  bool dir_fsync = false;
};

struct OrderingResult {
  uint64_t seed = 0;
  OrderingCheck check = OrderingCheck::Passed;
  std::string detail;               // why the check failed, empty if it passed
  bool cut_reached = false;
  uint64_t completed_checkpoints = 0;  // checkpoints whose every step returned before the cut
  // What the cut left, before any recovery: whether each file's name survived, and how many whole
  // records the surviving log holds.
  bool log_survived = false;
  bool ckpt_survived = false;
  size_t log_records = 0;
  std::string failure;              // the harness itself broke (not a check failing)
  bool operator==(const OrderingResult&) const = default;
};

// Runs a flush-free workload with a seed-chosen cut, then checks the surviving files before any
// recovery. With all `lies` false this is the correct order and must pass.
OrderingResult power_cut_ordering_run(uint64_t seed, const Lies& lies = {});

}  // namespace logblock::test
