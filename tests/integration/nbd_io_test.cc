// Block I/O through a real nbdkit and libnbd.
//
// Error codes come from nbdkit's conventions: out of range is EINVAL (or ENOSPC for a
// write, which is what nbdkit itself reports), a full log is ENOSPC, and a
// corrupt record is EIO.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "logblock/engine.h"

namespace logblock::itest {
namespace {

constexpr uint64_t kBlock = kBlockSize;         // 4096
constexpr uint64_t kDeviceBytes = kDeviceSize;  // 1 GiB

using Bytes = std::vector<uint8_t>;

// `size` bytes that depend on `seed`, so two blocks written with different seeds
// can be told apart when read back.
Bytes pattern(std::size_t size, uint8_t seed) {
  Bytes data(size);
  for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(seed + i * 7);
  return data;
}

// Each helper returns 0 on success, or the errno the server reported. That
// keeps the tests short: EXPECT_EQ(write_at(...), ENOSPC).
int write_at(nbd_handle* nbd, const Bytes& data, uint64_t offset) {
  if (nbd_pwrite(nbd, data.data(), data.size(), offset, 0) == 0) return 0;
  return nbd_get_errno() != 0 ? nbd_get_errno() : -1;
}

int read_at(nbd_handle* nbd, Bytes& out, uint64_t size, uint64_t offset) {
  out.assign(size, 0xEE);  // a filler that a successful read must overwrite
  if (nbd_pread(nbd, out.data(), out.size(), offset, 0) == 0) return 0;
  return nbd_get_errno() != 0 ? nbd_get_errno() : -1;
}

int trim_at(nbd_handle* nbd, uint64_t size, uint64_t offset) {
  if (nbd_trim(nbd, size, offset, 0) == 0) return 0;
  return nbd_get_errno() != 0 ? nbd_get_errno() : -1;
}

class NbdIo : public ::testing::Test {
 protected:
  // Starts the plugin with the given cap and connects. Call at the start of each
  // test (a cap is part of the test's setup, so it is not fixed in SetUp).
  void start_server(const std::string& cap) {
    ASSERT_TRUE(harness_.start(harness_.default_settings(cap))) << "stderr:\n"
                                                                << harness_.stop().stderr_text;
    nbd_ = harness_.connect();
    ASSERT_TRUE(nbd_);
  }

