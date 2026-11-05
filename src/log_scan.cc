// The log scanner: rules 1 to 9 over a whole log image.
#include "log_scan.h"

#include <string>

namespace logblock {

namespace {

// Fills in how and where scanning stopped. `pos` is the start of the bad position.
ScanResult stop_at(ScanResult result, ScanStop stop, uint64_t pos, const std::string& why) {
  result.stop = stop;
  result.valid_end = pos;
  result.detail = "at offset " + std::to_string(pos) + ": " + why;
  return result;
}

}  // namespace

ScanResult scan_log(std::span<const uint8_t> file) {
  ScanResult result;
  uint64_t pos = 0;           // start of the record being examined
  uint64_t expected_seq = 1;  // rule 9: seq starts at 1 and grows by 1 with no gaps

  while (pos < file.size()) {
    const uint64_t remaining = file.size() - pos;

    // Too few bytes for even a header: the file ends in the middle of a header write.
    if (remaining < kRecordHeaderSize) {
      return stop_at(std::move(result), ScanStop::TornWrite, pos,
                     std::to_string(remaining) + " bytes are left, fewer than a 40-byte header");
    }

    // Rules 1 to 6 on the header alone. A bad header is corruption, even when the
    // record would also run past the end: a torn write only ever cuts a record that
    // was written correctly, so its header must be valid.
    RecordHeader header;
    const std::span<const uint8_t, kRecordHeaderSize> header_bytes(file.data() + pos,
                                                                   kRecordHeaderSize);
    if (Status s = decode_header(header_bytes, &header); !s.ok()) {
      return stop_at(std::move(result), ScanStop::Corrupt, pos, s.message());
    }

    // A valid header whose record does not fit: the write was cut short (torn). The
    // CRC cannot be checked without the payload, so this is all we can say.
    const uint64_t record_size = kRecordHeaderSize + header.data_len;
    if (record_size > remaining) {
      return stop_at(std::move(result), ScanStop::TornWrite, pos,
                     "a record of " + std::to_string(record_size) + " bytes has only " +
                         std::to_string(remaining) + " bytes in the file");
    }

    // Rules 7 and 8 (the record fits, the CRC matches). The record is complete here,
    // so any failure means damaged bytes, not a torn write.
    Record record;
    if (Status s = decode_record(file.subspan(pos), &record); !s.ok()) {
      return stop_at(std::move(result), ScanStop::Corrupt, pos, s.message());
    }

    // Rule 9: seq must follow on from the previous record.
    if (header.seq != expected_seq) {
      return stop_at(
          std::move(result), ScanStop::Corrupt, pos,
          "seq is " + std::to_string(header.seq) + ", expected " + std::to_string(expected_seq));
    }

    result.records.push_back({header, pos});
    pos += record_size;
    ++expected_seq;
  }

  result.valid_end = pos;  // Clean: every byte belongs to a valid record
  return result;
}

}  // namespace logblock
