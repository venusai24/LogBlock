#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace logblock {

// What went wrong. The values match the error table of the engine.
// C++20 has no std::expected (that is C++23), so the I/O path returns a Status
// value instead of throwing.
enum class StatusCode : std::uint8_t {
  Ok,
  OutOfRange,  // range outside the device, or offset + len overflows
  NoSpace,     // request does not fit under the cap, or the file system is full
  Corrupt,     // a log record failed the read checks in the record format
  IoError,     // an underlying read, write, truncate or sync failed
  Failed,      // the engine is in a sticky failed state
};

inline const char* code_name(StatusCode code) {
  switch (code) {
    case StatusCode::Ok:
      return "Ok";
    case StatusCode::OutOfRange:
      return "OutOfRange";
    case StatusCode::NoSpace:
      return "NoSpace";
    case StatusCode::Corrupt:
      return "Corrupt";
    case StatusCode::IoError:
      return "IoError";
    case StatusCode::Failed:
      return "Failed";
  }
  return "Unknown";
}

// A result code plus a human-readable message. [[nodiscard]] makes the compiler
// warn when a caller ignores a returned Status, so errors are not dropped.
class [[nodiscard]] Status {
 public:
  Status() = default;  // Ok
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() { return {}; }

  [[nodiscard]] bool ok() const { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return message_; }

  // "Ok", or "<CodeName>: <message>" for logs.
  [[nodiscard]] std::string to_string() const {
    if (ok()) {
      return code_name(code_);
    }
    return std::string(code_name(code_)) + ": " + message_;
  }

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
};

}  // namespace logblock
