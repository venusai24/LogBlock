#pragma once

// On-disk log record: encode, decode and validate (format version 1).

#include <cstdint>
#include <span>

#include "logblock/engine.h"
#include "logblock/status.h"

namespace logblock {

inline constexpr uint16_t kFormatVersion = 1;

enum class RecordType : uint8_t { Data = 1, Trim = 2 };

// The fields of a record header that a caller needs. magic, version, flags,
// reserved and the CRC are fixed by the format, so they are not stored here:
// decoding checks them, and encoding writes them.
struct RecordHeader {
  RecordType type;
  uint64_t seq;
  uint64_t block_no;     // first logical block affected
  uint32_t block_count;  // DATA: 1. TRIM: 1 or more
  uint32_t data_len;     // DATA: 4096. TRIM: 0
};

// A fully validated record. `payload` points into the bytes that were decoded,
// so it is only valid while those bytes are.
struct Record {
  RecordHeader header;
  std::span<const uint8_t> payload;  // 4096 bytes for DATA, empty for TRIM
};

// Writes one DATA record (40-byte header + 4096-byte payload) into `out`.
// Returns OutOfRange if block_no is not a block of the device.
Status encode_data_record(uint64_t seq, uint64_t block_no,
                          std::span<const uint8_t, kBlockSize> payload,
                          std::span<uint8_t, kDataRecordSize> out);

// Writes one header-only TRIM record into `out`. Returns OutOfRange if
// block_count is 0 or the range block_no .. block_no + block_count - 1 is not
// inside the device (including when block_no + block_count would overflow).
Status encode_trim_record(uint64_t seq, uint64_t block_no, uint32_t block_count,
                          std::span<uint8_t, kTrimRecordSize> out);

// Decodes a 40-byte header and checks rules 1 to 6: magic,
// version, type, flags and reserved, type-specific fields, and the block range.
// It does not check the CRC or that the payload exists, so the log scanner can
// tell a torn write (a valid header whose record runs past the end of the file)
// from corruption. Every failure is Corrupt.
Status decode_header(std::span<const uint8_t, kRecordHeaderSize> bytes, RecordHeader* out);

// Decodes and fully validates one record that starts at bytes[0]. `bytes` is
// everything from the record's start to the end of the file, so it may hold
// more than this record. Checks rules 1 to 8: the header rules above, that the
// whole record fits in `bytes`, and the CRC. Every failure is Corrupt.
// Rule 9 (seq order) needs the previous record, so only the scanner checks it.
Status decode_record(std::span<const uint8_t> bytes, Record* out);

}  // namespace logblock
