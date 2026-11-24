// The log scanner: rules 1 to 9, in one classifier shared by scan_log (bytes in memory) and
// scan_log_stream (bytes read through Io in chunks).
#include "log_scan.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace logblock {

namespace {

// Where the bytes come from: reads up to `len` bytes at `offset` into `buf`, and sets
// `*bytes_read` (fewer than `len` means end of file). Io::pread has this shape, and so does a
// memcpy from a span, which is how scan_log reuses the same core.
using ReadFn = std::function<Status(uint64_t offset, void* buf, size_t len, size_t* bytes_read)>;

// True if every byte is zero. An empty span counts as all zeros.
bool all_zero(std::span<const uint8_t> bytes) {
  return std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; });
}

// A sliding window over the file. The scan only moves forward, so the window drops bytes it has
// passed and never holds more than one chunk plus one record, whatever the file size.
class Window {
 public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a file size and a chunk size.
  Window(const ReadFn& read, uint64_t file_size, size_t chunk_size)
      : read_(read), file_size_(file_size), chunk_size_(std::max<size_t>(chunk_size, 1)) {}

  // Returns the `len` bytes at [pos, pos + len) in `*out`, reading more of the file if needed.
  // The caller guarantees pos + len <= file_size and that pos never goes backwards. The span is
  // valid only until the next call. Each pread asks for at most one chunk, and nothing is read
  // twice.
  Status bytes_at(uint64_t pos, size_t len, std::span<const uint8_t>* out) {
    if (pos + len > end()) {
      drop_before(pos);  // only now, so a run of small records costs no copying
      while (buf_.size() < len) {
        if (Status s = read_more(); !s.ok()) {
          return s;
        }
      }
    }
    *out = std::span<const uint8_t>(buf_.data() + (pos - start_), len);
    return Status::Ok();
  }

 private:
  [[nodiscard]] uint64_t end() const { return start_ + buf_.size(); }

  // Forgets everything before `pos`, which may be past the end of the window.
  void drop_before(uint64_t pos) {
    if (pos >= end()) {
      buf_.clear();
    } else {
      buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos - start_));
    }
    start_ = pos;
  }

  // Appends the next chunk (or what is left of the file) to the window.
  Status read_more() {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk_size_, file_size_ - end()));
    const size_t old_size = buf_.size();
    buf_.resize(old_size + want);
    size_t got = 0;
    Status s = read_(end() - want, buf_.data() + old_size, want, &got);
    buf_.resize(old_size + (s.ok() ? got : 0));
    if (!s.ok()) {
      return s;
    }
    if (got == 0) {
      // file_size said these bytes exist. Something else is changing the log, and retrying
      // would loop forever.
      return {StatusCode::IoError, "log ended at offset " + std::to_string(end()) +
                                       " but its size is " + std::to_string(file_size_)};
    }
    return Status::Ok();
  }

  const ReadFn& read_;
  uint64_t file_size_;
  size_t chunk_size_;
  std::vector<uint8_t> buf_;
  uint64_t start_ = 0;  // file offset of buf_[0]
};

// Sets *zero to whether every byte from `pos` to the end of the file is zero, reading the rest
// of the file one chunk at a time.
Status rest_is_zero(Window& window, uint64_t pos, uint64_t file_size, size_t chunk_size,
                    bool* zero) {
  *zero = true;
  while (pos < file_size) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk_size, file_size - pos));
    std::span<const uint8_t> bytes;
    if (Status s = window.bytes_at(pos, n, &bytes); !s.ok()) {
      return s;
    }
    if (!all_zero(bytes)) {
      *zero = false;
      return Status::Ok();
    }
    pos += n;
  }
  return Status::Ok();
}

