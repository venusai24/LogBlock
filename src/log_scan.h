#pragma once

// The log scanner: walks a whole log image and applies rules 1 to 9 of
// the record format to every record, in order.
//
// It works on bytes in memory and knows nothing about Engine, Io or FaultIo, so
// the same function serves three callers: unit tests (a std::string from FaultIo),
// the kill test (a file read into memory) and M2 replay.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "logblock/io.h"
#include "logblock/status.h"
#include "record.h"

namespace logblock {

// Why scanning stopped.
enum class ScanStop : uint8_t {
  Clean,  // every byte belongs to a valid record (an empty file is clean)
  // Fewer than 40 bytes are left, a header passing rules 1-6 runs past the end, or every byte
  // from a record boundary to the end of the file is zero (D19).
  TornWrite,
  // Anything else: a bad header, bad CRC, seq out of order, or zeros followed by a non-zero byte.
  Corrupt,
};

// One valid record. The payload is not copied: `offset` says where the record
// starts, so a caller that wants the payload can slice the file itself.
struct ScannedRecord {
  RecordHeader header;
  uint64_t offset;
};

struct ScanResult {
  // Every valid record before the first invalid position, in file order.
  std::vector<ScannedRecord> records;
  // Offset just past the last valid record. For Clean it equals the file size. For
  // TornWrite and Corrupt it is where the first invalid position starts (M2 replay
  // truncates a torn tail to here).
  uint64_t valid_end = 0;
  ScanStop stop = ScanStop::Clean;
  // Human-readable reason and offset, empty for Clean.
  std::string detail;
};

inline const char* scan_stop_name(ScanStop stop) {
  switch (stop) {
    case ScanStop::Clean:
      return "Clean";
    case ScanStop::TornWrite:
      return "TornWrite";
    case ScanStop::Corrupt:
      return "Corrupt";
  }
  return "Unknown";
}

// Scans `file` from offset 0. There is no error return: every outcome, including
// "the file is damaged", is a classification in the result.
ScanResult scan_log(std::span<const uint8_t> file);

// --- Streaming scan ----------------------------------------------------------------
// The same rules 1 to 9, but reading through Io in chunks, starting anywhere, and handing each
// valid record to a visitor instead of collecting them. Memory use is one chunk plus at most one
// record, whatever the file size or record count.

// Default read size. Tests pass tiny chunks (even smaller than one record) to force boundaries.
inline constexpr size_t kScanChunkSize = 1 << 20;

struct StreamScanResult {
  ScanStop stop = ScanStop::Clean;
  // Same meaning as ScanResult::valid_end: an absolute file offset.
  uint64_t valid_end = 0;
  // The seq the next record would get: the last valid seq + 1, or the expected_seq passed in if
  // no record was valid.
  uint64_t next_seq = 0;
  // How many valid records were visited.
  uint64_t records = 0;
  // Human-readable reason and absolute offset, empty for Clean.
  std::string detail;
};

using RecordVisitor = std::function<void(const ScannedRecord&)>;

// Scans the log held by `io` (already open) from `start_offset`, where the first record must have
// seq `expected_seq`. Calls `visit` for each valid record in order. If file_size or pread fails,
// returns that status and *out is unspecified. Otherwise returns Ok with the classification in
// *out. A `start_offset` past the end of the file is Corrupt (the caller's offset points at
// bytes that are not there), not TornWrite. One exactly at the end is Clean.
Status scan_log_stream(Io& io, uint64_t start_offset, uint64_t expected_seq,
                       const RecordVisitor& visit, StreamScanResult* out,
                       size_t chunk_size = kScanChunkSize);

}  // namespace logblock
