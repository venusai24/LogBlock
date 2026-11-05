// Encoding and validation of log records (format version 1).
//
// All multi-byte integers are little-endian. Each field is written and read one
// at a time, never by copying a struct, so the format does not depend on
// compiler padding or on the byte order of the machine.
#include "record.h"

#include <crc32c/crc32c.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>

namespace logblock {

namespace {

// Byte offsets of the header fields .
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVersion = 4;
constexpr size_t kOffType = 6;
constexpr size_t kOffFlags = 7;
constexpr size_t kOffSeq = 8;
constexpr size_t kOffBlockNo = 16;
constexpr size_t kOffBlockCount = 24;
constexpr size_t kOffDataLen = 28;
constexpr size_t kOffReserved = 32;
constexpr size_t kOffCrc = 36;  // the CRC covers bytes [0, kOffCrc) and the payload

static_assert(kOffCrc + 4 == kRecordHeaderSize, "the CRC is the last header field");

constexpr std::array<uint8_t, 4> kMagic = {'L', 'B', 'L', 'K'};

// Writes `value` into p[0..width) least significant byte first.
void put_le(uint8_t* p, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    p[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// Reads p[0..width) as a little-endian unsigned integer.
uint64_t get_le(const uint8_t* p, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) {
    value |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return value;
}

Status corrupt(const std::string& message) { return {StatusCode::Corrupt, message}; }

// True if blocks block_no .. block_no + block_count - 1 are all inside the
// device, and there is at least one of them.
//
// The order matters. `block_no + block_count` can wrap around in 64 bits (for
// example block_no = UINT64_MAX, block_count = 2 gives 1), which would look like
// a valid range. Checking block_no < kBlockCount first rules that out, because
// the sum is then at most about 2^18 + 2^32. The final comparison is a
// subtraction, which cannot underflow after that check, so it stays correct even
// for a 64-bit block_count.
bool range_in_device(uint64_t block_no, uint64_t block_count) {
  return block_count >= 1 && block_no < kBlockCount && block_count <= kBlockCount - block_no;
}

// Writes header bytes 0-35 (everything except the CRC). flags and reserved are
// always 0 in version 1.
void write_header(uint8_t* out, RecordType type, uint64_t seq, uint64_t block_no,
                  uint32_t block_count, uint32_t data_len) {
  for (size_t i = 0; i < kMagic.size(); ++i) {
    out[kOffMagic + i] = kMagic[i];
  }
  put_le(out + kOffVersion, kFormatVersion, 2);
  out[kOffType] = static_cast<uint8_t>(type);
  out[kOffFlags] = 0;
  put_le(out + kOffSeq, seq, 8);
  put_le(out + kOffBlockNo, block_no, 8);
  put_le(out + kOffBlockCount, block_count, 4);
  put_le(out + kOffDataLen, data_len, 4);
  put_le(out + kOffReserved, 0, 4);
}

// CRC32C over header bytes 0-35 followed by the payload. `record` points at the
// start of the record, and the payload (if any) starts at kRecordHeaderSize.
uint32_t record_crc(const uint8_t* record, size_t payload_len) {
  uint32_t crc = crc32c::Crc32c(record, kOffCrc);
  if (payload_len > 0) {
    crc = crc32c::Extend(crc, record + kRecordHeaderSize, payload_len);
  }
  return crc;
}

}  // namespace

Status encode_data_record(uint64_t seq, uint64_t block_no,
                          std::span<const uint8_t, kBlockSize> payload,
                          std::span<uint8_t, kDataRecordSize> out) {
  if (!range_in_device(block_no, 1)) {
    return {StatusCode::OutOfRange,
            std::format("DATA record for block {} is outside the device ({} blocks)", block_no,
                        kBlockCount)};
  }
  uint8_t* p = out.data();
  write_header(p, RecordType::Data, seq, block_no, 1, static_cast<uint32_t>(kBlockSize));
  std::ranges::copy(payload, p + kRecordHeaderSize);
  put_le(p + kOffCrc, record_crc(p, kBlockSize), 4);
  return Status::Ok();
}

Status encode_trim_record(uint64_t seq, uint64_t block_no, uint32_t block_count,
                          std::span<uint8_t, kTrimRecordSize> out) {
  if (!range_in_device(block_no, block_count)) {
    return {StatusCode::OutOfRange,
            std::format("TRIM record for blocks {} + {} is empty or outside the device ({} blocks)",
                        block_no, block_count, kBlockCount)};
  }
  uint8_t* p = out.data();
  write_header(p, RecordType::Trim, seq, block_no, block_count, 0);
  put_le(p + kOffCrc, record_crc(p, 0), 4);
  return Status::Ok();
}

Status decode_header(std::span<const uint8_t, kRecordHeaderSize> bytes, RecordHeader* out) {
  const uint8_t* p = bytes.data();

  // Rule 1: magic.
  for (size_t i = 0; i < kMagic.size(); ++i) {
    if (p[kOffMagic + i] != kMagic[i]) {
      return corrupt("rule 1: bad magic");
    }
  }
  // Rule 2: version. Checked before anything else is parsed, so a reader never
  // misreads a newer format.
  const uint64_t version = get_le(p + kOffVersion, 2);
  if (version != kFormatVersion) {
    return corrupt(std::format("rule 2: unsupported version {}", version));
  }
  // Rule 3: type. 0 is invalid on purpose, so zeroed space is never a record.
  const uint8_t type_byte = p[kOffType];
  if (type_byte != static_cast<uint8_t>(RecordType::Data) &&
      type_byte != static_cast<uint8_t>(RecordType::Trim)) {
    return corrupt(std::format("rule 3: invalid type {}", type_byte));
  }
  // Rule 4: flags and reserved must be 0 in version 1.
  if (p[kOffFlags] != 0) {
    return corrupt(std::format("rule 4: flags is {}, expected 0", p[kOffFlags]));
  }
  const uint64_t reserved = get_le(p + kOffReserved, 4);
  if (reserved != 0) {
    return corrupt(std::format("rule 4: reserved is {}, expected 0", reserved));
  }

  RecordHeader header{};
  header.type = static_cast<RecordType>(type_byte);
  header.seq = get_le(p + kOffSeq, 8);
  header.block_no = get_le(p + kOffBlockNo, 8);
  header.block_count = static_cast<uint32_t>(get_le(p + kOffBlockCount, 4));
  header.data_len = static_cast<uint32_t>(get_le(p + kOffDataLen, 4));

  // Rule 5: type-specific fields.
  if (header.type == RecordType::Data) {
    if (header.block_count != 1 || header.data_len != kBlockSize) {
      return corrupt(std::format("rule 5: DATA record has block_count {} and data_len {}",
                                 header.block_count, header.data_len));
    }
  } else {
    if (header.block_count < 1 || header.data_len != 0) {
      return corrupt(std::format("rule 5: TRIM record has block_count {} and data_len {}",
                                 header.block_count, header.data_len));
    }
  }
  // Rule 6: the whole block range is inside the device.
  if (!range_in_device(header.block_no, header.block_count)) {
    return corrupt(std::format("rule 6: blocks {} + {} are outside the device ({} blocks)",
                               header.block_no, header.block_count, kBlockCount));
  }

  *out = header;
  return Status::Ok();
}

Status decode_record(std::span<const uint8_t> bytes, Record* out) {
  if (bytes.size() < kRecordHeaderSize) {
    return corrupt(
        std::format("rule 7: {} bytes left, a header needs {}", bytes.size(), kRecordHeaderSize));
  }
  RecordHeader header{};
  if (Status s = decode_header(bytes.first<kRecordHeaderSize>(), &header); !s.ok()) {
    return s;
  }

  // Rule 7: the whole record fits. data_len is 0 or 4096 here (rule 5), so the
  // addition cannot overflow.
  const size_t total = kRecordHeaderSize + header.data_len;
  if (bytes.size() < total) {
    return corrupt(std::format("rule 7: record needs {} bytes but only {} are available", total,
                               bytes.size()));
  }

  // Rule 8: CRC.
  const auto stored = static_cast<uint32_t>(get_le(bytes.data() + kOffCrc, 4));
  const uint32_t computed = record_crc(bytes.data(), header.data_len);
  if (stored != computed) {
    return corrupt(std::format("rule 8: crc32c mismatch (stored {:#010x}, computed {:#010x})",
                               stored, computed));
  }

  out->header = header;
  out->payload = bytes.subspan(kRecordHeaderSize, header.data_len);
  return Status::Ok();
}

}  // namespace logblock
