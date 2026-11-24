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
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "logblock/engine.h"
#include "record.h"
#include "support/fault_io.h"
#include "support/io_helpers.h"

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
  // Too short to hold a header, so there is nothing to validate, zero or not.
  const TestLog log = four_records();
  for (uint8_t fill : {uint8_t{0}, uint8_t{0xEE}}) {
    SCOPED_TRACE("fill byte " + std::to_string(fill));
    Bytes file = log.bytes;
    file.insert(file.end(), 39, fill);

    scan_and_check(file, ScanStop::TornWrite, 4, log.size());
  }
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

TEST(LogScanTest, ZerosFromARecordBoundaryToTheEndAreATornWrite) {
  // After a power cut the file's new size can be durable while its data never reached the
  // disk, which leaves zeros where the unsynced records would have been (D19).
  const TestLog log = four_records();
  for (size_t zeros : {size_t{40}, size_t{41}, size_t{4096}, size_t{100000}}) {
    SCOPED_TRACE(std::to_string(zeros) + " zero bytes");
    Bytes file = log.bytes;
    file.insert(file.end(), zeros, uint8_t{0});

    scan_and_check(file, ScanStop::TornWrite, 4, log.size());
  }
}

TEST(LogScanTest, ALastRecordOverwrittenWithZerosIsATornWrite) {
  // The scanner cannot tell this from a record that never reached the disk. If that record
  // was flushed, only a checkpoint covering it can catch the loss (docs/formats.md).
  TestLog log = four_records();
  std::fill(log.bytes.begin() + static_cast<std::ptrdiff_t>(log.starts[3]), log.bytes.end(),
            uint8_t{0});

  scan_and_check(log.bytes, ScanStop::TornWrite, 3, log.starts[3]);
}

TEST(LogScanTest, AFileThatIsAllZerosIsATornWriteWithNoRecords) {
  scan_and_check(Bytes(4096, 0), ScanStop::TornWrite, 0, 0);
}

TEST(LogScanTest, ANonZeroByteInAZeroTailIsCorruption) {
  // The tail must be zeros all the way to the end, so check the first, a middle and the
  // last byte of it.
  const TestLog log = four_records();
  for (size_t position : {size_t{0}, size_t{2048}, size_t{4095}}) {
    SCOPED_TRACE("non-zero byte at tail position " + std::to_string(position));
    Bytes file = log.bytes;
    file.insert(file.end(), 4096, uint8_t{0});
    file[log.size() + position] = 1;

    scan_and_check(file, ScanStop::Corrupt, 4, log.size());
  }
}

TEST(LogScanTest, ZerosThatDoNotStartAtARecordBoundaryAreCorruption) {
  // 20 bytes of a fifth record, then zeros: the invalid position is not all zeros, and the
  // bytes there do not form a valid header.
  const TestLog log = four_records();
  TestLog next;
  next.add_data(5, 13, 4);
  Bytes file = log.bytes;
  file.insert(file.end(), next.bytes.begin(), next.bytes.begin() + 20);
  file.insert(file.end(), 100, uint8_t{0});

  scan_and_check(file, ScanStop::Corrupt, 4, log.size());
}

