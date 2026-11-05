#include "record.h"

#include <crc32c/crc32c.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace logblock {
namespace {

using Bytes = std::vector<uint8_t>;

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// Byte offsets of the header fields.
constexpr size_t kOffMagic = 0;
constexpr size_t kOffVersion = 4;
constexpr size_t kOffType = 6;
constexpr size_t kOffFlags = 7;
constexpr size_t kOffSeq = 8;
constexpr size_t kOffBlockNo = 16;
constexpr size_t kOffBlockCount = 24;
constexpr size_t kOffDataLen = 28;
constexpr size_t kOffReserved = 32;
constexpr size_t kOffCrc = 36;

// ---------------------------------------------------------------------------
// Helpers that build records by hand, so the decode tests do not depend on the
// encode code, and each rejection test breaks exactly one rule.
// ---------------------------------------------------------------------------

void put_le(Bytes& b, size_t offset, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    b[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// The test payload: byte i is i % 251, so it is not periodic at 256 and every
// position holds a predictable value.
Bytes pattern_payload() {
  Bytes p(kBlockSize);
  for (size_t i = 0; i < p.size(); ++i) {
    p[i] = static_cast<uint8_t>(i % 251);
  }
  return p;
}

// Recomputes the CRC over header bytes 0-35 followed by the payload, and stores it.
void seal(Bytes& rec) {
  uint32_t crc = crc32c::Crc32c(rec.data(), kOffCrc);
  if (rec.size() > kRecordHeaderSize) {
    crc = crc32c::Extend(crc, rec.data() + kRecordHeaderSize, rec.size() - kRecordHeaderSize);
  }
  put_le(rec, kOffCrc, crc, 4);
}

Bytes build_record(uint8_t type, uint64_t seq, uint64_t block_no, uint32_t block_count,
                   uint32_t data_len, const Bytes& payload) {
  Bytes rec(kRecordHeaderSize + payload.size(), 0);
  rec[kOffMagic + 0] = 'L';
  rec[kOffMagic + 1] = 'B';
  rec[kOffMagic + 2] = 'L';
  rec[kOffMagic + 3] = 'K';
  put_le(rec, kOffVersion, 1, 2);
  rec[kOffType] = type;
  put_le(rec, kOffSeq, seq, 8);
  put_le(rec, kOffBlockNo, block_no, 8);
  put_le(rec, kOffBlockCount, block_count, 4);
  put_le(rec, kOffDataLen, data_len, 4);
  std::copy(payload.begin(), payload.end(), rec.begin() + kRecordHeaderSize);
  seal(rec);
  return rec;
}

Bytes valid_data(uint64_t block_no = 7) {
  return build_record(1, 5, block_no, 1, 4096, pattern_payload());
}
Bytes valid_trim(uint64_t block_no = 10, uint32_t count = 3) {
  return build_record(2, 6, block_no, count, 0, {});
}

std::span<const uint8_t, kRecordHeaderSize> header_of(const Bytes& rec) {
  return std::span<const uint8_t, kRecordHeaderSize>(rec.data(), kRecordHeaderSize);
}

// ---------------------------------------------------------------------------
// Encoding: exact bytes.
// ---------------------------------------------------------------------------

TEST(RecordEncode, DataRecordExactBytes) {
  // Header bytes 0-35, written out by hand from the format description. Using distinct
  // values makes a byte-order mistake visible: seq = 0x1122334455667788 and
  // block_no = 0x010203 must appear least significant byte first.
  const Bytes header = {
      0x4C, 0x42, 0x4C, 0x4B,                          // magic "LBLK"
      0x01, 0x00,                                      // version 1
      0x01,                                            // type DATA
      0x00,                                            // flags
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // seq
      0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,  // block_no
      0x01, 0x00, 0x00, 0x00,                          // block_count 1
      0x00, 0x10, 0x00, 0x00,                          // data_len 4096
      0x00, 0x00, 0x00, 0x00,                          // reserved
  };
  // CRC32C over those 36 bytes plus the payload, computed outside this program
  // with an independent bitwise implementation (checked against the published
  // vectors for "123456789" and 32 zero bytes).
  const Bytes crc_le = {0xD6, 0xD4, 0x32, 0x27};  // 0x2732D4D6

  Bytes expected = header;
  expected.insert(expected.end(), crc_le.begin(), crc_le.end());
  Bytes payload = pattern_payload();
  expected.insert(expected.end(), payload.begin(), payload.end());
  ASSERT_EQ(expected.size(), kDataRecordSize);

  std::array<uint8_t, kDataRecordSize> out{};
  std::span<const uint8_t, kBlockSize> payload_span(payload.data(), kBlockSize);
  Status s = encode_data_record(0x1122334455667788ULL, 0x010203, payload_span, out);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(Bytes(out.begin(), out.end()), expected);
}

TEST(RecordEncode, TrimRecordExactBytes) {
  // A whole-device trim: block_no 0, block_count 262,144 (0x00040000).
  const Bytes expected = {
      0x4C, 0x42, 0x4C, 0x4B,                          // magic
      0x01, 0x00,                                      // version
      0x02,                                            // type TRIM
      0x00,                                            // flags
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // seq 2
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // block_no 0
      0x00, 0x00, 0x04, 0x00,                          // block_count 262144
      0x00, 0x00, 0x00, 0x00,                          // data_len 0
      0x00, 0x00, 0x00, 0x00,                          // reserved
      0x8A, 0x1F, 0x43, 0x26,                          // crc32c 0x26431F8A
  };
  ASSERT_EQ(expected.size(), kTrimRecordSize);

  std::array<uint8_t, kTrimRecordSize> out{};
  Status s = encode_trim_record(2, 0, static_cast<uint32_t>(kBlockCount), out);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(Bytes(out.begin(), out.end()), expected);
}

// Guards the tests themselves, and needs no record code: the hand-written golden
// bytes above must equal what the test helpers build with google-crc32c. If a
// golden constant were mistyped, this fails now instead of sending you chasing
// a bug in your encoder.
TEST(RecordTestHelpers, HandBuiltRecordsMatchTheGoldenBytes) {
  Bytes data = build_record(1, 0x1122334455667788ULL, 0x010203, 1, 4096, pattern_payload());
  ASSERT_EQ(data.size(), kDataRecordSize);
  EXPECT_EQ(Bytes(data.begin() + kOffCrc, data.begin() + kOffCrc + 4),
            (Bytes{0xD6, 0xD4, 0x32, 0x27}));

  Bytes trim = build_record(2, 2, 0, static_cast<uint32_t>(kBlockCount), 0, {});
  ASSERT_EQ(trim.size(), kTrimRecordSize);
  EXPECT_EQ(Bytes(trim.begin() + kOffCrc, trim.end()), (Bytes{0x8A, 0x1F, 0x43, 0x26}));
}

TEST(RecordEncode, DataRecordRejectsABlockOutsideTheDevice) {
  Bytes payload = pattern_payload();
  std::span<const uint8_t, kBlockSize> payload_span(payload.data(), kBlockSize);
  std::array<uint8_t, kDataRecordSize> out{};
  // The last block is fine; one past it is not.
  EXPECT_TRUE(encode_data_record(1, kBlockCount - 1, payload_span, out).ok());
  EXPECT_EQ(encode_data_record(1, kBlockCount, payload_span, out).code(), StatusCode::OutOfRange);
}

TEST(RecordEncode, TrimRecordRejectsEmptyAndOutOfRangeRanges) {
  std::array<uint8_t, kTrimRecordSize> out{};
  EXPECT_TRUE(encode_trim_record(1, kBlockCount - 1, 1, out).ok());
  EXPECT_TRUE(encode_trim_record(1, 0, static_cast<uint32_t>(kBlockCount), out).ok());

  EXPECT_EQ(encode_trim_record(1, 5, 0, out).code(), StatusCode::OutOfRange);  // empty range
  EXPECT_EQ(encode_trim_record(1, kBlockCount - 1, 2, out).code(), StatusCode::OutOfRange);
  EXPECT_EQ(encode_trim_record(1, kBlockCount, 1, out).code(), StatusCode::OutOfRange);
  // block_no + block_count wraps around in 64 bits; it must not look valid.
  EXPECT_EQ(encode_trim_record(1, std::numeric_limits<uint64_t>::max(), 2, out).code(),
            StatusCode::OutOfRange);
}

// ---------------------------------------------------------------------------
// Decoding valid records.
// ---------------------------------------------------------------------------

TEST(RecordDecode, ValidDataRecord) {
  Bytes rec = valid_data(7);
  Record r{};
  Status s = decode_record(rec, &r);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(r.header.type, RecordType::Data);
  EXPECT_EQ(r.header.seq, 5U);
  EXPECT_EQ(r.header.block_no, 7U);
  EXPECT_EQ(r.header.block_count, 1U);
  EXPECT_EQ(r.header.data_len, 4096U);
  ASSERT_EQ(r.payload.size(), 4096U);
  EXPECT_EQ(Bytes(r.payload.begin(), r.payload.end()), pattern_payload());
}

TEST(RecordDecode, ValidTrimRecord) {
  Bytes rec = valid_trim(10, 3);
  Record r{};
  Status s = decode_record(rec, &r);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(r.header.type, RecordType::Trim);
  EXPECT_EQ(r.header.seq, 6U);
  EXPECT_EQ(r.header.block_no, 10U);
  EXPECT_EQ(r.header.block_count, 3U);
  EXPECT_EQ(r.header.data_len, 0U);
  EXPECT_TRUE(r.payload.empty());
}

TEST(RecordDecode, AcceptsTheDeviceBoundaries) {
  Record r{};
  EXPECT_TRUE(decode_record(valid_data(kBlockCount - 1), &r).ok());  // last block
  EXPECT_TRUE(decode_record(valid_data(0), &r).ok());                // first block
  EXPECT_TRUE(decode_record(valid_trim(0, static_cast<uint32_t>(kBlockCount)), &r).ok());
  EXPECT_TRUE(decode_record(valid_trim(kBlockCount - 1, 1), &r).ok());
}

TEST(RecordDecode, IgnoresBytesAfterTheRecord) {
  // `bytes` runs to the end of the file, so the next record may follow.
  Bytes rec = valid_data(7);
  Bytes next = valid_trim();
  rec.insert(rec.end(), next.begin(), next.end());
  Record r{};
  ASSERT_TRUE(decode_record(rec, &r).ok());
  EXPECT_EQ(r.payload.size(), 4096U);
}

TEST(RecordDecode, EncodeThenDecodeRoundTrips) {
  Bytes payload = pattern_payload();
  std::array<uint8_t, kDataRecordSize> data{};
  std::span<const uint8_t, kBlockSize> payload_span(payload.data(), kBlockSize);
  ASSERT_TRUE(encode_data_record(42, 1234, payload_span, data).ok());
  Record r{};
  ASSERT_TRUE(decode_record(data, &r).ok());
  EXPECT_EQ(r.header.type, RecordType::Data);
  EXPECT_EQ(r.header.seq, 42U);
  EXPECT_EQ(r.header.block_no, 1234U);
  EXPECT_EQ(Bytes(r.payload.begin(), r.payload.end()), payload);

  std::array<uint8_t, kTrimRecordSize> trim{};
  ASSERT_TRUE(encode_trim_record(43, 100, 50, trim).ok());
  ASSERT_TRUE(decode_record(trim, &r).ok());
  EXPECT_EQ(r.header.type, RecordType::Trim);
  EXPECT_EQ(r.header.seq, 43U);
  EXPECT_EQ(r.header.block_no, 100U);
  EXPECT_EQ(r.header.block_count, 50U);
}

// ---------------------------------------------------------------------------
// Rejection. Each case changes one thing in a valid record and re-seals the CRC,
// so the CRC is correct and the named rule is the only reason to reject.
// ---------------------------------------------------------------------------

struct Mutation {
  const char* name;
  std::function<void(Bytes&)> apply;
};

// Checks the base record is valid, applies the mutation, re-seals, and expects
// both the full decode and (for the header rules) the header-only decode to fail
// with Corrupt.
void expect_header_rule_rejects(const Bytes& base, const Mutation& m) {
  SCOPED_TRACE(m.name);
  Record valid_record{};
  ASSERT_TRUE(decode_record(base, &valid_record).ok()) << "the unmutated record must be valid";

  Bytes rec = base;
  m.apply(rec);
  seal(rec);

  Record r{};
  EXPECT_EQ(decode_record(rec, &r).code(), StatusCode::Corrupt);
  RecordHeader h{};
  EXPECT_EQ(decode_header(header_of(rec), &h).code(), StatusCode::Corrupt);
}

TEST(RecordDecode, RejectsDataRecordsThatBreakAHeaderRule) {
  const std::vector<Mutation> mutations = {
      {"rule 1: bad magic", [](Bytes& r) { r[kOffMagic] = 'X'; }},
      {"rule 1: magic in the wrong case", [](Bytes& r) { r[kOffMagic + 3] = 'k'; }},
      {"rule 2: version 0", [](Bytes& r) { put_le(r, kOffVersion, 0, 2); }},
      {"rule 2: version 2", [](Bytes& r) { put_le(r, kOffVersion, 2, 2); }},
      {"rule 2: version 257", [](Bytes& r) { put_le(r, kOffVersion, 257, 2); }},
      {"rule 3: type 0", [](Bytes& r) { r[kOffType] = 0; }},
      {"rule 3: type 3", [](Bytes& r) { r[kOffType] = 3; }},
      {"rule 3: type 255", [](Bytes& r) { r[kOffType] = 255; }},
      {"rule 4: flags 1", [](Bytes& r) { r[kOffFlags] = 1; }},
      {"rule 4: flags 128", [](Bytes& r) { r[kOffFlags] = 128; }},
      {"rule 4: reserved 1", [](Bytes& r) { put_le(r, kOffReserved, 1, 4); }},
      {"rule 4: reserved in the top byte", [](Bytes& r) { r[kOffReserved + 3] = 1; }},
      {"rule 5: DATA block_count 0", [](Bytes& r) { put_le(r, kOffBlockCount, 0, 4); }},
      {"rule 5: DATA block_count 2", [](Bytes& r) { put_le(r, kOffBlockCount, 2, 4); }},
      {"rule 5: DATA data_len 0", [](Bytes& r) { put_le(r, kOffDataLen, 0, 4); }},
      {"rule 5: DATA data_len 4095", [](Bytes& r) { put_le(r, kOffDataLen, 4095, 4); }},
      {"rule 5: DATA data_len 8192", [](Bytes& r) { put_le(r, kOffDataLen, 8192, 4); }},
      {"rule 6: block_no is the device's block count",
       [](Bytes& r) { put_le(r, kOffBlockNo, kBlockCount, 8); }},
      {"rule 6: block_no is the largest uint64",
       [](Bytes& r) { put_le(r, kOffBlockNo, std::numeric_limits<uint64_t>::max(), 8); }},
  };
  for (const Mutation& m : mutations) {
    expect_header_rule_rejects(valid_data(), m);
  }
}

TEST(RecordDecode, RejectsTrimRecordsThatBreakAHeaderRule) {
  const std::vector<Mutation> mutations = {
      {"rule 5: TRIM block_count 0", [](Bytes& r) { put_le(r, kOffBlockCount, 0, 4); }},
      {"rule 5: TRIM data_len 1", [](Bytes& r) { put_le(r, kOffDataLen, 1, 4); }},
      {"rule 5: TRIM data_len 4096", [](Bytes& r) { put_le(r, kOffDataLen, 4096, 4); }},
      {"rule 6: range ends one block past the device",
       [](Bytes& r) {
         put_le(r, kOffBlockNo, kBlockCount - 1, 8);
         put_le(r, kOffBlockCount, 2, 4);
       }},
      {"rule 6: block_no past the device",
       [](Bytes& r) { put_le(r, kOffBlockNo, kBlockCount, 8); }},
      {"rule 6: block_count larger than the device",
       [](Bytes& r) { put_le(r, kOffBlockCount, kBlockCount + 1, 4); }},
      {"rule 6: block_count is the largest uint32",
       [](Bytes& r) { put_le(r, kOffBlockCount, std::numeric_limits<uint32_t>::max(), 4); }},
      {"rule 6: block_no + block_count overflows a uint64",
       [](Bytes& r) {
         put_le(r, kOffBlockNo, std::numeric_limits<uint64_t>::max(), 8);
         put_le(r, kOffBlockCount, 2, 4);
       }},
  };
  for (const Mutation& m : mutations) {
    expect_header_rule_rejects(valid_trim(), m);
  }
}

TEST(RecordDecode, RejectsARecordThatDoesNotFitInTheFile) {
  Record r{};
  Bytes rec = valid_data();
  ASSERT_TRUE(decode_record(rec, &r).ok());

  // The message must name rule 7. A truncated record would usually fail the CRC
  // as well (the CRC would read bytes that are not there), so checking the code
  // alone would pass even if the length check were missing, and ASan cannot see
  // the out-of-bounds read because it happens inside libcrc32c.
  auto expect_rule_7 = [&](const Bytes& cut) {
    Status s = decode_record(cut, &r);
    EXPECT_EQ(s.code(), StatusCode::Corrupt);
    EXPECT_TRUE(contains(s.message(), "rule 7")) << s.message();
  };

  // Rule 7: the header is intact but the payload is cut short.
  for (size_t keep : {rec.size() - 1, rec.size() - 100, kRecordHeaderSize}) {
    SCOPED_TRACE("keep " + std::to_string(keep) + " bytes");
    expect_rule_7(Bytes(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(keep)));
  }
  // Less than a whole header.
  for (size_t keep : {size_t{39}, size_t{20}, size_t{1}, size_t{0}}) {
    SCOPED_TRACE("keep " + std::to_string(keep) + " bytes");
    expect_rule_7(Bytes(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(keep)));
  }
  // A TRIM record cut inside its header.
  Bytes trim = valid_trim();
  expect_rule_7(Bytes(trim.begin(), trim.begin() + 39));
}

TEST(RecordDecode, RejectsACrcMismatch) {
  // Rule 8. Here the bytes change and the CRC is NOT recomputed.
  const struct {
    const char* name;
    Bytes base;
    size_t offset;
  } cases[] = {
      {"DATA header byte (seq)", valid_data(), kOffSeq},
      {"DATA header byte (block_no)", valid_data(), kOffBlockNo},
      {"DATA payload first byte", valid_data(), kRecordHeaderSize},
      {"DATA payload last byte", valid_data(), kDataRecordSize - 1},
      {"DATA crc field", valid_data(), kOffCrc},
      {"TRIM header byte (seq)", valid_trim(), kOffSeq},
      {"TRIM crc field", valid_trim(), kOffCrc + 3},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    Record r{};
    ASSERT_TRUE(decode_record(c.base, &r).ok());
    Bytes rec = c.base;
    rec[c.offset] ^= 0x01;
    EXPECT_EQ(decode_record(rec, &r).code(), StatusCode::Corrupt);
  }
}

TEST(RecordDecode, RejectsAZeroedRegion) {
  // Type 0 is invalid so that never-written or preallocated space can never
  // look like a record .
  Record r{};
  EXPECT_EQ(decode_record(Bytes(kDataRecordSize, 0), &r).code(), StatusCode::Corrupt);
  EXPECT_EQ(decode_record(Bytes(kRecordHeaderSize, 0), &r).code(), StatusCode::Corrupt);
  Bytes zeros(kRecordHeaderSize, 0);
  RecordHeader h{};
  EXPECT_EQ(decode_header(header_of(zeros), &h).code(), StatusCode::Corrupt);
}

// ---------------------------------------------------------------------------
// decode_header checks rules 1 to 6 only, so the log scanner can tell a torn
// write from corruption .
// ---------------------------------------------------------------------------

TEST(RecordDecodeHeader, AcceptsAValidHeader) {
  Bytes rec = valid_data(9);
  RecordHeader h{};
  Status s = decode_header(header_of(rec), &h);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(h.type, RecordType::Data);
  EXPECT_EQ(h.seq, 5U);
  EXPECT_EQ(h.block_no, 9U);
  EXPECT_EQ(h.block_count, 1U);
  EXPECT_EQ(h.data_len, 4096U);
}

TEST(RecordDecodeHeader, DoesNotCheckTheCrc) {
  Bytes rec = valid_data();
  rec[kOffCrc] ^= 0xFF;  // wrong CRC, but a perfectly good header
  RecordHeader h{};
  EXPECT_TRUE(decode_header(header_of(rec), &h).ok());
  Record r{};
  EXPECT_EQ(decode_record(rec, &r).code(), StatusCode::Corrupt);
}

TEST(RecordDecodeHeader, DoesNotRequireThePayloadToExist) {
  // Only the 40 header bytes are looked at: a header that passes rules 1 to 6
  // but whose record runs past the end of the file is a torn write.
  Bytes rec = valid_data();
  Bytes header_only(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderSize));
  RecordHeader h{};
  EXPECT_TRUE(decode_header(header_of(header_only), &h).ok());
  EXPECT_EQ(h.data_len, 4096U);
}

}  // namespace
}  // namespace logblock