// The classifier. Walks records from `start_offset` and applies rules 1 to 9, calling `visit` for
// each valid one. Returns a non-Ok Status only when `read` fails; every other outcome, damaged
// file included, is a classification in *out.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): sizes and offsets, named at each call.
Status scan_core(uint64_t file_size, const ReadFn& read, uint64_t start_offset,
                 uint64_t expected_seq, const RecordVisitor& visit, size_t chunk_size,
                 StreamScanResult* out) {
  chunk_size = std::max<size_t>(chunk_size, 1);
  Window window(read, file_size, chunk_size);
  StreamScanResult result;
  result.next_seq = expected_seq;  // rule 9: each record's seq must equal this, then it grows by 1

  // Fills in how and where scanning stopped. `pos` is the start of the bad position.
  auto stop_at = [&](ScanStop stop, uint64_t pos, const std::string& why) {
    result.stop = stop;
    result.valid_end = pos;
    result.detail = "at offset " + std::to_string(pos) + ": " + why;
    *out = std::move(result);
    return Status::Ok();
  };

  // An offset past the end names bytes that are not there. It is not a record cut short, so it
  // is damage (or a stale checkpoint), not a torn write.
  if (start_offset > file_size) {
    return stop_at(ScanStop::Corrupt, start_offset,
                   "start offset is past the end of the file, which is " +
                       std::to_string(file_size) + " bytes");
  }

  uint64_t pos = start_offset;  // start of the record being examined
  while (pos < file_size) {
    const uint64_t remaining = file_size - pos;

    // Too few bytes for even a header: the file ends in the middle of a header write.
    if (remaining < kRecordHeaderSize) {
      return stop_at(ScanStop::TornWrite, pos,
                     std::to_string(remaining) + " bytes are left, fewer than a 40-byte header");
    }

    // Rules 1 to 6 on the header alone. A bad header is corruption, even when the
    // record would also run past the end: a torn write only ever cuts a record that
    // was written correctly, so its header must be valid.
    std::span<const uint8_t> bytes;
    if (Status s = window.bytes_at(pos, kRecordHeaderSize, &bytes); !s.ok()) {
      return s;
    }
    RecordHeader header;
    const std::span<const uint8_t, kRecordHeaderSize> header_bytes(bytes.data(), kRecordHeaderSize);
    if (Status s = decode_header(header_bytes, &header); !s.ok()) {
      // One exception (D19): zeros from here to the end of the file. A power cut can leave
      // the file's new size on disk without its data, so this is a torn write. The check is
      // only made here, after the header failed (zeros always do: type 0 is invalid), so
      // valid records never pay for it. A single non-zero byte anywhere after `pos` keeps
      // it corruption. The tail can be many chunks long, so it is read chunk by chunk.
      bool zero = false;
      if (Status io = rest_is_zero(window, pos, file_size, chunk_size, &zero); !io.ok()) {
        return io;
      }
      if (zero) {
        return stop_at(ScanStop::TornWrite, pos,
                       std::to_string(remaining) +
                           " zero bytes from this record boundary to the end of the file");
      }
      return stop_at(ScanStop::Corrupt, pos, s.message());
    }

    // A valid header whose record does not fit: the write was cut short (torn). The
    // CRC cannot be checked without the payload, so this is all we can say.
    const uint64_t record_size = kRecordHeaderSize + header.data_len;
    if (record_size > remaining) {
      return stop_at(ScanStop::TornWrite, pos,
                     "a record of " + std::to_string(record_size) + " bytes has only " +
                         std::to_string(remaining) + " bytes in the file");
    }

    // Rules 7 and 8 (the record fits, the CRC matches). The record is complete here,
    // so any failure means damaged bytes, not a torn write. data_len is at most 4096 (rule 5),
    // so the window stays small.
    if (Status s = window.bytes_at(pos, record_size, &bytes); !s.ok()) {
      return s;
    }
    Record record;
    if (Status s = decode_record(bytes, &record); !s.ok()) {
      return stop_at(ScanStop::Corrupt, pos, s.message());
    }

    // Rule 9: seq must follow on from the previous record.
    if (header.seq != result.next_seq) {
      return stop_at(
          ScanStop::Corrupt, pos,
          "seq is " + std::to_string(header.seq) + ", expected " + std::to_string(result.next_seq));
    }

    visit({.header = header, .offset = pos});
    ++result.records;
    ++result.next_seq;
    pos += record_size;
  }

  result.valid_end = pos;  // Clean: every byte belongs to a valid record
  *out = std::move(result);
  return Status::Ok();
}

}  // namespace

Status scan_log_stream(Io& io, uint64_t start_offset, uint64_t expected_seq,
                       const RecordVisitor& visit, StreamScanResult* out, size_t chunk_size) {
  uint64_t file_size = 0;
  if (Status s = io.file_size(&file_size); !s.ok()) {
    return s;
  }
  const ReadFn read = [&io](uint64_t offset, void* buf, size_t len, size_t* bytes_read) {
    return io.pread(offset, buf, len, bytes_read);
  };
  return scan_core(file_size, read, start_offset, expected_seq, visit, chunk_size, out);
}

ScanResult scan_log(std::span<const uint8_t> file) {
  // The bytes are already in memory, so "reading" is a copy and cannot fail.
  const ReadFn read = [file](uint64_t offset, void* buf, size_t len, size_t* bytes_read) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len, file.size() - offset));
    std::memcpy(buf, file.data() + offset, n);
    *bytes_read = n;
    return Status::Ok();
  };

  ScanResult result;
  StreamScanResult stream;
  const Status status = scan_core(
      file.size(), read, 0, 1,
      [&result](const ScannedRecord& rec) { result.records.push_back(rec); }, kScanChunkSize,
      &stream);
  (void)status;  // always Ok: the read above never fails
  result.valid_end = stream.valid_end;
  result.stop = stream.stop;
  result.detail = std::move(stream.detail);
  return result;
}

}  // namespace logblock
