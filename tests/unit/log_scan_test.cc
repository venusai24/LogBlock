// Tests for the log scanner (rules 1 to 9, torn writes versus corruption). The scanner walks a
// whole log image and reports the valid records and why it stopped: Clean (every byte is a valid
// record), TornWrite (an unfinished record at the end) or Corrupt (anything else). Logs are built
// from encode_data_record and encode_trim_record; to test one rule at a time, a record's bytes are
// changed and its CRC is recomputed ("resealed"), so only the changed rule can make the record
// invalid.
#include "log_scan.h"

#include <crc32c/crc32c.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "logblock/engine.h"
#include "record.h"

namespace logblock {
namespace {

using Bytes = std::vector<uint8_t>;

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

void put_le(Bytes& b, size_t offset, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    b[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// Recomputes the CRC of the record that starts at `start` and is `len` bytes long (the CRC
// covers header bytes 0-35 and the payload), and stores it in the header.
void reseal(Bytes& file, size_t start, size_t len) {
  uint32_t crc = crc32c::Crc32c(file.data() + start, kOffCrc);
  if (len > kRecordHeaderSize) {
    crc = crc32c::Extend(crc, file.data() + start + kRecordHeaderSize, len - kRecordHeaderSize);
  }
  put_le(file, start + kOffCrc, crc, 4);
}

// A log image under construction. `starts` holds the offset of every record, so tests can
// say "the third record" and "where the third record begins".
struct TestLog {
  Bytes bytes;
  std::vector<uint64_t> starts;

  // Appends a valid DATA record with the given seq. The payload depends on `seed`, so
  // records differ from each other.
  void add_data(uint64_t seq, uint64_t block_no, uint8_t seed = 1) {
    std::array<uint8_t, kBlockSize> payload{};
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>(seed + i * 7 + i / 256);
    }
    std::array<uint8_t, kDataRecordSize> rec{};
    Status s = encode_data_record(seq, block_no, payload, rec);
    EXPECT_TRUE(s.ok()) << s.to_string();
    starts.push_back(bytes.size());
    bytes.insert(bytes.end(), rec.begin(), rec.end());
  }

  void add_trim(uint64_t seq, uint64_t block_no, uint32_t block_count) {
    std::array<uint8_t, kTrimRecordSize> rec{};
    Status s = encode_trim_record(seq, block_no, block_count, rec);
    EXPECT_TRUE(s.ok()) << s.to_string();
    starts.push_back(bytes.size());
    bytes.insert(bytes.end(), rec.begin(), rec.end());
  }

  [[nodiscard]] uint64_t size() const { return bytes.size(); }
  [[nodiscard]] uint64_t end_of(size_t index) const {
    return index + 1 < starts.size() ? starts[index + 1] : bytes.size();
  }
};

// DATA(1), TRIM(2), DATA(3), DATA(4): a small valid log with both record types.
TestLog four_records() {
  TestLog log;
  log.add_data(1, 10, 1);
  log.add_trim(2, 20, 5);
  log.add_data(3, 11, 2);
  log.add_data(4, 12, 3);
  return log;
}

// Scans and checks the parts every test cares about. `valid_end` is where the first invalid
// position starts (or the file size for a clean log). Returns the result for further checks.
ScanResult scan_and_check(const Bytes& file, ScanStop stop, size_t record_count,
                          uint64_t valid_end) {
  ScanResult result = scan_log(file);
  EXPECT_EQ(scan_stop_name(result.stop), std::string(scan_stop_name(stop))) << result.detail;
  EXPECT_EQ(result.records.size(), record_count);
  EXPECT_EQ(result.valid_end, valid_end);
  if (stop == ScanStop::Clean) {
    EXPECT_TRUE(result.detail.empty()) << result.detail;
  } else {
    EXPECT_FALSE(result.detail.empty()) << "a stop that is not Clean must say why";
  }
  return result;
}

// --- Clean logs ------------------------------------------------------------------

TEST(LogScanTest, AnEmptyFileScansCleanWithNoRecords) {
  scan_and_check(Bytes{}, ScanStop::Clean, 0, 0);
}

TEST(LogScanTest, ACleanLogScansCleanAndReturnsEveryRecordHeaderAndOffset) {
  const TestLog log = four_records();

  const ScanResult result = scan_and_check(log.bytes, ScanStop::Clean, 4, log.size());

  ASSERT_EQ(result.records.size(), 4U);
  const RecordType types[] = {RecordType::Data, RecordType::Trim, RecordType::Data,
                              RecordType::Data};
  const uint64_t blocks[] = {10, 20, 11, 12};
  const uint32_t counts[] = {1, 5, 1, 1};
  const uint32_t data_lens[] = {4096, 0, 4096, 4096};
  for (size_t i = 0; i < 4; ++i) {
    SCOPED_TRACE("record " + std::to_string(i));
    const ScannedRecord& rec = result.records[i];
    EXPECT_EQ(rec.offset, log.starts[i]);
    EXPECT_EQ(rec.header.type, types[i]);
    EXPECT_EQ(rec.header.seq, i + 1);
    EXPECT_EQ(rec.header.block_no, blocks[i]);
    EXPECT_EQ(rec.header.block_count, counts[i]);
    EXPECT_EQ(rec.header.data_len, data_lens[i]);
  }
  // Offsets match the sizes in the format: 4,136 for DATA, 40 for TRIM.
  EXPECT_EQ(log.starts[1], kDataRecordSize);
  EXPECT_EQ(log.starts[2], kDataRecordSize + kTrimRecordSize);
}

TEST(LogScanTest, ALogWithASingleRecordScansClean) {
  TestLog data_only;
  data_only.add_data(1, 0);
  scan_and_check(data_only.bytes, ScanStop::Clean, 1, data_only.size());

  TestLog trim_only;
  trim_only.add_trim(1, 0, 1);
  scan_and_check(trim_only.bytes, ScanStop::Clean, 1, trim_only.size());
}

TEST(LogScanTest, ALogCutAtARecordBoundaryScansCleanWithFewerRecords) {
  // What the kill test sees when the process died between two appends.
  const TestLog log = four_records();
  const Bytes prefix(log.bytes.begin(),
                     log.bytes.begin() + static_cast<std::ptrdiff_t>(log.starts[2]));

  scan_and_check(prefix, ScanStop::Clean, 2, log.starts[2]);
}

TEST(LogScanTest, TheLastBlockAndAWholeDeviceTrimAreInsideTheDevice) {
  // Rule 6 is inclusive at the end: block_no + block_count - 1 == kBlockCount - 1 is fine.
  TestLog log;
  log.add_data(1, kBlockCount - 1);
  log.add_trim(2, kBlockCount - 1, 1);
  log.add_trim(3, 0, static_cast<uint32_t>(kBlockCount));

  scan_and_check(log.bytes, ScanStop::Clean, 3, log.size());
}

// --- Rule 9: seq -----------------------------------------------------------------

TEST(LogScanTest, AFirstSeqOtherThanOneIsCorruption) {
  for (uint64_t first : {uint64_t{0}, uint64_t{2}, uint64_t{1000}}) {
    SCOPED_TRACE("first seq " + std::to_string(first));
    TestLog log;
    log.add_data(first, 0);
    log.add_data(first + 1, 1);

    scan_and_check(log.bytes, ScanStop::Corrupt, 0, 0);
  }
}

TEST(LogScanTest, ASeqGapInTheMiddleIsCorruptionAndLaterRecordsAreNotReturned) {
  TestLog log;
  log.add_data(1, 0);
  log.add_data(2, 1);
  log.add_data(4, 2);  // 3 is missing
  log.add_data(5, 3);  // valid after the gap, and still not returned

  scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
}

TEST(LogScanTest, ASeqGapOnTheLastRecordIsCorruptionNotATornWrite) {
  // The record is complete and its CRC is fine, so nothing is "unfinished".
  TestLog log;
  log.add_data(1, 0);
  log.add_trim(2, 5, 1);
  log.add_trim(4, 6, 1);

  scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
}

TEST(LogScanTest, ARepeatedOrBackwardsSeqIsCorruption) {
  {
    SCOPED_TRACE("seq repeats: 1, 2, 2");
    TestLog log;
    log.add_data(1, 0);
    log.add_data(2, 1);
    log.add_data(2, 2);
    scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
  }
  {
    SCOPED_TRACE("seq goes back: 1, 2, 3, 2");
    TestLog log;
    log.add_data(1, 0);
    log.add_data(2, 1);
    log.add_data(3, 2);
    log.add_data(2, 3);
    scan_and_check(log.bytes, ScanStop::Corrupt, 3, log.starts[3]);
  }
}

// --- Torn writes -----------------------------------------------------------------

TEST(LogScanTest, FewerThan40BytesAtTheEndIsATornWrite) {
  const TestLog log = four_records();
  // The first n bytes of what would have been the fifth record.
  TestLog next;
  next.add_data(5, 13, 4);
  for (size_t n : {size_t{1}, size_t{10}, size_t{39}}) {
    SCOPED_TRACE(std::to_string(n) + " stray bytes");
    Bytes file = log.bytes;
    file.insert(file.end(), next.bytes.begin(),
                next.bytes.begin() + static_cast<std::ptrdiff_t>(n));

    scan_and_check(file, ScanStop::TornWrite, 4, log.size());
  }
}

TEST(LogScanTest, AFileShorterThanOneHeaderIsATornWriteWithNoRecords) {
  TestLog log;
  log.add_data(1, 0);
  const Bytes file(log.bytes.begin(), log.bytes.begin() + 20);

  scan_and_check(file, ScanStop::TornWrite, 0, 0);
}

TEST(LogScanTest, FewerThan40BytesAreATornWriteWhateverTheyContain) {
  // Too short to hold a header, so there is nothing to validate: 39 zero bytes are torn,
  // while 40 zero bytes are corruption (see below).
  const TestLog log = four_records();
  Bytes file = log.bytes;
  file.insert(file.end(), 39, uint8_t{0});

  scan_and_check(file, ScanStop::TornWrite, 4, log.size());
}

TEST(LogScanTest, AValidHeaderWhoseRecordRunsPastTheEndIsATornWrite) {
  const TestLog log = four_records();
  TestLog next;
  next.add_data(5, 13, 4);  // 4,136 bytes; the file will hold only part of it
  // 40: the header and no payload. 41: one payload byte. The last: one byte short.
  for (size_t n :
       {size_t{40}, size_t{41}, size_t{2000}, static_cast<size_t>(kDataRecordSize - 1)}) {
    SCOPED_TRACE("record cut after " + std::to_string(n) + " bytes");
    Bytes file = log.bytes;
    file.insert(file.end(), next.bytes.begin(),
                next.bytes.begin() + static_cast<std::ptrdiff_t>(n));

    scan_and_check(file, ScanStop::TornWrite, 4, log.size());
  }
}

TEST(LogScanTest, ATrimRecordCutShortIsATornWrite) {
  TestLog log;
  log.add_data(1, 0);
  log.add_trim(2, 5, 3);
  const Bytes file(log.bytes.begin(), log.bytes.end() - 1);  // 39 of the TRIM's 40 bytes

  scan_and_check(file, ScanStop::TornWrite, 1, log.starts[1]);
}

TEST(LogScanTest, ATornWriteAtTheStartOfTheLogIsATornWriteWithNoRecords) {
  TestLog log;
  log.add_data(1, 0);
  const Bytes file(log.bytes.begin(), log.bytes.begin() + 500);

  scan_and_check(file, ScanStop::TornWrite, 0, 0);
}

// --- Corruption ------------------------------------------------------------------

TEST(LogScanTest, ACompleteLastRecordThatFailsItsCrcIsCorruption) {
  // Flip one byte at a time: in the payload, in the stored CRC, and in a header field
  // that is still valid on its own (block_no 12 -> 13), so only the CRC can catch it.
  struct Case {
    const char* name;
    size_t offset_in_record;
  };
  const Case cases[] = {{"payload byte", kRecordHeaderSize + 100},
                        {"last payload byte", kDataRecordSize - 1},
                        {"crc field", kOffCrc},
                        {"block_no", kOffBlockNo}};
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TestLog log = four_records();
    log.bytes[log.starts[3] + c.offset_in_record] ^= 0x01;

    scan_and_check(log.bytes, ScanStop::Corrupt, 3, log.starts[3]);
  }
}

TEST(LogScanTest, ACompleteLastTrimRecordThatFailsItsCrcIsCorruption) {
  TestLog log;
  log.add_data(1, 0);
  log.add_trim(2, 5, 3);
  log.bytes[log.starts[1] + kOffBlockCount] ^= 0x01;  // 3 -> 2 blocks, a valid range

  scan_and_check(log.bytes, ScanStop::Corrupt, 1, log.starts[1]);
}

TEST(LogScanTest, AnInvalidRecordInTheMiddleIsCorruptionAndLaterValidRecordsAreNotReturned) {
  TestLog log = four_records();
  log.bytes[log.starts[1] + kOffBlockCount] ^= 0x01;  // damage the TRIM, records 3 and 4 are fine

  scan_and_check(log.bytes, ScanStop::Corrupt, 1, log.starts[1]);
}

TEST(LogScanTest, ABadPayloadInTheMiddleIsCorruption) {
  TestLog log = four_records();
  log.bytes[log.starts[2] + kRecordHeaderSize + 7] ^= 0x80;

  scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
}

TEST(LogScanTest, EachHeaderRuleBrokenInTheMiddleIsCorruption) {
  // One broken rule per case, with the CRC recomputed so only that rule can fail
  // (rules 1 to 6; rule 8 is the CRC tests above, rule 9 the seq tests). The record is
  // the DATA record at index 2 of four_records(); case 7 is TRIM-only.
  struct Case {
    const char* name;
    std::function<void(Bytes&, size_t)> change;  // (file, start of record)
  };
  const std::vector<Case> cases = {
      {"rule 1: magic", [](Bytes& f, size_t s) { f[s + kOffMagic] = 'X'; }},
      {"rule 2: version 2", [](Bytes& f, size_t s) { put_le(f, s + kOffVersion, 2, 2); }},
      {"rule 2: version 0", [](Bytes& f, size_t s) { put_le(f, s + kOffVersion, 0, 2); }},
      {"rule 3: type 0", [](Bytes& f, size_t s) { f[s + kOffType] = 0; }},
      {"rule 3: type 3", [](Bytes& f, size_t s) { f[s + kOffType] = 3; }},
      {"rule 4: flags", [](Bytes& f, size_t s) { f[s + kOffFlags] = 1; }},
      {"rule 4: reserved", [](Bytes& f, size_t s) { put_le(f, s + kOffReserved, 1, 4); }},
      {"rule 5: DATA block_count 2",
       [](Bytes& f, size_t s) { put_le(f, s + kOffBlockCount, 2, 4); }},
      {"rule 5: DATA data_len 0", [](Bytes& f, size_t s) { put_le(f, s + kOffDataLen, 0, 4); }},
      {"rule 6: block_no past the device",
       [](Bytes& f, size_t s) { put_le(f, s + kOffBlockNo, kBlockCount, 8); }},
      {"rule 6: block_no huge",
       [](Bytes& f, size_t s) {
         put_le(f, s + kOffBlockNo, std::numeric_limits<uint64_t>::max(), 8);
       }},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TestLog log = four_records();
    c.change(log.bytes, log.starts[2]);
    reseal(log.bytes, log.starts[2], kDataRecordSize);

    scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
  }
}

TEST(LogScanTest, ATrimRecordBreakingATrimRuleIsCorruption) {
  struct Case {
    const char* name;
    std::function<void(Bytes&, size_t)> change;
  };
  const std::vector<Case> cases = {
      {"rule 5: TRIM block_count 0",
       [](Bytes& f, size_t s) { put_le(f, s + kOffBlockCount, 0, 4); }},
      {"rule 6: range runs past the device",
       [](Bytes& f, size_t s) { put_le(f, s + kOffBlockNo, kBlockCount - 2, 8); }},  // count is 5
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TestLog log = four_records();
    c.change(log.bytes, log.starts[1]);
    reseal(log.bytes, log.starts[1], kTrimRecordSize);

    scan_and_check(log.bytes, ScanStop::Corrupt, 1, log.starts[1]);
  }
}

TEST(LogScanTest, AnInvalidHeaderOnTheLastRecordIsCorruptionEvenWhenTheRecordIsCutShort) {
  // A torn write needs a header that passes rules 1 to 6. A broken header cannot be
  // blamed on a crash during the append, so a short file does not turn it into one.
  TestLog log = four_records();
  const size_t last = log.starts[3];
  put_le(log.bytes, last + kOffBlockCount, 2, 4);  // rule 5 broken
  reseal(log.bytes, last, kDataRecordSize);

  Bytes cut_in_payload(log.bytes.begin(),
                       log.bytes.begin() + static_cast<std::ptrdiff_t>(last + 1000));
  scan_and_check(cut_in_payload, ScanStop::Corrupt, 3, last);

  Bytes header_only(log.bytes.begin(),
                    log.bytes.begin() + static_cast<std::ptrdiff_t>(last + kRecordHeaderSize));
  scan_and_check(header_only, ScanStop::Corrupt, 3, last);
}

TEST(LogScanTest, AWrongMagicOnAFullHeaderAtTheEndIsCorruption) {
  // Exactly 40 bytes that are not a header: not "fewer than 40 bytes", so not torn.
  const TestLog log = four_records();
  Bytes file = log.bytes;
  file.insert(file.end(), kRecordHeaderSize, uint8_t{0xEE});

  scan_and_check(file, ScanStop::Corrupt, 4, log.size());
}

// --- Zeroed space ------------------------------------------------------------------

TEST(LogScanTest, AZeroedRegionOf40OrMoreBytesAtTheEndIsCorruption) {
  // Zeros can never parse as a record (type 0 is invalid), so preallocated or never-written
  // space is reported, not skipped.
  const TestLog log = four_records();
  for (size_t zeros : {size_t{40}, size_t{41}, size_t{4096}, size_t{100000}}) {
    SCOPED_TRACE(std::to_string(zeros) + " zero bytes");
    Bytes file = log.bytes;
    file.insert(file.end(), zeros, uint8_t{0});

    scan_and_check(file, ScanStop::Corrupt, 4, log.size());
  }
}

TEST(LogScanTest, AZeroedRegionInTheMiddleIsCorruptionAndLaterRecordsAreNotReturned) {
  TestLog log;
  log.add_data(1, 0);
  log.add_data(2, 1);
  const uint64_t hole = log.size();
  log.bytes.insert(log.bytes.end(), 100, uint8_t{0});
  log.add_data(3, 2);  // a valid record after the hole

  scan_and_check(log.bytes, ScanStop::Corrupt, 2, hole);
}

TEST(LogScanTest, ARecordOverwrittenWithZerosIsCorruption) {
  TestLog log = four_records();
  std::fill(log.bytes.begin() + static_cast<std::ptrdiff_t>(log.starts[2]),
            log.bytes.begin() + static_cast<std::ptrdiff_t>(log.end_of(2)), uint8_t{0});

  scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
}

TEST(LogScanTest, AFileThatIsAllZerosIsCorruptionWithNoRecords) {
  scan_and_check(Bytes(4096, 0), ScanStop::Corrupt, 0, 0);
}

}  // namespace
}  // namespace logblock