TEST(LogScanTest, ACompleteFinalRecordWhosePayloadEndsInZerosIsCorruption) {
  // A record cut across a page boundary after a power cut looks like this. It is complete
  // and non-zero with a bad CRC, so the rule stays conservative (D19).
  TestLog log = four_records();
  std::fill(log.bytes.end() - 2048, log.bytes.end(), uint8_t{0});

  scan_and_check(log.bytes, ScanStop::Corrupt, 3, log.starts[3]);
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

TEST(LogScanTest, ARecordOverwrittenWithZerosBeforeTheLastRecordIsCorruption) {
  TestLog log = four_records();
  std::fill(log.bytes.begin() + static_cast<std::ptrdiff_t>(log.starts[2]),
            log.bytes.begin() + static_cast<std::ptrdiff_t>(log.end_of(2)), uint8_t{0});

  scan_and_check(log.bytes, ScanStop::Corrupt, 2, log.starts[2]);
}


// --- Streaming scan ------------------------------------------------------------------
// scan_log_stream reads through Io in chunks. It shares one classifier with scan_log, so the
// tests below check that the two agree, then check what only the streaming form does: a start
// offset, chunk boundaries, and Io failures. Chunk sizes smaller than a record force every kind
// of boundary (inside a header, inside a payload, between records, inside a zero tail).

using test::FaultIo;
using Op = FaultIo::Op;

// What one streaming scan returned, and the records it visited.
struct StreamRun {
  Status status;
  StreamScanResult result;
  std::vector<ScannedRecord> visited;
};

// Puts `file` into a fresh, opened FaultIo.
std::unique_ptr<FaultIo> io_with(const Bytes& file) {
  auto io = std::make_unique<FaultIo>();
  EXPECT_TRUE(test::open_or_create_log(*io, "log").ok());
  io->bytes().assign(file.begin(), file.end());
  return io;
}

StreamRun stream_scan(FaultIo& io, uint64_t start = 0, uint64_t seq = 1,
                      size_t chunk = kScanChunkSize) {
  StreamRun run;
  run.status = scan_log_stream(
      io, start, seq, [&](const ScannedRecord& rec) { run.visited.push_back(rec); }, &run.result,
      chunk);
  return run;
}

StreamRun stream_scan(const Bytes& file, uint64_t start = 0, uint64_t seq = 1,
                      size_t chunk = kScanChunkSize) {
  auto io = io_with(file);
  return stream_scan(*io, start, seq, chunk);
}

// Checks that the streaming scan classifies `file` exactly as scan_log does.
void expect_same_as_scan_log(const Bytes& file, size_t chunk) {
  SCOPED_TRACE("chunk size " + std::to_string(chunk));
  const ScanResult want = scan_log(file);
  const StreamRun got = stream_scan(file, 0, 1, chunk);

  ASSERT_TRUE(got.status.ok()) << got.status.to_string();
  EXPECT_EQ(scan_stop_name(got.result.stop), std::string(scan_stop_name(want.stop)));
  EXPECT_EQ(got.result.valid_end, want.valid_end);
  EXPECT_EQ(got.result.detail, want.detail);
  EXPECT_EQ(got.result.records, want.records.size());
  EXPECT_EQ(got.result.next_seq, want.records.size() + 1);
  ASSERT_EQ(got.visited.size(), want.records.size());
  for (size_t i = 0; i < want.records.size(); ++i) {
    EXPECT_EQ(got.visited[i].offset, want.records[i].offset);
    EXPECT_EQ(got.visited[i].header.seq, want.records[i].header.seq);
    EXPECT_EQ(got.visited[i].header.type, want.records[i].header.type);
    EXPECT_EQ(got.visited[i].header.block_no, want.records[i].header.block_no);
    EXPECT_EQ(got.visited[i].header.block_count, want.records[i].header.block_count);
    EXPECT_EQ(got.visited[i].header.data_len, want.records[i].header.data_len);
  }
}

// One image per class of outcome the scan_log tests above cover.
std::vector<std::pair<std::string, Bytes>> equivalence_cases() {
  std::vector<std::pair<std::string, Bytes>> cases;
  const TestLog log = four_records();
  TestLog next;
  next.add_data(5, 13, 4);

  cases.emplace_back("empty", Bytes{});
  cases.emplace_back("clean four records", log.bytes);
  cases.emplace_back("cut at a boundary",
                     Bytes(log.bytes.begin(),
                           log.bytes.begin() + static_cast<std::ptrdiff_t>(log.starts[2])));
  for (size_t n : {size_t{1}, size_t{39}, size_t{40}, size_t{41}, size_t{2000},
                   static_cast<size_t>(kDataRecordSize - 1)}) {
    Bytes file = log.bytes;
    file.insert(file.end(), next.bytes.begin(), next.bytes.begin() + static_cast<std::ptrdiff_t>(n));
    cases.emplace_back("fifth record cut after " + std::to_string(n), file);
  }
  cases.emplace_back("shorter than a header", Bytes(log.bytes.begin(), log.bytes.begin() + 20));
  {
    Bytes file = log.bytes;
    file.insert(file.end(), 39, uint8_t{0xEE});
    cases.emplace_back("39 stray bytes", file);
  }
  {
    Bytes file = log.bytes;
    file.insert(file.end(), kRecordHeaderSize, uint8_t{0xEE});
    cases.emplace_back("40 bytes of 0xEE", file);
  }
  {
    TestLog gap;
    gap.add_data(1, 0);
    gap.add_data(2, 1);
    gap.add_data(4, 2);
    gap.add_data(5, 3);
    cases.emplace_back("seq gap", gap.bytes);
  }
  {
    TestLog first;
    first.add_data(2, 0);
    cases.emplace_back("first seq 2", first.bytes);
  }
  for (size_t offset : {kRecordHeaderSize + 100, kDataRecordSize - 1, kOffCrc, kOffBlockNo}) {
    TestLog bad = four_records();
    bad.bytes[bad.starts[3] + offset] ^= 0x01;
    cases.emplace_back("last record flipped at " + std::to_string(offset), bad.bytes);
  }
  {
    TestLog bad = four_records();
    bad.bytes[bad.starts[1] + kOffBlockCount] ^= 0x01;
    cases.emplace_back("bad TRIM in the middle", bad.bytes);
  }
  {
    TestLog bad = four_records();
    put_le(bad.bytes, bad.starts[2] + kOffBlockCount, 2, 4);
    reseal(bad.bytes, bad.starts[2], kDataRecordSize);
    cases.emplace_back("rule 5 broken", bad.bytes);
    Bytes cut(bad.bytes.begin(),
              bad.bytes.begin() + static_cast<std::ptrdiff_t>(bad.starts[2] + 1000));
    cases.emplace_back("rule 5 broken and cut short", cut);
  }
  for (size_t zeros : {size_t{39}, size_t{40}, size_t{41}, size_t{4096}, size_t{100000}}) {
    Bytes file = log.bytes;
    file.insert(file.end(), zeros, uint8_t{0});
    cases.emplace_back(std::to_string(zeros) + " zero bytes at the end", file);
  }
  for (size_t position : {size_t{0}, size_t{2048}, size_t{4095}}) {
    Bytes file = log.bytes;
    file.insert(file.end(), 4096, uint8_t{0});
    file[log.size() + position] = 1;
    cases.emplace_back("zero tail with a 1 at " + std::to_string(position), file);
  }
  {
    TestLog zeroed = four_records();
    std::fill(zeroed.bytes.begin() + static_cast<std::ptrdiff_t>(zeroed.starts[3]),
              zeroed.bytes.end(), uint8_t{0});
    cases.emplace_back("last record zeroed", zeroed.bytes);
  }
  {
    TestLog zeroed = four_records();
    std::fill(zeroed.bytes.end() - 2048, zeroed.bytes.end(), uint8_t{0});
    cases.emplace_back("last payload ends in zeros", zeroed.bytes);
  }
  cases.emplace_back("all zeros", Bytes(4096, 0));
  {
    TestLog hole;
    hole.add_data(1, 0);
    hole.add_data(2, 1);
    hole.bytes.insert(hole.bytes.end(), 100, uint8_t{0});
    hole.add_data(3, 2);
    cases.emplace_back("zero hole in the middle", hole.bytes);
  }
  {
    Bytes file = log.bytes;
    file.insert(file.end(), next.bytes.begin(), next.bytes.begin() + 20);
    file.insert(file.end(), 100, uint8_t{0});
    cases.emplace_back("20 bytes of a record then zeros", file);
  }
  return cases;
}

TEST(LogScanStreamTest, ClassifiesEveryScanLogCaseTheSameAtManyChunkSizes) {
  // 1 and 7: inside a header. 40 and 41: header-sized. 4136 and 4137: record-sized. A huge
  // chunk: the whole file in one read.
  for (const auto& [name, file] : equivalence_cases()) {
    SCOPED_TRACE(name);
    for (size_t chunk : {size_t{1}, size_t{7}, size_t{40}, size_t{41}, size_t{4136}, size_t{4137},
                         kScanChunkSize}) {
      expect_same_as_scan_log(file, chunk);
    }
  }
}

TEST(LogScanStreamTest, ACleanLogReportsRecordsNextSeqAndValidEnd) {
  const TestLog log = four_records();

  const StreamRun run = stream_scan(log.bytes);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Clean);
  EXPECT_EQ(run.result.valid_end, log.size());
  EXPECT_EQ(run.result.records, 4U);
  EXPECT_EQ(run.result.next_seq, 5U);
  EXPECT_TRUE(run.result.detail.empty());
}

