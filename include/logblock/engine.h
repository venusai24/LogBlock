#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "logblock/io.h"
#include "logblock/status.h"

namespace logblock {

// Internal pieces, defined in src/. Declared only, so this public header does not
// include them (log_file.h includes this header for the constants below).
class BlockMap;
class LogFile;

// Device geometry and record sizes.
inline constexpr uint64_t kBlockSize = 4096;
inline constexpr uint64_t kBlockCount = 262144;
inline constexpr uint64_t kDeviceSize = kBlockSize * kBlockCount;  // 1 GiB

inline constexpr uint64_t kRecordHeaderSize = 40;
inline constexpr uint64_t kDataRecordSize = kRecordHeaderSize + kBlockSize;  // 4,136
inline constexpr uint64_t kTrimRecordSize = kRecordHeaderSize;               // header only

static_assert(kDeviceSize == 1073741824, "the device is exactly 1 GiB");
static_assert(kDataRecordSize == 4136, "a DATA record is a 40-byte header plus 4096 bytes");

// The storage engine: a transport-independent interface with byte-range calls.
// Unit tests call it directly, and the nbdkit plugin is a thin adapter
// over it. No call throws; every call returns a Status
//.
class Engine {
 public:
  // Creates an engine on the log at `path`, using `io` for all file work.
  // Fails if `cap` is under kDataRecordSize (4,136), the log cannot be opened,
  // or the log is not empty. Also fsyncs the log's parent directory.
  // On success, *out holds the engine.
  static Status create(const std::string& path, uint64_t cap, std::unique_ptr<Io> io,
                       std::unique_ptr<Engine>* out);

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

  // Public only so std::make_unique can call it (the project avoids raw `new`).
  // Nobody outside Engine can name PrivateKey, so nobody else can build an
  // Engine, and every Engine goes through create().
  struct PrivateKey {
    explicit PrivateKey() = default;
  };
  Engine(PrivateKey, std::unique_ptr<Io> io, uint64_t cap);

 private:
  // Failed if the log is in the sticky AppendFailed state (an append failed and the
  // file could not be cut back). Writes and trims call this; reads do not.
  Status check_writable() const;

  // NoSpace unless `needed` more bytes of records fit under the cap. Appends nothing.
  Status check_room(uint64_t needed) const;

  // Fills `out` (kBlockSize bytes) with the block's current contents: zeros if the
  // block is unmapped, otherwise the payload of its latest DATA record.
  Status load_block(uint64_t block, uint8_t* out);

  // Members are destroyed in reverse order, so log_ (which holds a reference to
  // *io_) goes before io_. Keep io_ first.
  std::unique_ptr<Io> io_;
  uint64_t cap_;  // limit on the log's size in bytes; the write path checks it
  std::unique_ptr<BlockMap> map_;
  std::unique_ptr<LogFile> log_;
  // FlushFailed: set when an fsync fails, and never cleared. AppendFailed lives in
  // log_->broken().
  bool flush_failed_ = false;
};

}  // namespace logblock
