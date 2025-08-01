// Copyright 2024 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tensorstore/internal/os/file_util.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tensorstore/internal/os/aligned_alloc.h"
#include "tensorstore/internal/os/file_info.h"
#include "tensorstore/internal/testing/scoped_directory.h"
#include "tensorstore/util/span.h"
#include "tensorstore/util/status_testutil.h"

namespace {

using ::tensorstore::IsOk;
using ::tensorstore::IsOkAndHolds;
using ::tensorstore::StatusIs;
using ::tensorstore::internal_os::AllocatePageAlignedRegion;
using ::tensorstore::internal_os::DeleteFile;
using ::tensorstore::internal_os::DeleteOpenFile;
using ::tensorstore::internal_os::FileInfo;
using ::tensorstore::internal_os::FsyncFile;
using ::tensorstore::internal_os::GetDefaultPageSize;
using ::tensorstore::internal_os::GetDirectIoBlockAlignment;
using ::tensorstore::internal_os::GetFileInfo;
using ::tensorstore::internal_os::IsDirSeparator;
using ::tensorstore::internal_os::MemmapFileReadOnly;
using ::tensorstore::internal_os::OpenFileWrapper;
using ::tensorstore::internal_os::OpenFlags;
using ::tensorstore::internal_os::PReadFromFile;
using ::tensorstore::internal_os::ReadAllToString;
using ::tensorstore::internal_os::ReadFromFile;
using ::tensorstore::internal_os::RenameOpenFile;
using ::tensorstore::internal_os::TruncateFile;
using ::tensorstore::internal_os::WriteCordToFile;
using ::tensorstore::internal_os::WriteToFile;
using ::tensorstore::internal_testing::ScopedTemporaryDirectory;

TEST(FileUtilTest, Basics) {
  ScopedTemporaryDirectory tempdir;
  std::string foo_txt = tempdir.path() + "/foo.txt";
  std::string renamed_txt = tempdir.path() + "/renamed.txt";

  EXPECT_TRUE(IsDirSeparator('/'));

  // File time resolution is not as accurate as the current clock.
  auto now = absl::Now() - absl::Seconds(1);

  // Missing files:
  {
    auto f = OpenFileWrapper(foo_txt, OpenFlags::DefaultRead);
    EXPECT_THAT(f, StatusIs(absl::StatusCode::kNotFound));

    EXPECT_THAT(DeleteFile(foo_txt), StatusIs(absl::StatusCode::kNotFound));
  }

  // Write a file:
  {
    auto f = OpenFileWrapper(foo_txt, OpenFlags::DefaultWrite);

    EXPECT_THAT(f, IsOk());
    EXPECT_THAT(TruncateFile(f->get()), IsOk());

    EXPECT_THAT(WriteCordToFile(f->get(), absl::Cord("foo")), IsOkAndHolds(3));
    EXPECT_THAT(WriteToFile(f->get(), "bar", 3), IsOkAndHolds(3));
    EXPECT_THAT(FsyncFile(f->get()), IsOk());
  }

  EXPECT_THAT(ReadAllToString(foo_txt), IsOkAndHolds(std::string("foobar")));

  // Read a file:
  {
    char buf[16];
    auto f = OpenFileWrapper(foo_txt, OpenFlags::DefaultRead);
    EXPECT_THAT(f, IsOk());

    EXPECT_THAT(ReadFromFile(f->get(), tensorstore::span(buf, 3)),
                IsOkAndHolds(3));
    EXPECT_THAT(std::string_view(buf, 3), testing::StrEq("foo"));
    EXPECT_THAT(PReadFromFile(f->get(), tensorstore::span(buf, 3), 0),
                IsOkAndHolds(3));
    EXPECT_THAT(std::string_view(buf, 3), testing::StrEq("foo"));

    // Check the file info
    FileInfo info;
    EXPECT_THAT(GetFileInfo(f->get(), &info), IsOk());
    EXPECT_TRUE(info.IsRegularFile());
    EXPECT_THAT(info.GetSize(), 6);
    EXPECT_THAT(info.GetMTime(), ::testing::Ge(now));
    EXPECT_THAT(info.GetCTime(), ::testing::Ge(now));
    EXPECT_THAT(info.GetMode() & 0600, ::testing::Eq(0600));

    EXPECT_THAT(RenameOpenFile(f->get(), foo_txt, renamed_txt), IsOk());
  }

  // Truncate a read-only file.
  {
    auto f = OpenFileWrapper(renamed_txt, OpenFlags::DefaultRead);
    EXPECT_THAT(f, IsOk());

    // Can't truncate a read-only file.
    EXPECT_THAT(
        TruncateFile(f->get()),
        ::testing::AnyOf(StatusIs(absl::StatusCode::kInvalidArgument),
                         StatusIs(absl::StatusCode::kPermissionDenied)));
  }

  // Delete an open file.
  {
    std::string bar_txt = tempdir.path() + "/bar.txt";
    auto f = OpenFileWrapper(bar_txt, OpenFlags::DefaultWrite);
    EXPECT_THAT(WriteToFile(f->get(), "bar", 3), IsOkAndHolds(3));
    EXPECT_THAT(DeleteOpenFile(f->get(), bar_txt), IsOk());
  }
}

TEST(FileUtilTest, ExclusiveFile) {
  ScopedTemporaryDirectory tempdir;
  std::string foo_txt = absl::StrCat(tempdir.path(), "/foo.txt",
                                     tensorstore::internal_os::kLockSuffix);

  // Create
  {
    auto f = OpenFileWrapper(foo_txt, OpenFlags::Create | OpenFlags::Exclusive |
                                          OpenFlags::OpenWriteOnly);

    EXPECT_THAT(f, IsOk());
    EXPECT_THAT(WriteCordToFile(f->get(), absl::Cord("foo")), IsOkAndHolds(3));
  }

  // Create again
  {
    auto f = OpenFileWrapper(foo_txt, OpenFlags::Create | OpenFlags::Exclusive |
                                          OpenFlags::OpenReadWrite);
    EXPECT_THAT(f.status(),
                ::testing::AnyOf(StatusIs(absl::StatusCode::kAlreadyExists)));
  }
}

TEST(FileUtilTest, LockFile) {
  ScopedTemporaryDirectory tempdir;
  std::string foo_txt = absl::StrCat(tempdir.path(), "/foo.txt",
                                     tensorstore::internal_os::kLockSuffix);

  // Create
  auto f = OpenFileWrapper(foo_txt, OpenFlags::DefaultWrite);
  EXPECT_THAT(f, IsOk());

  // Lock
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto lock, tensorstore::internal_os::AcquireFdLock(f->get()));