TEST(LogScanStreamTest, AnEmptyFileIsCleanAndNextSeqIsTheExpectedSeq) {
  const StreamRun run = stream_scan(Bytes{}, 0, 7);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Clean);
  EXPECT_EQ(run.result.records, 0U);
  EXPECT_EQ(run.result.next_seq, 7U);
  EXPECT_EQ(run.result.valid_end, 0U);
}

TEST(LogScanStreamTest, EveryChunkSizeFromOneByteUpSeesRecordsThatCrossBoundaries) {
  // Chunks 1 to 130 put a boundary at every offset in each header; the larger ones put one
  // just inside and just outside each payload.
  const TestLog log = four_records();
  std::vector<size_t> chunks;
  for (size_t c = 1; c <= 130; ++c) {
    chunks.push_back(c);
  }
  for (size_t c : {size_t{4095}, size_t{4096}, size_t{4097}, size_t{4135}, size_t{4175},
                   size_t{4176}, size_t{4177}, size_t{8272}, size_t{12447}, size_t{12448},
                   size_t{12449}}) {
    chunks.push_back(c);
  }
  for (size_t chunk : chunks) {
    expect_same_as_scan_log(log.bytes, chunk);
  }
}

TEST(LogScanStreamTest, AChunkSizeOfZeroIsTreatedAsOneByte) {
  // Guards against a loop that never makes progress.
  const TestLog log = four_records();
  const StreamRun run = stream_scan(log.bytes, 0, 1, 0);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Clean);
  EXPECT_EQ(run.result.records, 4U);
}

