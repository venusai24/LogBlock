#pragma once

// The append-only log: appends records at explicit offsets and reads them back.
//
// This class knows nothing about the cap or the block map. The engine does the cap
// check before it calls append_*. A failed append is cleaned up here, and the
// engine turns broken() into its sticky AppendFailed state.

#include <array>
#include <cstdint>
#include <span>

#include "logblock/engine.h"
#include "logblock/io.h"
#include "logblock/status.h"
#include "record.h"

namespace logblock {

class LogFile {
 public:
  // Works on a log that is empty and already open. M1 refuses a non-empty log at
  // startup, so appends always begin at offset 0 with seq 1.
  explicit LogFile(Io& io) : io_(io) {}

  // The offset just past the last complete record, which is also the number of
  // bytes in the log's records. Advances only after an append fully succeeds.
  [[nodiscard]] uint64_t end() const { return end_; }

  // The seq the next record will get. Starts at 1 and advances with end().
  [[nodiscard]] uint64_t next_seq() const { return next_seq_; }

  // Appends one DATA record for `block_no` at end(), as a single pwrite. On
  // success *record_offset is where the record starts and end() and next_seq()
  // move past it. On failure nothing moves.
  Status append_data(uint64_t block_no, std::span<const uint8_t, kBlockSize> payload,
                     uint64_t* record_offset);

  // Appends one header-only TRIM record for block_count blocks starting at
  // block_no, at end(), as a single pwrite. Same success and failure behaviour.
  Status append_trim(uint64_t block_no, uint32_t block_count);

  // True once an append failed and the file could not be cut back to end().
  // Torn bytes may then sit past end(), so no further append is attempted: append_*
  // return Failed. Reads of records before end() stay valid. There is no way back.
  [[nodiscard]] bool broken() const { return broken_; }

  // Reads the record that starts at `offset` and checks rules 1 to 8 of
  // the record format. `buffer` holds the bytes that were read, and
  // record->payload points into it, so keep it alive while you use the record.
  // Every validation failure is Corrupt, including an offset at or past the end
  // of the log. Does not change end() or next_seq().
  Status read_record(uint64_t offset, std::array<uint8_t, kDataRecordSize>* buffer, Record* record);

 private:
  // Writes one encoded record at end_ and, only if that succeeds, advances end_
  // and next_seq_. If the write fails, cuts the file back to end_ (ftruncate) so
  // torn bytes cannot stay in the middle of the log; if that fails too, sets broken_.
  Status write_record(std::span<const uint8_t> record);

  Io& io_;
  uint64_t end_ = 0;
  uint64_t next_seq_ = 1;
  bool broken_ = false;
};

}  // namespace logblock
