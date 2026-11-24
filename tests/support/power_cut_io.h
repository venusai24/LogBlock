#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "logblock/io.h"
#include "support/fault_io.h"

namespace logblock::test {

// An in-memory Io that models what a power cut can lose (contracts/power-cut-io.md, D21, D30).
// Test-only: never linked into the plugin.
//
// For each of the log and the checkpoint file it keeps:
//   - the durable image: what survives any cut;
//   - a pending list of writes and truncates since the last fsync, in order;
//   - whether the file's name is durable. A new file's name becomes durable only after
//     fsync_parent_dir().
// Reads see the durable image with the pending operations applied, as a running process would.
// fsync moves that file's pending list into its durable image.
//
// A cut armed at operation N makes operation N and every later one fail with IoError, without
// taking effect. reboot() then returns a new object holding the files that survived. What a file
// keeps of its pending list is an Outcome, chosen per file by the seed (or by the test).
//
// All randomness comes from one std::mt19937_64 seeded in the constructor. Every pick is
// `rng() % n` instead of a std distribution, because distributions give different results in
// different standard libraries and a seed must replay anywhere.
class PowerCutIo final : public Io {
 public:
  // The same names as FaultIo, so tests and the run helper describe operations one way.
  using Op = FaultIo::Op;

  // What a file keeps of its pending list when the power goes.
  enum class Kind {
    DropAll,       // the durable image only
    RandomPrefix,  // the first K pending operations are applied
    TearLast       // the first K are applied, then a shorter prefix of the next write
  };

  // One file's outcome. Left empty, prefix_ops and tear_bytes are drawn from the seed. A test can
  // set them to get exact bytes. Too-large values are clamped: RandomPrefix keeps at most all
  // pending operations; TearLast keeps at most all but one (a write must remain to be torn), and
  // tears at most one byte fewer than the write holds, so a tear never contributes a whole write.
  // After reboot(), boot_outcomes() has them all filled in with what was actually used.
  struct Outcome {
    Kind kind = Kind::DropAll;
    // DropAll and RandomPrefix only: the file keeps the length it would have had with every
    // pending operation applied, and bytes that were never written read as zero.
    bool zero_fill = false;
    std::optional<size_t> prefix_ops;  // K
    std::optional<size_t> tear_bytes;  // bytes of the torn write that reach the file
  };
  struct Outcomes {
    Outcome log;
    Outcome ckpt;
  };

  explicit PowerCutIo(uint64_t seed = 0) : rng_(seed) {}

  // --- Io ------------------------------------------------------------------
  Status open_log_existing(const std::string& path, bool* found) override;
  Status create_log(const std::string& path, bool* created) override;
  Status file_size(uint64_t* size) override;
  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override;
  Status pwrite(uint64_t offset, const void* buf, size_t len) override;
  Status fsync() override;
  Status ftruncate(uint64_t size) override;
  Status fsync_parent_dir() override;
  Status lock_log() override;
  Status open_ckpt(const std::string& path, bool create, bool* opened, bool* created) override;
  Status ckpt_size(uint64_t* size) override;
  Status ckpt_pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override;
  Status ckpt_pwrite(uint64_t offset, const void* buf, size_t len) override;
  Status ckpt_fsync() override;

  // --- Setting up a starting state -----------------------------------------
  // Makes the file exist with these bytes, fully durable (name and data). For tests that start
  // from a device that already has a log or a checkpoint file. Call before the file is opened.
  void set_log_image(std::string bytes);
  void set_ckpt_image(std::string bytes);

  // --- The cut -------------------------------------------------------------
  // Operations are numbered from 0 in the order they are called, and every Io call counts, reads
  // included. arm_cut(n) makes operation number n, and all later ones, fail. To cut at the next
  // call, pass operation_count().
  void arm_cut(size_t operation_number) { cut_at_ = operation_number; }
  [[nodiscard]] bool is_cut() const { return cut_; }
  [[nodiscard]] size_t operation_count() const { return ops_.size(); }
  // Every call so far, in order, including the one that hit the cut.
  [[nodiscard]] const std::vector<Op>& operations() const { return ops_; }

