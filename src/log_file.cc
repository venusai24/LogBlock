// The append-only log: success-path appends and read-back.
//
// A failed append is cut back with ftruncate; if that also fails, the log is
// marked broken and accepts no more appends.
#include "log_file.h"

#include <algorithm>

namespace logblock {

// The only place that moves end_ and next_seq_. Doing it here, after the write
// has fully succeeded, is what keeps them describing complete records only.
Status LogFile::write_record(std::span<const uint8_t> record) {
  if (broken_) {
    // Torn bytes may be sitting past end_ (see below). Appending now would put a
    // record after them, which is exactly the gap this state exists to prevent.
    return {StatusCode::Failed, "the log is closed for appends after an earlier failed append"};
  }
  // One pwrite for the whole record, at an explicit offset (not O_APPEND). A failed
  // write can then leave at most a prefix of this one record behind.
  Status written = io_.pwrite(end_, record.data(), record.size());
  if (written.ok()) {
    end_ += record.size();
    ++next_seq_;
    return written;
  }

  // The write failed and may have left a partial record behind. Cut the file back
  // to the last good end so the log is again only whole records. end_ and
  // next_seq_ were never moved, so the next append starts exactly here.
  if (Status cut = io_.ftruncate(end_); !cut.ok()) {
    // The torn bytes cannot be removed. Stop appending for good. The caller
    // still gets the original error, with a note about what else went wrong.
    broken_ = true;
    return {written.code(), written.message() + "; cutting the log back to " +
                                std::to_string(end_) + " also failed (" + cut.message() +
                                "), so the log accepts no more appends"};
  }
  return written;
}

Status LogFile::append_data(uint64_t block_no, std::span<const uint8_t, kBlockSize> payload,
                            uint64_t* record_offset) {
  std::array<uint8_t, kDataRecordSize> record{};
  if (Status s = encode_data_record(next_seq_, block_no, payload, record); !s.ok()) {
    return s;
  }
  const uint64_t offset = end_;  // write_record moves end_, so remember where this one starts
  if (Status s = write_record(record); !s.ok()) {
    return s;
  }
  *record_offset = offset;
  return Status::Ok();
}

Status LogFile::append_trim(uint64_t block_no, uint32_t block_count) {
  std::array<uint8_t, kTrimRecordSize> record{};
  if (Status s = encode_trim_record(next_seq_, block_no, block_count, record); !s.ok()) {
    return s;
  }
  return write_record(record);
}

Status LogFile::read_record(uint64_t offset, std::array<uint8_t, kDataRecordSize>* buffer,
                            Record* record) {
  // Records live in [0, end_). An offset outside that is not a record start. This
  // must come first: for offset > end_, `end_ - offset` below would wrap around to a
  // huge number and the read would no longer be limited to the log. It also keeps
  // a huge offset away from pread.
  if (offset >= end_) {
    return {StatusCode::Corrupt, "record offset " + std::to_string(offset) +
                                     " is not inside the log (end is " + std::to_string(end_) +
                                     ")"};
  }
  // Read at most the largest record, and never past end_: bytes after it are not
  // part of the log (they could be a torn append that was not cleaned up).
  const size_t want = static_cast<size_t>(std::min<uint64_t>(buffer->size(), end_ - offset));
  size_t got = 0;
  if (Status s = io_.pread(offset, buffer->data(), want, &got); !s.ok()) {
    return s;
  }
  // decode_record checks rules 1 to 8. If the file is shorter than end_ says (got <
  // want), a record that runs off the end fails rule 7 there.
  return decode_record(std::span<const uint8_t>(buffer->data(), got), record);
}

}  // namespace logblock