// --- Start offset and expected seq ----------------------------------------------------

TEST(LogScanStreamTest, ScansFromARecordBoundaryWithTheMatchingSeq) {
  const TestLog log = four_records();
  for (size_t chunk : {size_t{7}, size_t{4136}, kScanChunkSize}) {
    SCOPED_TRACE("chunk " + std::to_string(chunk));
    const StreamRun run = stream_scan(log.bytes, log.starts[2], 3, chunk);

    ASSERT_TRUE(run.status.ok());
    EXPECT_EQ(run.result.stop, ScanStop::Clean);
    EXPECT_EQ(run.result.valid_end, log.size());
    EXPECT_EQ(run.result.records, 2U);
    EXPECT_EQ(run.result.next_seq, 5U);
    ASSERT_EQ(run.visited.size(), 2U);
    EXPECT_EQ(run.visited[0].offset, log.starts[2]);  // absolute, not relative to the start
    EXPECT_EQ(run.visited[0].header.seq, 3U);
    EXPECT_EQ(run.visited[1].offset, log.starts[3]);
    EXPECT_EQ(run.visited[1].header.seq, 4U);
  }
}

TEST(LogScanStreamTest, AWrongFirstSeqAtAStartOffsetIsCorruptAndDetailNamesTheAbsoluteOffset) {
  const TestLog log = four_records();
  for (uint64_t seq : {uint64_t{1}, uint64_t{2}, uint64_t{4}}) {
    SCOPED_TRACE("expected seq " + std::to_string(seq));
    const StreamRun run = stream_scan(log.bytes, log.starts[2], seq);

    ASSERT_TRUE(run.status.ok());
    EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
    EXPECT_EQ(run.result.valid_end, log.starts[2]);
    EXPECT_EQ(run.result.records, 0U);
    EXPECT_EQ(run.result.next_seq, seq);  // no record was valid
    EXPECT_TRUE(run.visited.empty());
    EXPECT_NE(run.result.detail.find("offset " + std::to_string(log.starts[2])), std::string::npos)
        << run.result.detail;
  }
}

