#pragma once

// Builds log images record by record, for tests that start an engine on an existing log.
// Test-only, header-only.

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "record.h"
#include "support/engine_test_base.h"

namespace logblock::test {

class LogBuilder {
 public:
  // Appends a DATA record for `block` whose payload is pattern(seed, kBlockSize), and returns
  // the offset the record starts at.
  uint64_t data(uint64_t block, uint8_t seed) {
    const uint64_t offset = bytes_.size();
    Bytes payload = pattern(seed, kBlockSize);
    std::array<uint8_t, kDataRecordSize> record{};
    Status s =
        encode_data_record(next_seq_, block, std::span<const uint8_t, kBlockSize>(payload), record);
    EXPECT_TRUE(s.ok()) << s.message();
    bytes_.append(reinterpret_cast<const char*>(record.data()), record.size());
    last_seq_ = next_seq_++;
    return offset;
  }

  // Appends a TRIM record for `count` blocks from `first_block`, and returns its offset.
  uint64_t trim(uint64_t first_block, uint32_t count) {
    const uint64_t offset = bytes_.size();
    std::array<uint8_t, kTrimRecordSize> record{};
    Status s = encode_trim_record(next_seq_, first_block, count, record);
    EXPECT_TRUE(s.ok()) << s.message();
    bytes_.append(reinterpret_cast<const char*>(record.data()), record.size());
    last_seq_ = next_seq_++;
    return offset;
  }

  // Makes the next record get this seq instead, to build a gap or a repeat.
  void set_next_seq(uint64_t seq) { next_seq_ = seq; }

  [[nodiscard]] const std::string& bytes() const { return bytes_; }
  [[nodiscard]] uint64_t end() const { return bytes_.size(); }
  // The seq of the most recent record, and the seq the next one would get.
  [[nodiscard]] uint64_t last_seq() const { return last_seq_; }
  [[nodiscard]] uint64_t next_seq() const { return next_seq_; }
  [[nodiscard]] uint64_t records() const { return next_seq_ - 1; }

  // The payload data(block, seed) writes, for expected values.
  static Bytes payload(uint8_t seed) { return pattern(seed, kBlockSize); }

 private:
  std::string bytes_;
  uint64_t next_seq_ = 1;
  uint64_t last_seq_ = 0;
};

}  // namespace logblock::test
