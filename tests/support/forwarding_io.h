#pragma once

#include <cstdint>
#include <string>

#include "logblock/io.h"

namespace logblock::test {

// Engine::create takes ownership of its Io and destroys it if creation fails. A test usually
// still needs to look at its FaultIo (calls and bytes) afterwards, and to start a second
// engine on the same bytes, so the engine gets this wrapper and the test keeps the FaultIo.
// Test-only, header-only.
class ForwardingIo final : public Io {
 public:
  explicit ForwardingIo(Io& target) : target_(target) {}
  Status open_log_existing(const std::string& path, bool* found) override {
    return target_.open_log_existing(path, found);
  }
  Status create_log(const std::string& path, bool* created) override {
    return target_.create_log(path, created);
  }
  Status file_size(uint64_t* size) override { return target_.file_size(size); }
  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override {
    return target_.pread(offset, buf, len, bytes_read);
  }
  Status pwrite(uint64_t offset, const void* buf, size_t len) override {
    return target_.pwrite(offset, buf, len);
  }
  Status fsync() override { return target_.fsync(); }
  Status ftruncate(uint64_t size) override { return target_.ftruncate(size); }
  Status fsync_parent_dir() override { return target_.fsync_parent_dir(); }
  Status lock_log() override { return target_.lock_log(); }
  Status open_ckpt(const std::string& path, bool create, bool* opened, bool* created) override {
    return target_.open_ckpt(path, create, opened, created);
  }
  Status ckpt_size(uint64_t* size) override { return target_.ckpt_size(size); }
  Status ckpt_pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override {
    return target_.ckpt_pread(offset, buf, len, bytes_read);
  }
  Status ckpt_pwrite(uint64_t offset, const void* buf, size_t len) override {
    return target_.ckpt_pwrite(offset, buf, len);
  }
  Status ckpt_fsync() override { return target_.ckpt_fsync(); }

 private:
  Io& target_;
};

}  // namespace logblock::test
