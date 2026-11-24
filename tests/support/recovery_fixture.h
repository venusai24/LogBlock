#pragma once

// A fixture for tests that start an Engine on files held by a FaultIo: start(), restart(), and
// helpers to read blocks and find calls. Shared by the recovery tests. Test-only, header-only.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "logblock/engine.h"
#include "support/engine_test_base.h"
#include "support/fault_io.h"
#include "support/forwarding_io.h"
#include "support/log_builder.h"

namespace logblock::test {

inline constexpr const char* kLogPath = "recovery.log";
inline constexpr const char* kCkptPath = "recovery.log.ckpt";

using Op = FaultIo::Op;

class RecoveryFixture : public ::testing::Test {
 protected:
  void SetUp() override { io_ = std::make_unique<FaultIo>(); }

  // Starts an engine on the bytes now in io_. The engine gets a forwarding wrapper, so io_
  // stays inspectable even when creation fails.
  Status start(uint64_t cap = kBigCap, uint64_t interval = kDefaultCheckpointInterval) {
    engine_.reset();
    return Engine::create(kLogPath, cap, std::make_unique<ForwardingIo>(*io_), &engine_, interval);
  }

  void load(const LogBuilder& log) { io_->bytes() = log.bytes(); }

  // A process restart: a new Io over a copy of the files as they are now.
  void restart() {
    engine_.reset();
    auto next = std::make_unique<FaultIo>();
    next->bytes() = io_->bytes();
    next->ckpt_bytes() = io_->ckpt_bytes();
    io_ = std::move(next);
  }

  Bytes read_block(uint64_t block) {
    Bytes out(kBlockSize, 0xAA);
    Status s = engine_->read(block * kBlockSize, kBlockSize, out.data());
    EXPECT_TRUE(s.ok()) << s.message();
    return out;
  }

  void expect_block(uint64_t block, uint8_t seed) {
    EXPECT_TRUE(SameBytes(read_block(block), LogBuilder::payload(seed))) << "block " << block;
  }
  void expect_zero_block(uint64_t block) {
    EXPECT_TRUE(SameBytes(read_block(block), zeros(kBlockSize))) << "block " << block;
  }

  // The index of the first call of this kind, or -1.
  [[nodiscard]] int first_call(Op op) const {
    for (size_t i = 0; i < io_->calls().size(); ++i) {
      if (io_->calls()[i].op == op) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  std::unique_ptr<FaultIo> io_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace logblock::test
