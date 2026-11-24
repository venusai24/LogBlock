#include "support/power_cut_run.h"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <utility>

#include "checkpoint.h"
#include "log_scan.h"
#include "logblock/engine.h"
#include "support/forwarding_io.h"
#include "support/power_cut_io.h"

namespace logblock::test {
namespace {

using Op = PowerCutIo::Op;

constexpr uint64_t kCap = uint64_t{64} << 20;  // far above what a short workload writes
constexpr const char* kLogPath = "power_cut.log";

// splitmix64: turns (seed, salt) into an independent-looking generator seed, so the workload,
// the cut and the PowerCutIo each get their own stream from one test seed.
uint64_t mix(uint64_t seed, uint64_t salt) {
  uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (salt + 1);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// A DATA payload that says which request wrote it: the id in the first 8 bytes, then a pattern.
// A block is either all zeros or exactly payload(id) for some request, so a torn or mixed block
// is noticed.
std::vector<uint8_t> payload(uint64_t id) {
  std::vector<uint8_t> out(kBlockSize);
  for (size_t j = 0; j < out.size(); ++j) {
    out[j] = static_cast<uint8_t>(id * 131 + j * 7);
  }
  for (int b = 0; b < 8; ++b) {
    out[static_cast<size_t>(b)] = static_cast<uint8_t>(id >> (8 * b));
  }
  return out;
}

// --- Running a workload -----------------------------------------------------------------------

struct Step {
  Request::Kind kind;
  size_t ops_begin;  // the Io operations this request made: [ops_begin, ops_end)
  size_t ops_end;
  bool ok;
};

struct Execution {
  Status create_status = Status::Ok();
  std::unique_ptr<Engine> engine;  // must be destroyed before the PowerCutIo it points at
  size_t create_ops_end = 0;
  std::vector<Step> steps;  // the requests started; stops at the first failure or once cut
};

Status recover_on(PowerCutIo& io, std::unique_ptr<Engine>* out) {
  return Engine::create(kLogPath, kCap, std::make_unique<ForwardingIo>(io), out,
                        kPowerCutCheckpointInterval);
}

// Starts an engine on `io` and issues the workload until a request fails or the power is cut.
Execution execute(PowerCutIo& io, const std::vector<Request>& workload) {
  Execution ex;
  ex.create_status = recover_on(io, &ex.engine);
  ex.create_ops_end = io.operation_count();
  if (!ex.create_status.ok()) {
    return ex;
  }
  for (size_t i = 0; i < workload.size(); ++i) {
    const Request& r = workload[i];
    const size_t begin = io.operation_count();
    Status s = Status::Ok();
    switch (r.kind) {
      case Request::Kind::Write: {
        const std::vector<uint8_t> data = payload(i + 1);
        s = ex.engine->write(r.block * kBlockSize, kBlockSize, data.data());
        break;
      }
      case Request::Kind::Trim:
        s = ex.engine->trim(r.block * kBlockSize, kBlockSize);
        break;
      case Request::Kind::Flush:
        s = ex.engine->flush();
        break;
    }
    ex.steps.push_back({r.kind, begin, io.operation_count(), s.ok()});
    // A write whose checkpoint was cut still returns Ok (D27), so look at the Io too.
    if (!s.ok() || io.is_cut()) {
      break;
    }
  }
  return ex;
}

// Operations that change something a cut could lose. A cut on a read is the same as a cut on the
// next change, so these are the interesting places.
bool is_mutating(Op op) {
  switch (op) {
    case Op::Pwrite:
    case Op::Fsync:
    case Op::Ftruncate:
    case Op::FsyncParentDir:
    case Op::OpenCkpt:
    case Op::CkptPwrite:
    case Op::CkptFsync:
      return true;
    default:
      return false;
  }
}

// The operation numbers of the dry pass that belong to a crash-point class. Which request made an
// operation tells the two log fsyncs apart: one inside a flush is the client's, one inside a
// write or trim is a checkpoint's.
std::vector<size_t> ops_in_class(const PowerCutIo& io, const Execution& ex, CrashPoint cp) {
  std::vector<size_t> found;
  for (const Step& step : ex.steps) {
    for (size_t i = step.ops_begin; i < step.ops_end; ++i) {
      const Op op = io.operations()[i];
      const bool in_flush = step.kind == Request::Kind::Flush;
      bool match = false;
      switch (cp) {
        case CrashPoint::LogWrite:
          match = op == Op::Pwrite;
          break;
        case CrashPoint::ClientFlushFsync:
          match = op == Op::Fsync && in_flush;
          break;
        case CrashPoint::CheckpointLogFsync:
          match = op == Op::Fsync && !in_flush;
          break;
        case CrashPoint::CheckpointWrite:
          match = op == Op::CkptPwrite;
          break;
        case CrashPoint::CheckpointFsync:
          match = op == Op::CkptFsync;
          break;
        case CrashPoint::DirectoryFsync:
          match = op == Op::FsyncParentDir;
          break;
        case CrashPoint::DuringRecovery:
          match = is_mutating(op);  // the main cut may land anywhere
          break;
      }
      if (match) {
        found.push_back(i);
      }
    }
  }
  return found;
}

PowerCutIo::Outcome to_outcome(FileOutcome o) {
  PowerCutIo::Outcome out;
  switch (o) {
    case FileOutcome::DropAll:
      out.kind = PowerCutIo::Kind::DropAll;
      break;
    case FileOutcome::DropAllZeroFill:
      out.kind = PowerCutIo::Kind::DropAll;
      out.zero_fill = true;
      break;
    case FileOutcome::RandomPrefix:
      out.kind = PowerCutIo::Kind::RandomPrefix;
      break;
    case FileOutcome::RandomPrefixZeroFill:
      out.kind = PowerCutIo::Kind::RandomPrefix;
      out.zero_fill = true;
      break;
    case FileOutcome::TearLast:
      out.kind = PowerCutIo::Kind::TearLast;
      break;
  }
  return out;
}

// Reads every touched block through the engine. Returns "" or what went wrong.
std::string read_state(Engine& engine, DeviceState* state) {
  std::vector<uint8_t> buf(kBlockSize);
  for (uint64_t block : touched_blocks()) {
    Status s = engine.read(block * kBlockSize, kBlockSize, buf.data());
    if (!s.ok()) {
      return "read of block " + std::to_string(block) + " failed: " + s.message();
    }
    if (std::ranges::all_of(buf, [](uint8_t b) { return b == 0; })) {
      continue;  // reads as zeros
    }
    uint64_t id = 0;
    for (int b = 0; b < 8; ++b) {
      id |= static_cast<uint64_t>(buf[static_cast<size_t>(b)]) << (8 * b);
    }
    if (buf != payload(id)) {
      return "block " + std::to_string(block) + " holds bytes that no request wrote";
    }
    (*state)[block] = id;
  }
  return "";
}

}  // namespace

const char* to_string(CrashPoint v) {
  switch (v) {
    case CrashPoint::LogWrite:
      return "log-write";
    case CrashPoint::ClientFlushFsync:
      return "client-flush-fsync";
    case CrashPoint::CheckpointLogFsync:
      return "checkpoint-log-fsync";
    case CrashPoint::CheckpointWrite:
      return "checkpoint-write";
    case CrashPoint::CheckpointFsync:
      return "checkpoint-fsync";
    case CrashPoint::DirectoryFsync:
      return "directory-fsync";
    case CrashPoint::DuringRecovery:
      return "during-recovery";
  }
  return "unknown";
}

const char* to_string(FileOutcome v) {
  switch (v) {
    case FileOutcome::DropAll:
      return "drop-all";
    case FileOutcome::DropAllZeroFill:
      return "drop-all-zero-fill";
    case FileOutcome::RandomPrefix:
      return "random-prefix";
    case FileOutcome::RandomPrefixZeroFill:
      return "random-prefix-zero-fill";
    case FileOutcome::TearLast:
      return "tear-last";
  }
  return "unknown";
}

const char* to_string(RecoveryPath v) {
  switch (v) {
    case RecoveryPath::None:
      return "none";
    case RecoveryPath::Checkpoint:
      return "checkpoint";
    case RecoveryPath::FullScan:
      return "full-scan";
  }
  return "unknown";
}

const char* to_string(OrderingCheck v) {
  switch (v) {
    case OrderingCheck::Passed:
      return "passed";
    case OrderingCheck::SlotClaimsLostRecord:
      return "slot-claims-lost-record";
    case OrderingCheck::NewestCheckpointLost:
      return "newest-checkpoint-lost";
  }
  return "unknown";
}

// --- The workload -----------------------------------------------------------------------------

const std::vector<uint64_t>& touched_blocks() {
  // Spread across the device so the checkpoint map has entries far apart. Few enough that
  // overwrites and trims of a mapped block are common.
  static const std::vector<uint64_t> blocks = {0, 1, 7, 4096, 100000, kBlockCount - 1};
  return blocks;
}

std::vector<Request> make_workload(uint64_t seed, bool with_flushes) {
  std::mt19937_64 rng(mix(seed, 1));
  const auto& blocks = touched_blocks();
  const size_t count = 11 + rng() % 4;
  std::vector<Request> out;
  for (size_t i = 0; i < count; ++i) {
    // Both draws happen for every request, so the stream does not depend on what is chosen.
    const uint64_t block = blocks[rng() % blocks.size()];
    const uint64_t roll = rng() % 100;
    Request r;
    r.block = block;
    if (i == 4 && with_flushes) {
      r.kind = Request::Kind::Flush;
    } else if (i < 4 || roll < 65) {
      r.kind = Request::Kind::Write;  // the first four are writes: three reach the first checkpoint
    } else if (roll < 85) {
      r.kind = Request::Kind::Trim;
    } else {
      r.kind = with_flushes ? Request::Kind::Flush : Request::Kind::Write;
    }
    out.push_back(r);
  }
  return out;
}

bool matches_some_prefix(const std::vector<Request>& workload, const DeviceState& state,
                         size_t min_prefix, size_t max_prefix, size_t* matched) {
  DeviceState expected;
  max_prefix = std::min(max_prefix, workload.size());
  for (size_t p = 0; p <= max_prefix; ++p) {
    if (p >= min_prefix && expected == state) {
      *matched = p;
      return true;
    }
    if (p == max_prefix) {
      break;
    }
    const Request& r = workload[p];
    if (r.kind == Request::Kind::Write) {
      expected[r.block] = p + 1;
    } else if (r.kind == Request::Kind::Trim) {
      expected.erase(r.block);
    }
  }
  return false;
}

// --- One iteration ----------------------------------------------------------------------------

PowerCutResult power_cut_run(uint64_t seed) {
  PowerCutResult r;
  r.seed = seed;
  // The seed cycles through classes and outcomes. Class has period 7 and the log outcome period 5,
  // so 35 consecutive seeds meet every pair; the checkpoint outcome moves more slowly.
  r.crash_point = static_cast<CrashPoint>(seed % kCrashPointCount);
  r.log_outcome = static_cast<FileOutcome>(seed % kFileOutcomeCount);
  r.ckpt_outcome = static_cast<FileOutcome>((seed / 3) % kFileOutcomeCount);

  auto fail = [&](const std::string& why) {
    r.failure = "seed " + std::to_string(seed) + " (" + to_string(r.crash_point) +
                ", log " + to_string(r.log_outcome) + ", ckpt " + to_string(r.ckpt_outcome) +
                "): " + why;
    return r;
  };

  const std::vector<Request> workload = make_workload(seed, /*with_flushes=*/true);

  // 1. A dry pass with no cut, to list the operations.
  PowerCutIo dry_io(mix(seed, 2));
  size_t cut_op = 0;
  std::vector<Op> dry_ops;
  {
    Execution dry = execute(dry_io, workload);
    if (!dry.create_status.ok() || dry.steps.size() != workload.size() || !dry.steps.back().ok) {
      return fail("the dry pass did not finish: " + dry.create_status.message());
    }
    // 2. The seed picks one operation of the class.
    const std::vector<size_t> candidates = ops_in_class(dry_io, dry, r.crash_point);
    if (candidates.empty()) {
      return fail("the workload has no operation of this crash-point class");
    }
    std::mt19937_64 pick(mix(seed, 3));
    cut_op = candidates[pick() % candidates.size()];
    dry_ops = dry_io.operations();
  }

  // 3. The same workload with the cut armed.
  PowerCutIo io(mix(seed, 2));
  io.arm_cut(cut_op);
  size_t flushed_prefix = 0;
  {
    Execution run = execute(io, workload);
    if (!run.create_status.ok()) {
      return fail("create failed before the cut: " + run.create_status.message());
    }
    r.cut_reached = io.is_cut();
    if (!r.cut_reached) {
      return fail("the cut at operation " + std::to_string(cut_op) + " never fired");
    }
    // Up to the cut the run must be the dry pass, or the operation numbers meant nothing.
    if (io.operations().size() <= cut_op ||
        !std::equal(dry_ops.begin(), dry_ops.begin() + static_cast<std::ptrdiff_t>(cut_op + 1),
                    io.operations().begin())) {
      return fail("the run diverged from the dry pass before the cut");
    }
    r.requests_issued = run.steps.size();
    for (size_t i = 0; i < run.steps.size(); ++i) {
      if (run.steps[i].kind == Request::Kind::Flush && run.steps[i].ok) {
        flushed_prefix = i + 1;
      }
    }
    r.flushed_prefix = flushed_prefix;
  }  // the engine is gone before its Io is used for anything else

  // 4. The power returns: each file keeps what the seed says.
  std::unique_ptr<PowerCutIo> restart =
      io.reboot(to_outcome(r.log_outcome), to_outcome(r.ckpt_outcome));
  r.pre_recovery_log = restart->log_bytes();
  r.pre_recovery_ckpt = restart->ckpt_bytes();

  // 5. For the recovery class, cut the first restart too, then restart again.
  if (r.crash_point == CrashPoint::DuringRecovery) {
    // A second copy of the same survivors lists the restart's operations. Passing the filled-in
    // outcomes reproduces the same bytes (the seed's generator has moved on).
    const PowerCutIo::Outcomes used = restart->boot_outcomes();
    std::unique_ptr<PowerCutIo> dry_restart = io.reboot(used.log, used.ckpt);
    size_t recovery_cut = 0;
    {
      std::unique_ptr<Engine> engine;
      Status s = recover_on(*dry_restart, &engine);
      if (!s.ok()) {
        return fail("recovery refused before the second cut: " + s.message());
      }
      std::vector<size_t> candidates;
      for (size_t i = 0; i < dry_restart->operations().size(); ++i) {
        if (is_mutating(dry_restart->operations()[i])) {
          candidates.push_back(i);
        }
      }
      if (candidates.empty()) {
        return fail("recovery made no operation a cut could hit");
      }
      std::mt19937_64 pick(mix(seed, 4));
      recovery_cut = candidates[pick() % candidates.size()];
    }
    restart->arm_cut(recovery_cut);
    {
      std::unique_ptr<Engine> engine;
      (void)recover_on(*restart, &engine);  // may fail, or succeed if only a checkpoint was cut
    }
    if (!restart->is_cut()) {
      return fail("the cut at recovery operation " + std::to_string(recovery_cut) +
                  " never fired");
    }
    restart = restart->reboot();
    r.pre_recovery_log = restart->log_bytes();
    r.pre_recovery_ckpt = restart->ckpt_bytes();
  }

  // 6. The final restart, and the check.
  std::unique_ptr<Engine> engine;
  if (Status s = recover_on(*restart, &engine); !s.ok()) {
    return fail("recovery refused: " + s.message());
  }
  const RecoveryReport& report = engine->recovery_report();
  switch (report.source) {
    case RecoverySource::None:
      r.recovery_path = RecoveryPath::None;
      break;
    case RecoverySource::Checkpoint:
      r.recovery_path = RecoveryPath::Checkpoint;
      break;
    case RecoverySource::FullScan:
      r.recovery_path = RecoveryPath::FullScan;
      break;
  }
  r.torn_tail = report.torn_bytes_removed > 0;

  DeviceState state;
  if (std::string why = read_state(*engine, &state); !why.empty()) {
    return fail(why);
  }
  size_t matched = 0;
  if (!matches_some_prefix(workload, state, flushed_prefix, r.requests_issued, &matched)) {
    return fail("the device matches no unbroken prefix of the " +
                std::to_string(r.requests_issued) + " requests issued, at or after the " +
                std::to_string(flushed_prefix) + " a completed flush covered");
  }
  r.matched_prefix = matched;
  // A pass keeps nothing: the files are only worth carrying when the run failed.
  r.pre_recovery_log.clear();
  r.pre_recovery_ckpt.clear();
  return r;
}

// --- The ordering test ------------------------------------------------------------------------

OrderingResult power_cut_ordering_run(uint64_t seed, const Lies& lies) {
  OrderingResult r;
  r.seed = seed;
  auto harness_fail = [&](const std::string& why) {
    r.failure = "seed " + std::to_string(seed) + ": " + why;
    return r;
  };
  auto set_lies = [&](PowerCutIo& io) {
    io.set_lying_log_fsync(lies.log_fsync);
    io.set_lying_ckpt_fsync(lies.ckpt_fsync);
    io.set_lying_dir_fsync(lies.dir_fsync);
  };

  // No flush, so every log fsync comes from a checkpoint and each lie breaks one step.
  const std::vector<Request> workload = make_workload(seed, /*with_flushes=*/false);

  // A dry pass lists the operations. A lie does not change the call sequence, only what the
  // calls do, so the dry pass runs honestly, and its answer (a function of the seed alone) is kept:
  // the same seed under another lie, or run again, makes the same calls. The pass is the slow part
  // (every checkpoint writes a whole 2 MiB slot), so this keeps the ordering test short.
  static std::map<uint64_t, size_t> cut_ops;
  auto known = cut_ops.find(seed);
  if (known == cut_ops.end()) {
    PowerCutIo dry_io(mix(seed, 12));
    Execution dry = execute(dry_io, workload);
    if (!dry.create_status.ok() || dry.steps.size() != workload.size()) {
      return harness_fail("the dry pass did not finish");
    }
    // Any change the workload makes is a place to cut, and so is "after everything".
    std::vector<size_t> candidates;
    for (size_t i = dry.create_ops_end; i < dry_io.operations().size(); ++i) {
      if (is_mutating(dry_io.operations()[i])) {
        candidates.push_back(i);
      }
    }
    candidates.push_back(dry_io.operations().size());  // never fires: the power goes at the end
    std::mt19937_64 pick(mix(seed, 13));
    known = cut_ops.emplace(seed, candidates[pick() % candidates.size()]).first;
  }
  const size_t cut_op = known->second;

  PowerCutIo io(mix(seed, 12));
  set_lies(io);
  io.arm_cut(cut_op);
  std::unique_ptr<PowerCutIo> survivors;
  {
    Execution run = execute(io, workload);
    if (!run.create_status.ok()) {
      return harness_fail("create failed: " + run.create_status.message());
    }
    r.cut_reached = io.is_cut();
    r.completed_checkpoints = run.engine->checkpoint_counters().written;
    // Outcomes cycle with the seed, as in power_cut_run.
    survivors = io.reboot(to_outcome(static_cast<FileOutcome>(seed % kFileOutcomeCount)),
                          to_outcome(static_cast<FileOutcome>((seed / 3) % kFileOutcomeCount)));
  }

  // Look at the surviving files directly, before any recovery.
  const std::string& log = survivors->log_bytes();
  const std::string& ckpt = survivors->ckpt_bytes();
  const ScanResult scan = scan_log(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(log.data()), log.size()));
  r.log_survived = survivors->log_exists();
  r.ckpt_survived = survivors->ckpt_exists();
  r.log_records = scan.records.size();

