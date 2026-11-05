#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <system_error>

#include "logblock/io.h"

namespace logblock {
namespace {

// Turns an errno value into a Status. A full disk or exhausted quota is NoSpace,
// which clients see as ENOSPC. Everything else is a generic I/O error.
Status errno_status(int err, const std::string& what) {
  StatusCode code = (err == ENOSPC || err == EDQUOT) ? StatusCode::NoSpace : StatusCode::IoError;
  // error_code::message() is thread-safe, unlike strerror().
  return {code, what + ": " + std::error_code(err, std::generic_category()).message()};
}

// Owns a file descriptor and closes it when destroyed (RAII).
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { reset(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  // Moving transfers ownership, so exactly one Fd ever closes a descriptor.
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] bool valid() const { return fd_ >= 0; }
  [[nodiscard]] int get() const { return fd_; }
  void reset() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

class PosixIo final : public Io {
 public:
  Status open_log(const std::string& path) override {
    if (fd_.valid()) {
      return {StatusCode::IoError, "log is already open: " + path_};
    }
    // O_CLOEXEC: do not leak the descriptor into child processes. No O_TRUNC and
    // no O_APPEND: an existing log is left alone, and writes go to explicit offsets.
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
      return errno_status(errno, "cannot open log " + path);
    }
    fd_ = Fd(fd);
    path_ = path;
    return Status::Ok();
  }

  Status file_size(uint64_t* size) override {
    if (Status s = check_open(); !s.ok()) {
      return s;
    }
    struct stat st{};
    if (::fstat(fd_.get(), &st) != 0) {
      return errno_status(errno, "fstat " + path_);
    }
    *size = static_cast<uint64_t>(st.st_size);
    return Status::Ok();
  }

  Status pread(uint64_t offset, void* buf, size_t len, size_t* bytes_read) override {
    if (Status s = check_open(); !s.ok()) {
      return s;
    }
    return read_fully(
        [this](void* b, size_t n, uint64_t off) {
          return ::pread(fd_.get(), b, n, static_cast<off_t>(off));
        },
        buf, len, offset, bytes_read);
  }

  Status pwrite(uint64_t offset, const void* buf, size_t len) override {
    if (Status s = check_open(); !s.ok()) {
      return s;
    }
    return write_fully(
        [this](const void* b, size_t n, uint64_t off) {
          return ::pwrite(fd_.get(), b, n, static_cast<off_t>(off));
        },
        buf, len, offset);
  }

  Status fsync() override {
    if (Status s = check_open(); !s.ok()) {
      return s;
    }
    if (::fsync(fd_.get()) != 0) {
      return errno_status(errno, "fsync " + path_);
    }
    return Status::Ok();
  }

  Status ftruncate(uint64_t size) override {
    if (Status s = check_open(); !s.ok()) {
      return s;
    }
    if (::ftruncate(fd_.get(), static_cast<off_t>(size)) != 0) {
      return errno_status(errno, "ftruncate " + path_);
    }
    return Status::Ok();
  }

  Status fsync_parent_dir() override {
    if (Status s = check_open(); !s.ok()) {
      return s;
    }
    std::filesystem::path dir = std::filesystem::path(path_).parent_path();
    if (dir.empty()) {
      dir = ".";  // a bare file name lives in the current directory
    }
    Fd dir_fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir_fd.valid()) {
      return errno_status(errno, "cannot open directory " + dir.string());
    }
    if (::fsync(dir_fd.get()) != 0) {
      return errno_status(errno, "fsync directory " + dir.string());
    }
    return Status::Ok();
  }

 private:
  Status check_open() const {
    if (!fd_.valid()) {
      return {StatusCode::IoError, "log is not open"};
    }
    return Status::Ok();
  }

  Fd fd_;
  std::string path_;
};

}  // namespace

Status write_fully(const RawWriteFn& write_once, const void* buf, size_t len, uint64_t offset) {
  const auto* bytes = static_cast<const unsigned char*>(buf);
  size_t done = 0;
  while (done < len) {
    ssize_t n = write_once(bytes + done, len - done, offset + done);
    if (n < 0) {
      int err = errno;  // read errno before anything else can change it
      if (err == EINTR) {
        continue;  // interrupted by a signal before writing anything: try again
      }
      return errno_status(err, "pwrite");
    }
    if (n == 0) {
      // A write of len > 0 bytes should never return 0. Retrying could loop forever.
      return {StatusCode::IoError, "pwrite wrote 0 bytes"};
    }
    done += static_cast<size_t>(n);  // a short write: go round again for the rest
  }
  return Status::Ok();
}

Status read_fully(const RawReadFn& read_once, void* buf, size_t len, uint64_t offset,
                  size_t* bytes_read) {
  auto* bytes = static_cast<unsigned char*>(buf);
  size_t done = 0;
  while (done < len) {
    ssize_t n = read_once(bytes + done, len - done, offset + done);
    if (n < 0) {
      int err = errno;
      if (err == EINTR) {
        continue;
      }
      *bytes_read = done;
      return errno_status(err, "pread");
    }
    if (n == 0) {
      break;  // end of file: report what we got
    }
    done += static_cast<size_t>(n);
  }
  *bytes_read = done;
  return Status::Ok();
}

std::unique_ptr<Io> make_posix_io() { return std::make_unique<PosixIo>(); }

}  // namespace logblock
