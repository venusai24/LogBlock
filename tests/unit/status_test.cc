#include "logblock/status.h"

#include <gtest/gtest.h>

namespace logblock {
namespace {

TEST(Status, DefaultConstructedIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::Ok);
  EXPECT_TRUE(s.message().empty());
}

TEST(Status, OkFactoryIsOk) { EXPECT_TRUE(Status::Ok().ok()); }

TEST(Status, ErrorCarriesCodeAndMessage) {
  Status s(StatusCode::NoSpace, "log is full");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::NoSpace);
  EXPECT_EQ(s.message(), "log is full");
}

TEST(Status, EveryNonOkCodeIsAnError) {
  for (StatusCode code : {StatusCode::OutOfRange, StatusCode::NoSpace, StatusCode::Corrupt,
                          StatusCode::IoError, StatusCode::Failed}) {
    EXPECT_FALSE(Status(code, "x").ok()) << code_name(code);
  }
}

TEST(Status, CodeNamesMatchTheContract) {
  // These names appear in logs.
  EXPECT_STREQ(code_name(StatusCode::Ok), "Ok");
  EXPECT_STREQ(code_name(StatusCode::OutOfRange), "OutOfRange");
  EXPECT_STREQ(code_name(StatusCode::NoSpace), "NoSpace");
  EXPECT_STREQ(code_name(StatusCode::Corrupt), "Corrupt");
  EXPECT_STREQ(code_name(StatusCode::IoError), "IoError");
  EXPECT_STREQ(code_name(StatusCode::Failed), "Failed");
}

TEST(Status, ToStringJoinsNameAndMessage) {
  EXPECT_EQ(Status::Ok().to_string(), "Ok");
  EXPECT_EQ(Status(StatusCode::IoError, "pwrite failed").to_string(), "IoError: pwrite failed");
}

}  // namespace
}  // namespace logblock
