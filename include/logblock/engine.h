#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "logblock/io.h"
#include "logblock/status.h"

namespace logblock {

// Internal pieces, defined in src/. Declared only, so this public header does not
// include them (log_file.h includes this header for the constants below).
class BlockMap;
class LogFile;
struct RecoveredState;

// Device geometry and record sizes.
inline constexpr uint64_t kBlockSize = 4096;
inline constexpr uint64_t kBlockCount = 262144;
inline constexpr uint64_t kDeviceSize = kBlockSize * kBlockCount;  // 1 GiB

inline constexpr uint64_t kRecordHeaderSize = 40;
inline constexpr uint64_t kDataRecordSize = kRecordHeaderSize + kBlockSize;  // 4,136
inline constexpr uint64_t kTrimRecordSize = kRecordHeaderSize;               // header only

// A checkpoint is written each time the log has grown by this much (D25). The setting may be
// lowered, but never below one DATA record, so a single record can always trigger one.
inline constexpr uint64_t kDefaultCheckpointInterval = uint64_t{64} << 20;  // 64 MiB
inline constexpr uint64_t kMinCheckpointInterval = kDataRecordSize;        // 4,136

static_assert(kDeviceSize == 1073741824, "the device is exactly 1 GiB");
static_assert(kDataRecordSize == 4136, "a DATA record is a 40-byte header plus 4096 bytes");

// How startup rebuilt the block map (FR-023). The engine does not log, so the plugin prints this.
enum class RecoverySource : uint8_t {
  None,        // no records: a new device
  Checkpoint,  // started from a checkpoint slot, then replayed the log after it
  FullScan     // replayed the whole log
};

struct RecoveryReport {
  RecoverySource source = RecoverySource::None;
  char slot = '-';                        // 'A' or 'B' when source is Checkpoint
  uint64_t generation = 0;                // that slot's generation, else 0
  uint64_t records_replayed = 0;          // records applied to the map after the starting point
  uint64_t log_bytes_scanned = 0;         // bytes of log read by the scan
  uint64_t torn_bytes_removed = 0;        // size of the torn tail cut off, 0 if none
  uint64_t torn_offset = 0;               // where that tail started (meaningful if bytes > 0)
  uint64_t unreadable_checkpoints = 0;    // slots present but unusable
  uint64_t time_us = 0;                   // wall time of recovery, from a monotonic clock
  std::vector<std::string> warnings;      // one line each, printed by the plugin as [warning]
};

// Checkpoint activity since startup (FR-024). Durations cover completed checkpoints only.
struct CheckpointCounters {
  uint64_t written = 0;
  uint64_t failed = 0;
  uint64_t unreadable = 0;  // slots ignored at startup
  uint64_t duration_count = 0;
  uint64_t duration_max_us = 0;
  uint64_t duration_sum_us = 0;
};

// The storage engine: a transport-independent interface with byte-range calls.
// Unit tests call it directly, and the nbdkit plugin is a thin adapter
// over it. No call throws; every call returns a Status
//.
class Engine {
 public:
  // Creates an engine on the log at `path`, using `io` for all file work, and recovers whatever
  // the log holds: the block map is rebuilt from it before create() returns, so no request can
  // be served earlier (FR-004). A missing or empty log starts a new device.
  // Fails if `cap` or `checkpoint_interval` is under its minimum (OutOfRange), or the log cannot
  // be opened or recovered. Also fsyncs the log's parent directory.
  // On success, *out holds the engine.
  static Status create(const std::string& path, uint64_t cap, std::unique_ptr<Io> io,
                       std::unique_ptr<Engine>* out,
                       uint64_t checkpoint_interval = kDefaultCheckpointInterval);

  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Fills buf with the latest data in [offset, offset + len). Never-written and
  // trimmed blocks read as zeros.
  Status read(uint64_t offset, uint64_t len, void* buf);

  // Writes [offset, offset + len): appends one whole-block DATA record per
  // touched block, reading old contents first for partly covered blocks.
  Status write(uint64_t offset, uint64_t len, const void* buf);