TEST(LogScanStreamTest, AStartOffsetAtTheEndOfTheFileIsCleanWithNoRecords) {
  const TestLog log = four_records();

  const StreamRun run = stream_scan(log.bytes, log.size(), 5);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Clean);
  EXPECT_EQ(run.result.valid_end, log.size());
  EXPECT_EQ(run.result.records, 0U);
  EXPECT_EQ(run.result.next_seq, 5U);
}

TEST(LogScanStreamTest, AStartOffsetPastTheEndOfTheFileIsCorrupt) {
  // A checkpoint that points past the log means the log lost data. That is damage, so it is
  // not a torn write.
  const TestLog log = four_records();

  const StreamRun run = stream_scan(log.bytes, log.size() + 1, 5);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
  EXPECT_EQ(run.result.valid_end, log.size() + 1);
  EXPECT_EQ(run.result.records, 0U);
  EXPECT_EQ(run.result.next_seq, 5U);
  EXPECT_FALSE(run.result.detail.empty());
}

TEST(LogScanStreamTest, AStartOffsetInTheMiddleOfARecordIsCorrupt) {
  const TestLog log = four_records();

  const StreamRun run = stream_scan(log.bytes, log.starts[1] + 3, 2);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
  EXPECT_EQ(run.result.valid_end, log.starts[1] + 3);
}

TEST(LogScanStreamTest, TornAndCorruptTailsAfterAStartOffsetKeepAbsoluteOffsets) {
  const TestLog log = four_records();
  {
    SCOPED_TRACE("torn: last record cut short");
    const Bytes file(log.bytes.begin(), log.bytes.end() - 1);
    const StreamRun run = stream_scan(file, log.starts[1], 2, 11);

    ASSERT_TRUE(run.status.ok());
    EXPECT_EQ(run.result.stop, ScanStop::TornWrite);
    EXPECT_EQ(run.result.valid_end, log.starts[3]);
    EXPECT_EQ(run.result.records, 2U);
    EXPECT_EQ(run.result.next_seq, 4U);
  }
  {
    SCOPED_TRACE("corrupt: bad CRC in the third record");
    TestLog bad = four_records();
    bad.bytes[bad.starts[2] + kRecordHeaderSize + 5] ^= 0x01;
    const StreamRun run = stream_scan(bad.bytes, log.starts[1], 2, 11);

    ASSERT_TRUE(run.status.ok());
    EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
    EXPECT_EQ(run.result.valid_end, log.starts[2]);
    EXPECT_EQ(run.result.records, 1U);
    EXPECT_EQ(run.result.next_seq, 3U);
  }
}

// --- Zero tails across chunk boundaries -------------------------------------------------

TEST(LogScanStreamTest, ATornZeroTailIsFoundWhereverTheChunkBoundaryFalls) {
  // The log is 12,448 bytes. Chunk sizes around that put the first boundary at every position
  // from 50 bytes before the end of the last record to 110 bytes into the tail.
  const TestLog log = four_records();
  Bytes file = log.bytes;
  file.insert(file.end(), 100, uint8_t{0});
  for (size_t chunk = log.size() - 50; chunk <= log.size() + 110; ++chunk) {
    SCOPED_TRACE("chunk " + std::to_string(chunk));
    const StreamRun run = stream_scan(file, 0, 1, chunk);

    ASSERT_TRUE(run.status.ok());
    EXPECT_EQ(run.result.stop, ScanStop::TornWrite);
    EXPECT_EQ(run.result.valid_end, log.size());
    EXPECT_EQ(run.result.records, 4U);
  }
}

TEST(LogScanStreamTest, ANonZeroByteInAZeroTailIsCorruptWhereverTheChunkBoundaryFalls) {
  const TestLog log = four_records();
  for (size_t position : {size_t{0}, size_t{1}, size_t{39}, size_t{40}, size_t{99}}) {
    Bytes file = log.bytes;
    file.insert(file.end(), 100, uint8_t{0});
    file[log.size() + position] = 1;
    for (size_t chunk = log.size() - 50; chunk <= log.size() + 110; ++chunk) {
      SCOPED_TRACE("non-zero at tail position " + std::to_string(position) + ", chunk " +
                   std::to_string(chunk));
      const StreamRun run = stream_scan(file, 0, 1, chunk);

      ASSERT_TRUE(run.status.ok());
      EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
      EXPECT_EQ(run.result.valid_end, log.size());
      EXPECT_EQ(run.result.records, 4U);
    }
  }
}

