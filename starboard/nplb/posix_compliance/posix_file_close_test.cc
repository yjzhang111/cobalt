// Copyright 2024 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// close is partially tested in posix_file_open_test.cc.

#include <unistd.h>

#include "testing/gtest/include/gtest/gtest.h"

namespace starboard {
namespace nplb {
namespace {

TEST(PosixFileCloseTest, CloseInvalidFails) {
  constexpr int kInvalidFD = -1;

  const int result = close(kInvalidFD);
  EXPECT_NE(result, 0) << "close() with an invalid fd should not succeed";
  constexpr int kExpectedFailure = -1;
  EXPECT_EQ(result, kExpectedFailure)
      << "close() with an invalid fd should "
      << "return " << kExpectedFailure << " but returned " << result;
  EXPECT_EQ(errno, EBADF) << "Expected errno to be EBADF (" << EBADF
                          << ") after attempting to close(" << kInvalidFD
                          << ") "
                          << "for an invalid file descriptor, but got " << errno
                          << " (" << strerror(errno) << ").";
}

}  // namespace
}  // namespace nplb
}  // namespace starboard