  // Unlock
  lock(f->get());
}

TEST(FileUtilTest, MemmapFileReadOnly) {
  tensorstore::internal_testing::ScopedTemporaryDirectory tempdir;
  std::string foo_txt = absl::StrCat(tempdir.path(), "/baz.txt",
                                     tensorstore::internal_os::kLockSuffix);

  // Allocate two pages and fill them with a-z.
  uint32_t size = GetDefaultPageSize() * 2;
  std::unique_ptr<char[]> buffer;
  buffer.reset(new char[size]);
  for (uint32_t i = 0; i < size; ++i) {
    buffer[i] = 'a' + (i % 26);
  }
  std::string_view expected(buffer.get(), size);

  // Write a file:
  {
    auto f = OpenFileWrapper(foo_txt, OpenFlags::DefaultWrite);
    EXPECT_THAT(f, IsOk());

    EXPECT_THAT(WriteToFile(f->get(), expected.data(), expected.size()),
                IsOkAndHolds(expected.size()));
  }

  // Read with bad offset => error.
  {
    TENSORSTORE_ASSERT_OK_AND_ASSIGN(
        auto fd, OpenFileWrapper(foo_txt, OpenFlags::OpenReadOnly));
    auto result = MemmapFileReadOnly(fd.get(), /*offset=*/4, 0);
    EXPECT_THAT(result.status(), StatusIs(absl::StatusCode::kInvalidArgument));
  }

  // Read w/offset
  for (uint32_t offset : {uint32_t{0}, GetDefaultPageSize(), size}) {
    TENSORSTORE_ASSERT_OK_AND_ASSIGN(
        auto fd, OpenFileWrapper(foo_txt, OpenFlags::OpenReadOnly));

    TENSORSTORE_ASSERT_OK_AND_ASSIGN(auto data,
                                     MemmapFileReadOnly(fd.get(), offset, 0));
    EXPECT_EQ(data.as_string_view().size(), expected.size() - offset);
    EXPECT_THAT(data.as_string_view(), expected.substr(offset));

    auto cord_data = std::move(data).as_cord();
    EXPECT_EQ(cord_data.size(), expected.size() - offset);
    EXPECT_THAT(cord_data, expected.substr(offset));

    // Read a subset of the file.
    if (offset < size) {
      TENSORSTORE_ASSERT_OK_AND_ASSIGN(
          data, MemmapFileReadOnly(fd.get(), offset, 128));
      EXPECT_EQ(data.as_string_view().size(), 128);
      EXPECT_THAT(data.as_string_view(), expected.substr(offset, 128));
    }
  }
}

TEST(FileUtilTest, OpenDirect) {
  tensorstore::internal_testing::ScopedTemporaryDirectory tempdir;
  std::string foo_txt = absl::StrCat(tempdir.path(), "/baz.txt",
                                     tensorstore::internal_os::kLockSuffix);

  size_t alignment = 0;

  // Allocate two pages and fill them with a-z.
  std::string expected(4096, 'a');
  for (uint32_t i = 0; i < expected.size(); ++i) {
    expected[i] = 'a' + (i % 26);
  }

  // Write a file:
  {
    auto f = OpenFileWrapper(foo_txt, OpenFlags::DefaultWrite);
    EXPECT_THAT(f, IsOk());
    alignment = GetDirectIoBlockAlignment(f->get());

    EXPECT_THAT(WriteToFile(f->get(), expected.data(), expected.size()),
                IsOkAndHolds(expected.size()));
  }
  if (alignment == 0) {
    // Skip the rest if the file system doesn't support direct IO.
    return;
  }

  EXPECT_THAT(alignment, ::testing::Gt(0));
  EXPECT_THAT(alignment, ::testing::Le(4096));

  // Use 512 as the default alignment if the file system doesn't support
  // STATX_DIOALIGN.
  if (alignment == 0) alignment = 512;

  // For direct IO, all reads and buffers must be aligned to the block size.
  auto allocation = AllocatePageAlignedRegion(alignment, expected.size());
  tensorstore::span<char> buffer(allocation.data(), expected.size());
  std::string_view read_buffer(allocation.data(), expected.size());
  {
    TENSORSTORE_ASSERT_OK_AND_ASSIGN(
        auto fd,
        OpenFileWrapper(foo_txt, OpenFlags::OpenReadOnly | OpenFlags::Direct));

    EXPECT_THAT(ReadFromFile(fd.get(), buffer), IsOkAndHolds(expected.size()));
    EXPECT_THAT(read_buffer, testing::StrEq(expected));

    EXPECT_THAT(PReadFromFile(fd.get(), buffer, 0),
                IsOkAndHolds(expected.size()));
    EXPECT_THAT(read_buffer, testing::StrEq(expected));
  }
}

}  // namespace
