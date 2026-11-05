#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "logblock/io.h"

namespace logblock {
namespace {

namespace fs = std::filesystem;

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// ---------------------------------------------------------------------------
// write_fully / read_fully: the retry loops, fed by fake system calls so that
// short writes, EINTR and errors can be produced on demand.
// ---------------------------------------------------------------------------

TEST(WriteFully, ZeroLengthMakesNoCall) {
  int calls = 0;
  Status s = write_fully(
      [&](const void*, size_t, uint64_t) {
        ++calls;
        return ssize_t{0};
      },
      nullptr, 0, 0);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(calls, 0);
}

TEST(WriteFully, RetriesShortWritesFromTheRightOffset) {
  const std::string data = "0123456789";
  std::string written;
  std::vector<uint64_t> offsets;
  Status s = write_fully(
      [&](const void* buf, size_t len, uint64_t offset) {
        size_t n = std::min<size_t>(3, len);  // the "kernel" accepts 3 bytes at a time
        written.append(static_cast<const char*>(buf), n);
        offsets.push_back(offset);
        return static_cast<ssize_t>(n);
      },
      data.data(), data.size(), 100);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(written, data);
  EXPECT_EQ(offsets, (std::vector<uint64_t>{100, 103, 106, 109}));
}

TEST(WriteFully, RetriesAfterEintr) {
  int calls = 0;
  Status s = write_fully(
      [&](const void*, size_t len, uint64_t) -> ssize_t {
        if (++calls == 1) {
          errno = EINTR;
          return -1;
        }
        return static_cast<ssize_t>(len);
      },
      "abcd", 4, 0);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(calls, 2);
}

TEST(WriteFully, EnospcAndEdquotBecomeNoSpace) {
  for (int err : {ENOSPC, EDQUOT}) {
    Status s = write_fully(
        [&](const void*, size_t, uint64_t) -> ssize_t {
          errno = err;
          return -1;
        },
        "abcd", 4, 0);
    EXPECT_EQ(s.code(), StatusCode::NoSpace) << "errno " << err;
  }
}

TEST(WriteFully, OtherErrorsBecomeIoErrorWithTheErrnoText) {
  Status s = write_fully(
      [&](const void*, size_t, uint64_t) -> ssize_t {
        errno = EIO;
        return -1;
      },
      "abcd", 4, 0);
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_TRUE(contains(s.message(), "pwrite")) << s.message();
  EXPECT_TRUE(contains(s.message(), "Input/output error")) << s.message();
}

TEST(WriteFully, ErrorAfterPartialProgressIsReported) {
  int calls = 0;
  Status s = write_fully(
      [&](const void*, size_t, uint64_t) -> ssize_t {
        if (++calls == 1) {
          return 2;
        }
        errno = ENOSPC;
        return -1;
      },
      "abcd", 4, 0);
  EXPECT_EQ(s.code(), StatusCode::NoSpace);
}

TEST(WriteFully, ZeroByteWriteIsAnErrorNotAnInfiniteLoop) {
  Status s = write_fully([](const void*, size_t, uint64_t) { return ssize_t{0}; }, "abcd", 4, 0);
  EXPECT_EQ(s.code(), StatusCode::IoError);
}

TEST(ReadFully, AccumulatesShortReads) {
  const std::string source = "0123456789";
  std::string out(10, '?');
  size_t got = 0;
  Status s = read_fully(
      [&](void* buf, size_t len, uint64_t offset) {
        size_t n = std::min<size_t>(4, len);
        source.copy(static_cast<char*>(buf), n, offset);
        return static_cast<ssize_t>(n);
      },
      out.data(), out.size(), 0, &got);
  ASSERT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(got, 10U);
  EXPECT_EQ(out, source);
}

TEST(ReadFully, EndOfFileIsASuccessfulShortRead) {
  // 6 bytes exist; the third call returns 0, which means end of file.
  int calls = 0;
  char out[16];
  size_t got = 99;
  Status s = read_fully([&](void*, size_t, uint64_t) -> ssize_t { return ++calls <= 2 ? 3 : 0; },
                        out, sizeof(out), 0, &got);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(got, 6U);
}

TEST(ReadFully, RetriesAfterEintrAndReportsErrors) {
  int calls = 0;
  char out[4];
  size_t got = 0;
  Status s = read_fully(
      [&](void*, size_t len, uint64_t) -> ssize_t {
        if (++calls == 1) {
          errno = EINTR;
          return -1;
        }
        return static_cast<ssize_t>(len);
      },
      out, sizeof(out), 0, &got);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(got, 4U);

  s = read_fully(
      [](void*, size_t, uint64_t) -> ssize_t {
        errno = EIO;
        return -1;
      },
      out, sizeof(out), 0, &got);
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_TRUE(contains(s.message(), "pread")) << s.message();
}

// ---------------------------------------------------------------------------
// The real POSIX implementation, against real files. They live under the build
// directory because /tmp is tmpfs on Fedora.
// ---------------------------------------------------------------------------

class PosixIoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::path base = LOGBLOCK_TEST_TMP_DIR;
    fs::create_directories(base);
    std::string pattern = (base / "io-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    dir_ = pattern;
    path_ = (dir_ / "log").string();
    io_ = make_posix_io();
  }
  void TearDown() override {
    io_.reset();  // close the file before deleting it
    fs::remove_all(dir_);
  }