TEST(LogScanStreamTest, AZeroTailManyChunksLongIsTornAndAStrayByteAnywhereInItIsCorrupt) {
  // 60,000 zero bytes at chunk 4,096 is about 15 chunks. The stray byte goes just before, on
  // and just after chunk boundaries, and in the very last byte of the file.
  const TestLog log = four_records();
  constexpr size_t kChunk = 4096;
  constexpr size_t kTail = 60000;
  Bytes zeros = log.bytes;
  zeros.insert(zeros.end(), kTail, uint8_t{0});

  const StreamRun torn = stream_scan(zeros, 0, 1, kChunk);
  ASSERT_TRUE(torn.status.ok());
  EXPECT_EQ(torn.result.stop, ScanStop::TornWrite);
  EXPECT_EQ(torn.result.valid_end, log.size());

  for (size_t position :
       {kChunk * 2 - 1, kChunk * 2, kChunk * 2 + 1, kChunk * 10, kChunk * 14 + 3, kTail - 1}) {
    SCOPED_TRACE("stray byte at tail position " + std::to_string(position));
    Bytes file = zeros;
    file[log.size() + position] = 0x80;
    const StreamRun run = stream_scan(file, 0, 1, kChunk);

    ASSERT_TRUE(run.status.ok());
    EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
    EXPECT_EQ(run.result.valid_end, log.size());
  }
}

// --- Memory and read pattern ----------------------------------------------------------

TEST(LogScanStreamTest, ReadsEveryByteOnceInRequestsNoLargerThanTheChunk) {
  // No re-reads, and no read bigger than the chunk: the window cannot grow with the file.
  const TestLog log = four_records();
  for (size_t chunk : {size_t{1}, size_t{100}, size_t{1000}, size_t{4136}, size_t{5000}}) {
    SCOPED_TRACE("chunk " + std::to_string(chunk));
    auto io = io_with(log.bytes);
    const StreamRun run = stream_scan(*io, 0, 1, chunk);

    ASSERT_TRUE(run.status.ok());
    uint64_t total = 0;
    for (const FaultIo::Call& call : io->calls()) {
      if (call.op == Op::Pread) {
        EXPECT_LE(call.length, chunk);
        total += call.length;
      }
    }
    EXPECT_EQ(total, log.size());
  }
}

TEST(LogScanStreamTest, ATailOfZerosIsReadInChunksNotAllAtOnce) {
  const TestLog log = four_records();
  Bytes file = log.bytes;
  file.insert(file.end(), 100000, uint8_t{0});
  auto io = io_with(file);

  const StreamRun run = stream_scan(*io, 0, 1, 4096);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::TornWrite);
  uint64_t total = 0;
  for (const FaultIo::Call& call : io->calls()) {
    if (call.op == Op::Pread) {
      EXPECT_LE(call.length, 4096U);
      total += call.length;
    }
  }
  EXPECT_EQ(total, file.size());
}

TEST(LogScanStreamTest, ManyRecordsAreVisitedOneAtATimeWithoutBeingCollected) {
  // The visitor sees each record as it is found. The result has counters, not a vector.
  TestLog log;
  constexpr uint64_t kCount = 5000;
  for (uint64_t seq = 1; seq <= kCount; ++seq) {
    log.add_trim(seq, seq % kBlockCount, 1);
  }
  auto io = io_with(log.bytes);
  uint64_t seen = 0;
  uint64_t last_seq = 0;
  StreamScanResult result;

  const Status status = scan_log_stream(
      *io, 0, 1,
      [&](const ScannedRecord& rec) {
        ++seen;
        last_seq = rec.header.seq;
      },
      &result, 4096);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(result.stop, ScanStop::Clean);
  EXPECT_EQ(result.records, kCount);
  EXPECT_EQ(seen, kCount);
  EXPECT_EQ(last_seq, kCount);
  EXPECT_EQ(result.next_seq, kCount + 1);
  EXPECT_EQ(result.valid_end, log.size());
}

