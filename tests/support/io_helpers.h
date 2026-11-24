#pragma once

// Test-only: open the log the way a test usually wants, creating it if it is missing. Built from the
// two calls production uses (open_log_existing, then create_log), so no create-without-O_EXCL call
// exists on Io for production code to reach for.

#include <string>

#include "logblock/io.h"

namespace logblock::test {

inline Status open_or_create_log(Io& io, const std::string& path) {
  bool found = false;
  if (Status s = io.open_log_existing(path, &found); !s.ok() || found) {
    return s;
  }
  bool created = false;
  if (Status s = io.create_log(path, &created); !s.ok()) {
    return s;
  }
  if (!created) {
    return {StatusCode::Failed, "log appeared while it was being created: " + path};
  }
  return Status::Ok();
}

}  // namespace logblock::test
