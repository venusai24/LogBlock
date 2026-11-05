#pragma once

// Shared pieces for the engine unit tests: byte-vector helpers and a fixture that
// builds an Engine over a FaultIo. Test-only, header-only.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "logblock/engine.h"
#include "support/fault_io.h"

namespace logblock::test {

using Bytes = std::vector<uint8_t>;

// A cap that is far larger than any test writes, so the cap never gets in the way.
inline constexpr uint64_t kBigCap = uint64_t{1} << 40;

// Deterministic bytes that differ from position to position. `i / 256` matters:
// without it the pattern would repeat every 256 bytes, and since 4096 is a multiple
// of 256, every block would look the same, so a read returning the wrong block
// would go unnoticed.
inline Bytes pattern(uint8_t seed, size_t n) {
  Bytes out(n);
  for (size_t i = 0; i < n; ++i) {
    out[i] = static_cast<uint8_t>(seed + i * 7 + i / 256);
  }
  return out;
}

inline Bytes zeros(size_t n) { return Bytes(n, 0); }

// Concatenates parts, to write expected images such as "10 zeros, the data, zeros".
inline Bytes join(const std::vector<Bytes>& parts) {
  Bytes out;
  for (const Bytes& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

inline Bytes slice(const Bytes& whole, size_t offset, size_t len) {
  return Bytes(whole.begin() + static_cast<std::ptrdiff_t>(offset),
               whole.begin() + static_cast<std::ptrdiff_t>(offset + len));
}

// Reports the first differing byte instead of printing 4,096 values.
inline ::testing::AssertionResult SameBytes(const Bytes& got, const Bytes& want) {
  if (got.size() != want.size()) {
    return ::testing::AssertionFailure()
           << "size is " << got.size() << ", expected " << want.size();
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != want[i]) {
      return ::testing::AssertionFailure()
             << "first difference at byte " << i << ": got " << static_cast<int>(got[i])
             << ", expected " << static_cast<int>(want[i]);
    }
  }
  return ::testing::AssertionSuccess();
}

class EngineTestBase : public ::testing::Test {
 protected:
  void SetUp() override { make_engine(kBigCap); }

  // (Re)creates the engine over a fresh, empty FaultIo with the given cap. Tests
  // that need a small cap call this instead of relying on SetUp's large one.
  void make_engine(uint64_t cap) {
    engine_.reset();  // destroys the old engine, and with it the old FaultIo
    auto io = std::make_unique<FaultIo>();
    io_ = io.get();  // the engine owns the Io and lives as long as this fixture
    Status s = Engine::create("engine-test.log", cap, std::move(io), &engine_);
    ASSERT_TRUE(s.ok()) << s.message();
  }

  Status write(uint64_t offset, const Bytes& data) {
    return engine_->write(offset, data.size(), data.data());
  }

  // Reads into a buffer pre-filled with 0xAA, so a read that forgets to fill part
  // of it (for example the zeros of an unwritten block) is noticed.
  Bytes read(uint64_t offset, size_t len) {
    Bytes out(len, 0xAA);
    Status s = engine_->read(offset, len, out.data());
    EXPECT_TRUE(s.ok()) << s.message();
    return out;
  }

  std::unique_ptr<Engine> engine_;
  FaultIo* io_ = nullptr;
};

}  // namespace logblock::test