  // Trims [offset, offset + len): appends one TRIM record for the fully covered
  // blocks, or nothing if there are none.
  Status trim(uint64_t offset, uint64_t len);

  // Returns once everything logged so far is synchronized to disk.
  Status flush();

  // The device size in bytes: kDeviceSize.
  [[nodiscard]] uint64_t size() const;

  // Total bytes in the log's records, including headers: 4,136 per DATA record
  // and 40 per TRIM record. Bytes from a failed append are not counted. The
  // plugin reports this at shutdown.
  [[nodiscard]] uint64_t bytes_logged() const;

  // How this engine's startup went. Fixed once create() returns.
  [[nodiscard]] const RecoveryReport& recovery_report() const;

  // Checkpoint counters since startup. The plugin reports them at shutdown.
  [[nodiscard]] CheckpointCounters checkpoint_counters() const;

  // Warnings raised while serving (a failed checkpoint), one line each, oldest first. Returns them
  // and forgets them, so each is printed once. Startup warnings are in recovery_report().
  [[nodiscard]] std::vector<std::string> take_warnings();

  // Public only so std::make_unique can call it (the project avoids raw `new`).
  // Nobody outside Engine can name PrivateKey, so nobody else can build an
  // Engine, and every Engine goes through create().
  struct PrivateKey {
    explicit PrivateKey() = default;
  };
  Engine(PrivateKey, std::unique_ptr<Io> io, uint64_t cap, uint64_t checkpoint_interval,
         RecoveredState&& recovered);

 private:
  // Failed if the log is in the sticky AppendFailed state (an append failed and the
  // file could not be cut back). Writes and trims call this; reads do not.
  Status check_writable() const;

  // NoSpace unless `needed` more bytes of records fit under the cap. Appends nothing.
  Status check_room(uint64_t needed) const;

  // Fills `out` (kBlockSize bytes) with the block's current contents: zeros if the
  // block is unmapped, otherwise the payload of its latest DATA record.
  Status load_block(uint64_t block, uint8_t* out);

  // Takes a checkpoint if the log has grown by checkpoint_interval_ since the last one. Called
  // after each appended record, so a write that spans many blocks checkpoints at the crossing.
  // `record_offset` is where that record starts.
  void checkpoint_if_due(uint64_t record_offset);

  // Writes one checkpoint covering the record at `last_record_offset`, which must be the last one
  // in the log: fsync the log, write the older slot whole, fsync the checkpoint file, and, if this
  // created the file, fsync its directory (D26). A failure never fails the caller's request: it
  // is counted, warned about, and turns checkpoints off until restart (D27).
  void write_checkpoint(uint64_t last_record_offset);

  // Members are destroyed in reverse order, so log_ (which holds a reference to
  // *io_) goes before io_. Keep io_ first.
  std::unique_ptr<Io> io_;
  uint64_t cap_;  // limit on the log's size in bytes; the write path checks it
  std::unique_ptr<BlockMap> map_;
  std::unique_ptr<LogFile> log_;
  // FlushFailed: set when an fsync fails, and never cleared. AppendFailed lives in
  // log_->broken().
  bool flush_failed_ = false;
  uint64_t checkpoint_interval_;
  RecoveryReport report_;
  CheckpointCounters counters_;

  // Checkpoint state. The buffer holds one encoded slot (a little over 2 MiB) and is allocated
  // once, so a checkpoint never allocates in the middle of a write.
  std::string ckpt_path_;
  std::vector<uint8_t> ckpt_buffer_;
  bool ckpt_open_ = false;           // the Io has the checkpoint file open
  bool checkpoints_disabled_ = false;  // a checkpoint failed; none are tried until restart (D27)
  uint64_t last_checkpoint_end_ = 0;  // where the newest checkpoint's covered record ends
  uint64_t generation_ = 0;           // the newest checkpoint's generation; the next is + 1
  int next_slot_ = 0;                 // the slot the next checkpoint overwrites (D24)
  bool fresh_checkpoint_due_ = false;
  uint64_t startup_record_offset_ = 0;
  std::vector<std::string> warnings_;
};

}  // namespace logblock