  static std::string bytes(size_t n, char first) {
    std::string s(n, 0);
    for (size_t i = 0; i < n; ++i) {
      s[i] = static_cast<char>(first + static_cast<char>(i % 26));
    }
    return s;
  }

  uint64_t size() {
    uint64_t n = ~0ULL;
    EXPECT_TRUE(io_->file_size(&n).ok());
    return n;
  }

  fs::path dir_;
  std::string path_;
  std::unique_ptr<Io> io_;
};

TEST_F(PosixIoTest, OpenCreatesAMissingFileWithMode0600) {
  ASSERT_FALSE(fs::exists(path_));
  ASSERT_TRUE(io_->open_log(path_).ok());
  struct stat st{};
  ASSERT_EQ(stat(path_.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600U);
  EXPECT_EQ(size(), 0U);
}

TEST_F(PosixIoTest, OpenLeavesAnExistingFileUnchanged) {
  {
    std::ofstream f(path_, std::ios::binary);
    f << "hello";
  }
  ASSERT_TRUE(io_->open_log(path_).ok());
  EXPECT_EQ(size(), 5U);
  char buf[5];
  size_t got = 0;
  ASSERT_TRUE(io_->pread(0, buf, 5, &got).ok());
  EXPECT_EQ(std::string(buf, got), "hello");
}

TEST_F(PosixIoTest, OpenFailsWhenTheDirectoryIsMissing) {
  std::string bad = (dir_ / "no-such-dir" / "log").string();
  Status s = io_->open_log(bad);
  EXPECT_EQ(s.code(), StatusCode::IoError);
  EXPECT_TRUE(contains(s.message(), bad)) << s.message();
}

TEST_F(PosixIoTest, OpeningTwiceIsAnError) {
  ASSERT_TRUE(io_->open_log(path_).ok());
  EXPECT_EQ(io_->open_log(path_).code(), StatusCode::IoError);
}

TEST_F(PosixIoTest, WriteThenReadBackAtAnExplicitOffset) {
  ASSERT_TRUE(io_->open_log(path_).ok());
  std::string data = bytes(4096, 'a');
  ASSERT_TRUE(io_->pwrite(40, data.data(), data.size()).ok());
  EXPECT_EQ(size(), 40U + 4096U);

  std::string back(4096, '?');
  size_t got = 0;
  ASSERT_TRUE(io_->pread(40, back.data(), back.size(), &got).ok());
  EXPECT_EQ(got, 4096U);
  EXPECT_EQ(back, data);
}

TEST_F(PosixIoTest, PreadPastTheEndReportsAShortRead) {
  ASSERT_TRUE(io_->open_log(path_).ok());
  std::string data = bytes(10, 'a');
  ASSERT_TRUE(io_->pwrite(0, data.data(), data.size()).ok());

  char buf[20];
  size_t got = 99;
  ASSERT_TRUE(io_->pread(5, buf, sizeof(buf), &got).ok());
  EXPECT_EQ(got, 5U);
  ASSERT_TRUE(io_->pread(100, buf, sizeof(buf), &got).ok());
  EXPECT_EQ(got, 0U);
}

TEST_F(PosixIoTest, FtruncateShrinksTheFile) {
  ASSERT_TRUE(io_->open_log(path_).ok());
  std::string data = bytes(100, 'a');
  ASSERT_TRUE(io_->pwrite(0, data.data(), data.size()).ok());
  ASSERT_TRUE(io_->ftruncate(60).ok());
  EXPECT_EQ(size(), 60U);
}

TEST_F(PosixIoTest, FsyncAndParentDirectoryFsyncSucceed) {
  ASSERT_TRUE(io_->open_log(path_).ok());
  std::string data = bytes(10, 'a');
  ASSERT_TRUE(io_->pwrite(0, data.data(), data.size()).ok());
  EXPECT_TRUE(io_->fsync().ok());
  EXPECT_TRUE(io_->fsync_parent_dir().ok());
}

TEST_F(PosixIoTest, EveryCallBeforeOpenIsAnError) {
  char buf[4] = {};
  size_t got = 0;
  uint64_t n = 0;
  EXPECT_EQ(io_->file_size(&n).code(), StatusCode::IoError);
  EXPECT_EQ(io_->pread(0, buf, 4, &got).code(), StatusCode::IoError);
  EXPECT_EQ(io_->pwrite(0, buf, 4).code(), StatusCode::IoError);
  EXPECT_EQ(io_->fsync().code(), StatusCode::IoError);
  EXPECT_EQ(io_->ftruncate(0).code(), StatusCode::IoError);
  EXPECT_EQ(io_->fsync_parent_dir().code(), StatusCode::IoError);
}

}  // namespace
}  // namespace logblock
