#pragma once

// The log scanner: walks a whole log image and applies rules 1 to 9 of
// the record format to every record, in order.
//
// It works on bytes in memory and knows nothing about Engine, Io or FaultIo, so
// the same function serves three callers: unit tests (a std::string from FaultIo),
// the kill test (a file read into memory) and M2 replay.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "record.h"

namespace logblock {

// Why scanning stopped.
enum class ScanStop : uint8_t {
  Clean,      // every byte belongs to a valid record (an empty file is clean)
  TornWrite,  // fewer than 40 bytes are left, or a header passing rules 1-6 runs past the end
  Corrupt,    // anything else: a bad header, bad CRC, seq out of order, zeroed space
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

}  // namespace logblock