  Harness harness_;
  NbdHandle nbd_;
};

TEST_F(NbdIo, ExportSizeIsOneGiB) {
  ASSERT_NO_FATAL_FAILURE(start_server("1M"));
  EXPECT_EQ(nbd_get_size(nbd_.get()), 1073741824);
}

TEST_F(NbdIo, AdvertisedFeatures) {
  ASSERT_NO_FATAL_FAILURE(start_server("1M"));
  // FUA is not offered. Trim, flush and write-zeroes are.
  EXPECT_EQ(nbd_can_fua(nbd_.get()), 0) << nbd_get_error();
  EXPECT_EQ(nbd_can_trim(nbd_.get()), 1) << nbd_get_error();
  EXPECT_EQ(nbd_can_flush(nbd_.get()), 1) << nbd_get_error();
  EXPECT_EQ(nbd_can_zero(nbd_.get()), 1) << nbd_get_error();
  EXPECT_EQ(nbd_is_read_only(nbd_.get()), 0) << nbd_get_error();
}

TEST_F(NbdIo, WriteReadFlushAndTrim) {
  ASSERT_NO_FATAL_FAILURE(start_server("1M"));
  nbd_handle* nbd = nbd_.get();

  // A block that was never written reads as zeros.
  Bytes got;
  ASSERT_EQ(read_at(nbd, got, kBlock, 3 * kBlock), 0) << nbd_get_error();
  EXPECT_EQ(got, Bytes(kBlock, 0));

  // Write, read back, flush.
  const Bytes block3 = pattern(kBlock, 1);
  ASSERT_EQ(write_at(nbd, block3, 3 * kBlock), 0) << nbd_get_error();
  ASSERT_EQ(read_at(nbd, got, kBlock, 3 * kBlock), 0) << nbd_get_error();
  EXPECT_EQ(got, block3);
  EXPECT_EQ(nbd_flush(nbd, 0), 0) << nbd_get_error();

  // A write that is not block-aligned: 200 bytes at byte 100 of block 3 must keep
  // the rest of the block.
  const Bytes patch = pattern(200, 99);
  ASSERT_EQ(write_at(nbd, patch, 3 * kBlock + 100), 0) << nbd_get_error();
  ASSERT_EQ(read_at(nbd, got, kBlock, 3 * kBlock), 0) << nbd_get_error();
  Bytes expected = block3;
  std::copy(patch.begin(), patch.end(), expected.begin() + 100);
  EXPECT_EQ(got, expected);

  // Trim a fully covered block: it reads as zeros afterwards.
  ASSERT_EQ(trim_at(nbd, kBlock, 3 * kBlock), 0) << nbd_get_error();
  ASSERT_EQ(read_at(nbd, got, kBlock, 3 * kBlock), 0) << nbd_get_error();
  EXPECT_EQ(got, Bytes(kBlock, 0));
}

TEST_F(NbdIo, RequestsPastTheEndFailAndTheConnectionSurvives) {
  ASSERT_NO_FATAL_FAILURE(start_server("1M"));
  nbd_handle* nbd = nbd_.get();
  Bytes got;

  // Reads and trims: EINVAL. Writes: EINVAL, or ENOSPC as nbdkit itself reports.
  EXPECT_EQ(read_at(nbd, got, kBlock, kDeviceBytes), EINVAL);
  EXPECT_EQ(read_at(nbd, got, kBlock, kDeviceBytes - 100), EINVAL) << "crosses the end";
  EXPECT_EQ(trim_at(nbd, kBlock, kDeviceBytes), EINVAL);
  const int write_past_end = write_at(nbd, pattern(kBlock, 5), kDeviceBytes);
  EXPECT_TRUE(write_past_end == EINVAL || write_past_end == ENOSPC)
      << "errno was " << write_past_end;
  const int write_across_end = write_at(nbd, pattern(kBlock, 5), kDeviceBytes - 100);
  EXPECT_TRUE(write_across_end == EINVAL || write_across_end == ENOSPC)
      << "errno was " << write_across_end;
  // An offset so large that offset + length wraps around 2^64.
  EXPECT_EQ(read_at(nbd, got, kBlock, UINT64_MAX - 100), EINVAL);

  // The next valid request succeeds, on the same connection. nbd_aio_is_ready
  // is false once the server has dropped the connection.
  ASSERT_TRUE(nbd_aio_is_ready(nbd)) << "the server dropped the connection";
  const Bytes data = pattern(kBlock, 9);
  ASSERT_EQ(write_at(nbd, data, 0), 0) << nbd_get_error();
  ASSERT_EQ(read_at(nbd, got, kBlock, 0), 0) << nbd_get_error();
  EXPECT_EQ(got, data);
}

TEST_F(NbdIo, WriteBeyondSmallCapIsNoSpaceAndReadsStillWork) {
  // 8192 bytes hold one 4,136-byte DATA record (a second would need 8,272).
  ASSERT_NO_FATAL_FAILURE(start_server("8192"));
  nbd_handle* nbd = nbd_.get();

  const Bytes first = pattern(kBlock, 1);
  ASSERT_EQ(write_at(nbd, first, 0), 0) << nbd_get_error();

  // A different block needs a second record and does not fit.
  EXPECT_EQ(write_at(nbd, pattern(kBlock, 2), kBlock), ENOSPC);
  // So does an overwrite of the same block: every write appends a record.
  EXPECT_EQ(write_at(nbd, pattern(kBlock, 3), 0), ENOSPC);

  // Reads still work on a full log: the old data is intact and the rejected
  // write left no trace.
  Bytes got;
  ASSERT_EQ(read_at(nbd, got, kBlock, 0), 0) << nbd_get_error();
  EXPECT_EQ(got, first);
  ASSERT_EQ(read_at(nbd, got, kBlock, kBlock), 0) << nbd_get_error();
  EXPECT_EQ(got, Bytes(kBlock, 0));
}

TEST_F(NbdIo, CorruptPayloadReadsEioAndOtherBlocksStillWork) {
  ASSERT_NO_FATAL_FAILURE(start_server("1M"));
  nbd_handle* nbd = nbd_.get();

  // Two writes, so the log holds two DATA records: block 0 at file offset 0, and
  // block 1 right after it at offset 4,136.
  const Bytes block0 = pattern(kBlock, 10);
  const Bytes block1 = pattern(kBlock, 20);
  ASSERT_EQ(write_at(nbd, block0, 0), 0) << nbd_get_error();
  ASSERT_EQ(write_at(nbd, block1, kBlock), 0) << nbd_get_error();

  // Flip the first payload byte of record 0 (the header is 40 bytes) in the log
  // file while the server runs. The server has already written the record, and
  // the page cache is shared, so a plain file write is enough.
  {
    std::fstream log(harness_.log_path(), std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(log.is_open());
    log.seekg(static_cast<std::streamoff>(kRecordHeaderSize));
    char byte = 0;
    log.read(&byte, 1);
    ASSERT_TRUE(log.good());
    byte = static_cast<char>(~byte);
    log.seekp(static_cast<std::streamoff>(kRecordHeaderSize));
    log.write(&byte, 1);
    log.flush();
    ASSERT_TRUE(log.good());
  }

  // The checksum no longer matches, so the read fails with EIO, never with bad
  // data.
  Bytes got;
  EXPECT_EQ(read_at(nbd, got, kBlock, 0), EIO);
  // The damage is local to block 0: another block reads fine, and so do new writes.
  ASSERT_EQ(read_at(nbd, got, kBlock, kBlock), 0) << nbd_get_error();
  EXPECT_EQ(got, block1);
  const Bytes block5 = pattern(kBlock, 30);
  ASSERT_EQ(write_at(nbd, block5, 5 * kBlock), 0) << nbd_get_error();
  ASSERT_EQ(read_at(nbd, got, kBlock, 5 * kBlock), 0) << nbd_get_error();
  EXPECT_EQ(got, block5);
}

}  // namespace
}  // namespace logblock::itest