  // --- Restart -------------------------------------------------------------
  // Returns a new object over the files that survive, with empty pending lists, nothing open,
  // no cut armed and no lying mode. A file whose name is not durable does not survive at all.
  // The rest of the file is its Outcome. This object's files are left as they were, but the seed's
  // generator advances. A cut can be armed again on the result.
  //
  // reboot() lets the seed choose each file's outcome independently. The other form takes them
  // from the caller (and still draws any prefix or tear length the caller left empty).
  // The result's own seed comes from this object's generator, so chained reboots are repeatable.
  [[nodiscard]] std::unique_ptr<PowerCutIo> reboot();
  [[nodiscard]] std::unique_ptr<PowerCutIo> reboot(const Outcome& log, const Outcome& ckpt);

  // The process dies and the power stays on. Returns a new object whose files are this object's
  // running-process view: the live bytes, with each file's unsynced writes still unsynced (a later
  // reboot() can still drop them) and its name durable or not as it was. Nothing is open, no cut
  // is armed, no lying mode is set, and the operation count starts again. This object is left as
  // it was. The result's seed comes from this object's generator.
  [[nodiscard]] std::unique_ptr<PowerCutIo> restart();

  // --- Injected failure ----------------------------------------------------
  // Makes the next call to `op` return `code` and do nothing else. The call is still counted. Calls
  // to other operations leave it waiting, and it does not move an armed cut: a call at or after
  // the cut fails as a power cut, and the injected failure stays waiting.
  void fail_next(Op op, StatusCode code) { injected_ = Injected{op, code}; }

  // How this object's files came to be: the outcomes used by the reboot() that made it, with
  // every length filled in. Default (drop-all) for an object that was not made by reboot().
  [[nodiscard]] const Outcomes& boot_outcomes() const { return boot_outcomes_; }

  // --- Lying modes (FR-035) -------------------------------------------------
  // Each makes one call report success without doing its work, so the ordering test can check
  // that it notices a broken order. They are off in every new object, including after reboot().
  void set_lying_log_fsync(bool v) { lying_log_fsync_ = v; }
  void set_lying_ckpt_fsync(bool v) { lying_ckpt_fsync_ = v; }
  // The directory fsync returns Ok but leaves the checkpoint file's entry unsynced. The log's
  // entry is synced as usual, so a cut loses the checkpoint file and nothing else.
  void set_lying_dir_fsync(bool v) { lying_dir_fsync_ = v; }

  // --- Looking at a file ---------------------------------------------------
  // The running-process view (durable image with pending applied). On a freshly rebooted object
  // that is exactly the surviving file.
  [[nodiscard]] bool log_exists() const { return log_.exists; }
  [[nodiscard]] bool ckpt_exists() const { return ckpt_.exists; }
  // Whether the file's directory entry would survive a cut now (it exists and was synced).
  [[nodiscard]] bool log_name_durable() const { return log_.exists && log_.name_durable; }
  [[nodiscard]] bool ckpt_name_durable() const { return ckpt_.exists && ckpt_.name_durable; }
  [[nodiscard]] const std::string& log_bytes() const { return log_.live; }
  [[nodiscard]] const std::string& ckpt_bytes() const { return ckpt_.live; }

 private:
  // One write or truncate that has not been fsynced.
  struct PendingOp {
    bool is_truncate = false;
    uint64_t offset = 0;  // writes
    std::string data;     // writes
    uint64_t size = 0;    // truncates
  };

  struct File {
    bool exists = false;
    bool name_durable = false;
    bool open = false;
    std::string durable;  // survives any cut
    std::string live;     // durable with every pending operation applied
    std::vector<PendingOp> pending;
  };

  // Records the call and applies the cut. Returns a non-ok Status if this call is at or after
  // the cut, in which case the caller must do nothing else.
  Status begin(Op op);
  static void apply(const PendingOp& op, std::string* image);
  static Status write_file(File& f, uint64_t offset, const void* buf, size_t len);
  static Status truncate_file(File& f, uint64_t size);
  static Status read_file(const File& f, uint64_t offset, void* buf, size_t len, size_t* got);
  static void sync_file(File& f);
  static Status check_open(const File& f, const char* what);

  Outcome pick_outcome();
  // The bytes a file keeps. Fills in o's prefix_ops and tear_bytes with what was used.
  std::string survivor(const File& f, Outcome* o);

  struct Injected {
    Op op;
    StatusCode code;
  };

  File log_;
  File ckpt_;
  std::optional<Injected> injected_;
  std::vector<Op> ops_;
  std::optional<size_t> cut_at_;
  bool cut_ = false;
  bool lying_log_fsync_ = false;
  bool lying_ckpt_fsync_ = false;
  bool lying_dir_fsync_ = false;
  std::mt19937_64 rng_;
  Outcomes boot_outcomes_;
};

}  // namespace logblock::test