  bool newest_readable = false;
  for (int index = 0; index < 2; ++index) {
    const uint64_t offset = checkpoint_slot_offset(index);
    std::span<const uint8_t> bytes;
    if (survivors->ckpt_exists() && offset < ckpt.size()) {
      bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(ckpt.data()) + offset,
                                       ckpt.size() - offset);
    }
    const DecodedSlot slot = decode_slot(bytes);
    if (slot.state != SlotState::Valid) {
      continue;
    }
    // Check 1: a valid slot may only name a record the surviving log holds.
    const bool holds = std::ranges::any_of(scan.records, [&](const ScannedRecord& rec) {
      return rec.offset == slot.header.last_record_offset &&
             rec.header.seq == slot.header.covered_seq;
    });
    if (!holds) {
      r.check = OrderingCheck::SlotClaimsLostRecord;
      r.detail = std::string("slot ") + (index == 0 ? "A" : "B") + " (generation " +
                 std::to_string(slot.header.generation) + ") claims the record with seq " +
                 std::to_string(slot.header.covered_seq) + " at offset " +
                 std::to_string(slot.header.last_record_offset) + ", but the surviving log holds " +
                 std::to_string(scan.records.size()) + " records and ends at " +
                 std::to_string(scan.valid_end);
      return r;
    }
    if (slot.header.generation >= r.completed_checkpoints) {
      newest_readable = true;
    }
  }

  // Check 2: a checkpoint whose every step returned must have survived (or a newer one).
  if (r.completed_checkpoints > 0 && !newest_readable) {
    r.check = OrderingCheck::NewestCheckpointLost;
    r.detail = "checkpoint " + std::to_string(r.completed_checkpoints) +
               " completed, but no valid slot with generation " +
               std::to_string(r.completed_checkpoints) + " or later survived (checkpoint file " +
               (survivors->ckpt_exists() ? std::to_string(ckpt.size()) + " bytes" : "gone") + ")";
  }
  return r;
}

}  // namespace logblock::test