TEST(LogScanStreamTest, TheVisitorSeesARecordOnlyAfterItIsFullyValidated) {
  // A record with a bad CRC, or a seq out of order, is never passed to the visitor.
  TestLog log = four_records();
  log.bytes[log.starts[3] + kRecordHeaderSize + 9] ^= 0x01;

  const StreamRun run = stream_scan(log.bytes, 0, 1, 50);

  ASSERT_TRUE(run.status.ok());
  EXPECT_EQ(run.result.stop, ScanStop::Corrupt);
  ASSERT_EQ(run.visited.size(), 3U);
  EXPECT_EQ(run.visited.back().header.seq, 3U);
}

// --- Io failures -----------------------------------------------------------------------

TEST(LogScanStreamTest, AFailedFileSizeReturnsTheIoStatusNotAClassification) {
  const TestLog log = four_records();
  auto io = io_with(log.bytes);
  io->fail_next(Op::FileSize, StatusCode::IoError);

  const StreamRun run = stream_scan(*io);

  EXPECT_EQ(run.status.code(), StatusCode::IoError);
  EXPECT_NE(run.status.message().find("file_size"), std::string::npos) << run.status.to_string();
  EXPECT_TRUE(run.visited.empty());
}

TEST(LogScanStreamTest, AFailedFirstPreadReturnsTheIoStatusNotAClassification) {
  const TestLog log = four_records();
  auto io = io_with(log.bytes);
  io->fail_next(Op::Pread, StatusCode::IoError);

  const StreamRun run = stream_scan(*io);

  EXPECT_EQ(run.status.code(), StatusCode::IoError);
  EXPECT_NE(run.status.message().find("pread"), std::string::npos) << run.status.to_string();
  EXPECT_TRUE(run.visited.empty());
}

TEST(LogScanStreamTest, AFailedPreadInTheMiddleOfAScanStopsWithTheIoStatus) {
  const TestLog log = four_records();
  auto io = io_with(log.bytes);
  uint64_t seen = 0;
  StreamScanResult result;

  // Two records are delivered, then the next pread fails. The third record is 4,136 bytes and
  // the window (chunk 100) cannot hold it yet, so it needs more reads.
  const Status status = scan_log_stream(
      *io, 0, 1,
      [&](const ScannedRecord&) {
        if (++seen == 2) {
          io->fail_next(Op::Pread, StatusCode::NoSpace);
        }
      },
      &result, 100);

  EXPECT_EQ(status.code(), StatusCode::NoSpace);
  EXPECT_EQ(seen, 2U);
}

TEST(LogScanStreamTest, AFailedPreadWhileReadingAZeroTailReturnsTheIoStatus) {
  // A failed read must not be mistaken for "the rest was zeros" or for a non-zero byte.
  const TestLog log = four_records();
  Bytes file = log.bytes;
  file.insert(file.end(), 5000, uint8_t{0});
  auto io = io_with(file);
  uint64_t seen = 0;
  StreamScanResult result;

  const Status status = scan_log_stream(
      *io, 0, 1,
      [&](const ScannedRecord&) {
        if (++seen == 4) {
          io->fail_next(Op::Pread, StatusCode::IoError);  // the next read is into the tail
        }
      },
      &result, 1000);

  EXPECT_EQ(status.code(), StatusCode::IoError);
}

TEST(LogScanStreamTest, AFileThatShrinksDuringTheScanIsAnIoErrorNotAnInfiniteLoop) {
  // file_size said the bytes were there, but pread returns end of file. Something else is
  // changing the log, which the scan cannot classify.
  const TestLog log = four_records();
  auto io = io_with(log.bytes);
  StreamScanResult result;

  const Status status = scan_log_stream(
      *io, 0, 1,
      [&](const ScannedRecord&) { io->bytes().resize(log.starts[2] + 10); }, &result, 100);

  EXPECT_EQ(status.code(), StatusCode::IoError);
}

}  // namespace
}  // namespace logblock
