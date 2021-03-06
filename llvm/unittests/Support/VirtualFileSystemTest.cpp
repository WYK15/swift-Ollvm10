//===- unittests/Support/VirtualFileSystem.cpp -------------- VFS tests ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <map>
#include <string>

using namespace llvm;
using llvm::sys::fs::UniqueID;
using testing::ElementsAre;
using testing::Pair;
using testing::UnorderedElementsAre;

namespace {
struct DummyFile : public vfs::File {
  vfs::Status S;
  explicit DummyFile(vfs::Status S) : S(S) {}
  llvm::ErrorOr<vfs::Status> status() override { return S; }
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  getBuffer(const Twine &Name, int64_t FileSize, bool RequiresNullTerminator,
            bool IsVolatile) override {
    llvm_unreachable("unimplemented");
  }
  std::error_code close() override { return std::error_code(); }
};

class DummyFileSystem : public vfs::FileSystem {
  int FSID;   // used to produce UniqueIDs
  int FileID; // used to produce UniqueIDs
  std::string WorkingDirectory;
  std::map<std::string, vfs::Status> FilesAndDirs;
  typedef std::map<std::string, vfs::Status>::const_iterator const_iterator;

  static int getNextFSID() {
    static int Count = 0;
    return Count++;
  }

public:
  DummyFileSystem() : FSID(getNextFSID()), FileID(0) {}

  ErrorOr<vfs::Status> status(const Twine &Path) override {
    auto I = findEntry(Path);
    if (I == FilesAndDirs.end())
      return make_error_code(llvm::errc::no_such_file_or_directory);
    return I->second;
  }
  ErrorOr<std::unique_ptr<vfs::File>>
  openFileForRead(const Twine &Path) override {
    auto S = status(Path);
    if (S)
      return std::unique_ptr<vfs::File>(new DummyFile{*S});
    return S.getError();
  }
  llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const override {
    return WorkingDirectory;
  }
  std::error_code setCurrentWorkingDirectory(const Twine &Path) override {
    WorkingDirectory = Path.str();
    return std::error_code();
  }
  // Map any symlink to "/symlink".
  std::error_code getRealPath(const Twine &Path,
                              SmallVectorImpl<char> &Output) const override {
    auto I = findEntry(Path);
    if (I == FilesAndDirs.end())
      return make_error_code(llvm::errc::no_such_file_or_directory);
    if (I->second.isSymlink()) {
      Output.clear();
      Twine("/symlink").toVector(Output);
      return std::error_code();
    }
    Output.clear();
    Path.toVector(Output);
    return std::error_code();
  }

  struct DirIterImpl : public llvm::vfs::detail::DirIterImpl {
    std::map<std::string, vfs::Status> &FilesAndDirs;
    std::map<std::string, vfs::Status>::iterator I;
    std::string Path;
    bool isInPath(StringRef S) {
      if (Path.size() < S.size() && S.find(Path) == 0) {
        auto LastSep = S.find_last_of('/');
        if (LastSep == Path.size() || LastSep == Path.size() - 1)
          return true;
      }
      return false;
    }
    DirIterImpl(std::map<std::string, vfs::Status> &FilesAndDirs,
                const Twine &_Path)
        : FilesAndDirs(FilesAndDirs), I(FilesAndDirs.begin()),
          Path(_Path.str()) {
      for (; I != FilesAndDirs.end(); ++I) {
        if (isInPath(I->first)) {
          CurrentEntry =
              vfs::directory_entry(I->second.getName(), I->second.getType());
          break;
        }
      }
    }
    std::error_code increment() override {
      ++I;
      for (; I != FilesAndDirs.end(); ++I) {
        if (isInPath(I->first)) {
          CurrentEntry =
              vfs::directory_entry(I->second.getName(), I->second.getType());
          break;
        }
      }
      if (I == FilesAndDirs.end())
        CurrentEntry = vfs::directory_entry();
      return std::error_code();
    }
  };

  vfs::directory_iterator dir_begin(const Twine &Dir,
                                    std::error_code &EC) override {
    return vfs::directory_iterator(
        std::make_shared<DirIterImpl>(FilesAndDirs, Dir));
  }

  void addEntry(StringRef Path, const vfs::Status &Status) {
    FilesAndDirs[Path] = Status;
  }

  const_iterator findEntry(const Twine &Path) const {
    SmallString<128> P;
    Path.toVector(P);
    std::error_code EC = makeAbsolute(P);
    assert(!EC);
    (void)EC;
    return FilesAndDirs.find(P.str());
  }

  void addRegularFile(StringRef Path, sys::fs::perms Perms = sys::fs::all_all) {
    vfs::Status S(Path, UniqueID(FSID, FileID++),
                  std::chrono::system_clock::now(), 0, 0, 1024,
                  sys::fs::file_type::regular_file, Perms);
    addEntry(Path, S);
  }

  void addDirectory(StringRef Path, sys::fs::perms Perms = sys::fs::all_all) {
    vfs::Status S(Path, UniqueID(FSID, FileID++),
                  std::chrono::system_clock::now(), 0, 0, 0,
                  sys::fs::file_type::directory_file, Perms);
    addEntry(Path, S);
  }

  void addSymlink(StringRef Path) {
    vfs::Status S(Path, UniqueID(FSID, FileID++),
                  std::chrono::system_clock::now(), 0, 0, 0,
                  sys::fs::file_type::symlink_file, sys::fs::all_all);
    addEntry(Path, S);
  }
};

class ErrorDummyFileSystem : public DummyFileSystem {
  std::error_code setCurrentWorkingDirectory(const Twine &Path) override {
    return llvm::errc::no_such_file_or_directory;
  }
};

/// Replace back-slashes by front-slashes.
std::string getPosixPath(std::string S) {
  SmallString<128> Result;
  llvm::sys::path::native(S, Result, llvm::sys::path::Style::posix);
  return Result.str();
}
} // end anonymous namespace

TEST(VirtualFileSystemTest, StatusQueries) {
  IntrusiveRefCntPtr<DummyFileSystem> D(new DummyFileSystem());
  ErrorOr<vfs::Status> Status((std::error_code()));

  D->addRegularFile("/foo");
  Status = D->status("/foo");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->isStatusKnown());
  EXPECT_FALSE(Status->isDirectory());
  EXPECT_TRUE(Status->isRegularFile());
  EXPECT_FALSE(Status->isSymlink());
  EXPECT_FALSE(Status->isOther());
  EXPECT_TRUE(Status->exists());

  D->addDirectory("/bar");
  Status = D->status("/bar");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->isStatusKnown());
  EXPECT_TRUE(Status->isDirectory());
  EXPECT_FALSE(Status->isRegularFile());
  EXPECT_FALSE(Status->isSymlink());
  EXPECT_FALSE(Status->isOther());
  EXPECT_TRUE(Status->exists());

  D->addSymlink("/baz");
  Status = D->status("/baz");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->isStatusKnown());
  EXPECT_FALSE(Status->isDirectory());
  EXPECT_FALSE(Status->isRegularFile());
  EXPECT_TRUE(Status->isSymlink());
  EXPECT_FALSE(Status->isOther());
  EXPECT_TRUE(Status->exists());

  EXPECT_TRUE(Status->equivalent(*Status));
  ErrorOr<vfs::Status> Status2 = D->status("/foo");
  ASSERT_FALSE(Status2.getError());
  EXPECT_FALSE(Status->equivalent(*Status2));
}

TEST(VirtualFileSystemTest, BaseOnlyOverlay) {
  IntrusiveRefCntPtr<DummyFileSystem> D(new DummyFileSystem());
  ErrorOr<vfs::Status> Status((std::error_code()));
  EXPECT_FALSE(Status = D->status("/foo"));

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(new vfs::OverlayFileSystem(D));
  EXPECT_FALSE(Status = O->status("/foo"));

  D->addRegularFile("/foo");
  Status = D->status("/foo");
  EXPECT_FALSE(Status.getError());

  ErrorOr<vfs::Status> Status2((std::error_code()));
  Status2 = O->status("/foo");
  EXPECT_FALSE(Status2.getError());
  EXPECT_TRUE(Status->equivalent(*Status2));
}

TEST(VirtualFileSystemTest, GetRealPathInOverlay) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("/foo");
  Lower->addSymlink("/lower_link");
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Upper);

  // Regular file.
  SmallString<16> RealPath;
  EXPECT_FALSE(O->getRealPath("/foo", RealPath));
  EXPECT_EQ(RealPath.str(), "/foo");

  // Expect no error getting real path for symlink in lower overlay.
  EXPECT_FALSE(O->getRealPath("/lower_link", RealPath));
  EXPECT_EQ(RealPath.str(), "/symlink");

  // Try a non-existing link.
  EXPECT_EQ(O->getRealPath("/upper_link", RealPath),
            errc::no_such_file_or_directory);

  // Add a new symlink in upper.
  Upper->addSymlink("/upper_link");
  EXPECT_FALSE(O->getRealPath("/upper_link", RealPath));
  EXPECT_EQ(RealPath.str(), "/symlink");
}

TEST(VirtualFileSystemTest, OverlayFiles) {
  IntrusiveRefCntPtr<DummyFileSystem> Base(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Middle(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Top(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Base));
  O->pushOverlay(Middle);
  O->pushOverlay(Top);

  ErrorOr<vfs::Status> Status1((std::error_code())),
      Status2((std::error_code())), Status3((std::error_code())),
      StatusB((std::error_code())), StatusM((std::error_code())),
      StatusT((std::error_code()));

  Base->addRegularFile("/foo");
  StatusB = Base->status("/foo");
  ASSERT_FALSE(StatusB.getError());
  Status1 = O->status("/foo");
  ASSERT_FALSE(Status1.getError());
  Middle->addRegularFile("/foo");
  StatusM = Middle->status("/foo");
  ASSERT_FALSE(StatusM.getError());
  Status2 = O->status("/foo");
  ASSERT_FALSE(Status2.getError());
  Top->addRegularFile("/foo");
  StatusT = Top->status("/foo");
  ASSERT_FALSE(StatusT.getError());
  Status3 = O->status("/foo");
  ASSERT_FALSE(Status3.getError());

  EXPECT_TRUE(Status1->equivalent(*StatusB));
  EXPECT_TRUE(Status2->equivalent(*StatusM));
  EXPECT_TRUE(Status3->equivalent(*StatusT));

  EXPECT_FALSE(Status1->equivalent(*Status2));
  EXPECT_FALSE(Status2->equivalent(*Status3));
  EXPECT_FALSE(Status1->equivalent(*Status3));
}

TEST(VirtualFileSystemTest, OverlayDirsNonMerged) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Upper);

  Lower->addDirectory("/lower-only");
  Upper->addDirectory("/upper-only");

  // non-merged paths should be the same
  ErrorOr<vfs::Status> Status1 = Lower->status("/lower-only");
  ASSERT_FALSE(Status1.getError());
  ErrorOr<vfs::Status> Status2 = O->status("/lower-only");
  ASSERT_FALSE(Status2.getError());
  EXPECT_TRUE(Status1->equivalent(*Status2));

  Status1 = Upper->status("/upper-only");
  ASSERT_FALSE(Status1.getError());
  Status2 = O->status("/upper-only");
  ASSERT_FALSE(Status2.getError());
  EXPECT_TRUE(Status1->equivalent(*Status2));
}

TEST(VirtualFileSystemTest, MergedDirPermissions) {
  // merged directories get the permissions of the upper dir
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Upper);

  ErrorOr<vfs::Status> Status((std::error_code()));
  Lower->addDirectory("/both", sys::fs::owner_read);
  Upper->addDirectory("/both", sys::fs::owner_all | sys::fs::group_read);
  Status = O->status("/both");
  ASSERT_FALSE(Status.getError());
  EXPECT_EQ(0740, Status->getPermissions());

  // permissions (as usual) are not recursively applied
  Lower->addRegularFile("/both/foo", sys::fs::owner_read);
  Upper->addRegularFile("/both/bar", sys::fs::owner_write);
  Status = O->status("/both/foo");
  ASSERT_FALSE(Status.getError());
  EXPECT_EQ(0400, Status->getPermissions());
  Status = O->status("/both/bar");
  ASSERT_FALSE(Status.getError());
  EXPECT_EQ(0200, Status->getPermissions());
}

TEST(VirtualFileSystemTest, OverlayIterator) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("/foo");
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Upper);

  ErrorOr<vfs::Status> Status((std::error_code()));
  {
    auto it = O->overlays_begin();
    auto end = O->overlays_end();

    EXPECT_NE(it, end);

    Status = (*it)->status("/foo");
    ASSERT_TRUE(Status.getError());

    it++;
    EXPECT_NE(it, end);

    Status = (*it)->status("/foo");
    ASSERT_FALSE(Status.getError());
    EXPECT_TRUE(Status->exists());

    it++;
    EXPECT_EQ(it, end);
  }

  {
    auto it = O->overlays_rbegin();
    auto end = O->overlays_rend();

    EXPECT_NE(it, end);

    Status = (*it)->status("/foo");
    ASSERT_FALSE(Status.getError());
    EXPECT_TRUE(Status->exists());

    it++;
    EXPECT_NE(it, end);

    Status = (*it)->status("/foo");
    ASSERT_TRUE(Status.getError());

    it++;
    EXPECT_EQ(it, end);
  }
}

namespace {
struct ScopedDir {
  SmallString<128> Path;
  ScopedDir(const Twine &Name, bool Unique = false) {
    std::error_code EC;
    if (Unique) {
      EC = llvm::sys::fs::createUniqueDirectory(Name, Path);
      if (!EC) {
        // Resolve any symlinks in the new directory.
        std::string UnresolvedPath = Path.str();
        EC = llvm::sys::fs::real_path(UnresolvedPath, Path);
      }
    } else {
      Path = Name.str();
      EC = llvm::sys::fs::create_directory(Twine(Path));
    }
    if (EC)
      Path = "";
    EXPECT_FALSE(EC) << EC.message();
  }
  ~ScopedDir() {
    if (Path != "") {
      EXPECT_FALSE(llvm::sys::fs::remove(Path.str()));
    }
  }
  operator StringRef() { return Path.str(); }
};

struct ScopedLink {
  SmallString<128> Path;
  ScopedLink(const Twine &To, const Twine &From) {
    Path = From.str();
    std::error_code EC = sys::fs::create_link(To, From);
    if (EC)
      Path = "";
    EXPECT_FALSE(EC);
  }
  ~ScopedLink() {
    if (Path != "") {
      EXPECT_FALSE(llvm::sys::fs::remove(Path.str()));
    }
  }
  operator StringRef() { return Path.str(); }
};

struct ScopedFile {
  SmallString<128> Path;
  ScopedFile(const Twine &Path, StringRef Contents) {
    Path.toVector(this->Path);
    std::error_code EC;
    raw_fd_ostream OS(this->Path, EC);
    EXPECT_FALSE(EC);
    OS << Contents;
    OS.flush();
    EXPECT_FALSE(OS.error());
    if (EC || OS.error())
      this->Path = "";
  }
  ~ScopedFile() {
    if (Path != "") {
      EXPECT_FALSE(llvm::sys::fs::remove(Path.str()));
    }
  }
};
} // end anonymous namespace

TEST(VirtualFileSystemTest, BasicRealFSIteration) {
  ScopedDir TestDirectory("virtual-file-system-test", /*Unique*/ true);
  IntrusiveRefCntPtr<vfs::FileSystem> FS = vfs::getRealFileSystem();

  std::error_code EC;
  vfs::directory_iterator I = FS->dir_begin(Twine(TestDirectory), EC);
  ASSERT_FALSE(EC);
  EXPECT_EQ(vfs::directory_iterator(), I); // empty directory is empty

  ScopedDir _a(TestDirectory + "/a");
  ScopedDir _ab(TestDirectory + "/a/b");
  ScopedDir _c(TestDirectory + "/c");
  ScopedDir _cd(TestDirectory + "/c/d");

  I = FS->dir_begin(Twine(TestDirectory), EC);
  ASSERT_FALSE(EC);
  ASSERT_NE(vfs::directory_iterator(), I);
  // Check either a or c, since we can't rely on the iteration order.
  EXPECT_TRUE(I->path().endswith("a") || I->path().endswith("c"));
  I.increment(EC);
  ASSERT_FALSE(EC);
  ASSERT_NE(vfs::directory_iterator(), I);
  EXPECT_TRUE(I->path().endswith("a") || I->path().endswith("c"));
  I.increment(EC);
  EXPECT_EQ(vfs::directory_iterator(), I);
}

#ifdef LLVM_ON_UNIX
TEST(VirtualFileSystemTest, MultipleWorkingDirs) {
  // Our root contains a/aa, b/bb, c, where c is a link to a/.
  // Run tests both in root/b/ and root/c/ (to test "normal" and symlink dirs).
  // Interleave operations to show the working directories are independent.
  ScopedDir Root("r", true), ADir(Root.Path + "/a"), BDir(Root.Path + "/b");
  ScopedLink C(ADir.Path, Root.Path + "/c");
  ScopedFile AA(ADir.Path + "/aa", "aaaa"), BB(BDir.Path + "/bb", "bbbb");
  std::unique_ptr<vfs::FileSystem> BFS = vfs::createPhysicalFileSystem(),
                                   CFS = vfs::createPhysicalFileSystem();

  ASSERT_FALSE(BFS->setCurrentWorkingDirectory(BDir.Path));
  ASSERT_FALSE(CFS->setCurrentWorkingDirectory(C.Path));
  EXPECT_EQ(BDir.Path, *BFS->getCurrentWorkingDirectory());
  EXPECT_EQ(C.Path, *CFS->getCurrentWorkingDirectory());

  // openFileForRead(), indirectly.
  auto BBuf = BFS->getBufferForFile("bb");
  ASSERT_TRUE(BBuf);
  EXPECT_EQ("bbbb", (*BBuf)->getBuffer());

  auto ABuf = CFS->getBufferForFile("aa");
  ASSERT_TRUE(ABuf);
  EXPECT_EQ("aaaa", (*ABuf)->getBuffer());

  // status()
  auto BStat = BFS->status("bb");
  ASSERT_TRUE(BStat);
  EXPECT_EQ("bb", BStat->getName());

  auto AStat = CFS->status("aa");
  ASSERT_TRUE(AStat);
  EXPECT_EQ("aa", AStat->getName()); // unresolved name

  // getRealPath()
  SmallString<128> BPath;
  ASSERT_FALSE(BFS->getRealPath("bb", BPath));
  EXPECT_EQ(BB.Path, BPath);

  SmallString<128> APath;
  ASSERT_FALSE(CFS->getRealPath("aa", APath));
  EXPECT_EQ(AA.Path, APath); // Reports resolved name.

  // dir_begin
  std::error_code EC;
  auto BIt = BFS->dir_begin(".", EC);
  ASSERT_FALSE(EC);
  ASSERT_NE(BIt, vfs::directory_iterator());
  EXPECT_EQ((BDir.Path + "/./bb").str(), BIt->path());
  BIt.increment(EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ(BIt, vfs::directory_iterator());

  auto CIt = CFS->dir_begin(".", EC);
  ASSERT_FALSE(EC);
  ASSERT_NE(CIt, vfs::directory_iterator());
  EXPECT_EQ((ADir.Path + "/./aa").str(), CIt->path()); // Partly resolved name!
  CIt.increment(EC); // Because likely to read through this path.
  ASSERT_FALSE(EC);
  ASSERT_EQ(CIt, vfs::directory_iterator());
}

TEST(VirtualFileSystemTest, BrokenSymlinkRealFSIteration) {
  ScopedDir TestDirectory("virtual-file-system-test", /*Unique*/ true);
  IntrusiveRefCntPtr<vfs::FileSystem> FS = vfs::getRealFileSystem();

  ScopedLink _a("no_such_file", TestDirectory + "/a");
  ScopedDir _b(TestDirectory + "/b");
  ScopedLink _c("no_such_file", TestDirectory + "/c");

  // Should get no iteration error, but a stat error for the broken symlinks.
  std::map<std::string, std::error_code> StatResults;
  std::error_code EC;
  for (vfs::directory_iterator I = FS->dir_begin(Twine(TestDirectory), EC), E;
       I != E; I.increment(EC)) {
    EXPECT_FALSE(EC);
    StatResults[sys::path::filename(I->path())] =
        FS->status(I->path()).getError();
  }
  EXPECT_THAT(
      StatResults,
      ElementsAre(
          Pair("a", std::make_error_code(std::errc::no_such_file_or_directory)),
          Pair("b", std::error_code()),
          Pair("c",
               std::make_error_code(std::errc::no_such_file_or_directory))));
}
#endif

TEST(VirtualFileSystemTest, BasicRealFSRecursiveIteration) {
  ScopedDir TestDirectory("virtual-file-system-test", /*Unique*/ true);
  IntrusiveRefCntPtr<vfs::FileSystem> FS = vfs::getRealFileSystem();

  std::error_code EC;
  auto I = vfs::recursive_directory_iterator(*FS, Twine(TestDirectory), EC);
  ASSERT_FALSE(EC);
  EXPECT_EQ(vfs::recursive_directory_iterator(), I); // empty directory is empty

  ScopedDir _a(TestDirectory + "/a");
  ScopedDir _ab(TestDirectory + "/a/b");
  ScopedDir _c(TestDirectory + "/c");
  ScopedDir _cd(TestDirectory + "/c/d");

  I = vfs::recursive_directory_iterator(*FS, Twine(TestDirectory), EC);
  ASSERT_FALSE(EC);
  ASSERT_NE(vfs::recursive_directory_iterator(), I);

  std::vector<std::string> Contents;
  for (auto E = vfs::recursive_directory_iterator(); !EC && I != E;
       I.increment(EC)) {
    Contents.push_back(I->path());
  }

  // Check contents, which may be in any order
  EXPECT_EQ(4U, Contents.size());
  int Counts[4] = {0, 0, 0, 0};
  for (const std::string &Name : Contents) {
    ASSERT_FALSE(Name.empty());
    int Index = Name[Name.size() - 1] - 'a';
    ASSERT_TRUE(Index >= 0 && Index < 4);
    Counts[Index]++;
  }
  EXPECT_EQ(1, Counts[0]); // a
  EXPECT_EQ(1, Counts[1]); // b
  EXPECT_EQ(1, Counts[2]); // c
  EXPECT_EQ(1, Counts[3]); // d
}

TEST(VirtualFileSystemTest, BasicRealFSRecursiveIterationNoPush) {
  ScopedDir TestDirectory("virtual-file-system-test", /*Unique*/ true);

  ScopedDir _a(TestDirectory + "/a");
  ScopedDir _ab(TestDirectory + "/a/b");
  ScopedDir _c(TestDirectory + "/c");
  ScopedDir _cd(TestDirectory + "/c/d");
  ScopedDir _e(TestDirectory + "/e");
  ScopedDir _ef(TestDirectory + "/e/f");
  ScopedDir _g(TestDirectory + "/g");

  IntrusiveRefCntPtr<vfs::FileSystem> FS = vfs::getRealFileSystem();

  // Test that calling no_push on entries without subdirectories has no effect.
  {
    std::error_code EC;
    auto I = vfs::recursive_directory_iterator(*FS, Twine(TestDirectory), EC);
    ASSERT_FALSE(EC);

    std::vector<std::string> Contents;
    for (auto E = vfs::recursive_directory_iterator(); !EC && I != E;
         I.increment(EC)) {
      Contents.push_back(I->path());
      char last = I->path().back();
      switch (last) {
      case 'b':
      case 'd':
      case 'f':
      case 'g':
        I.no_push();
        break;
      default:
        break;
      }
    }
    EXPECT_EQ(7U, Contents.size());
  }

  // Test that calling no_push skips subdirectories.
  {
    std::error_code EC;
    auto I = vfs::recursive_directory_iterator(*FS, Twine(TestDirectory), EC);
    ASSERT_FALSE(EC);

    std::vector<std::string> Contents;
    for (auto E = vfs::recursive_directory_iterator(); !EC && I != E;
         I.increment(EC)) {
      Contents.push_back(I->path());
      char last = I->path().back();
      switch (last) {
      case 'a':
      case 'c':
      case 'e':
        I.no_push();
        break;
      default:
        break;
      }
    }

    // Check contents, which may be in any order
    EXPECT_EQ(4U, Contents.size());
    int Counts[7] = {0, 0, 0, 0, 0, 0, 0};
    for (const std::string &Name : Contents) {
      ASSERT_FALSE(Name.empty());
      int Index = Name[Name.size() - 1] - 'a';
      ASSERT_TRUE(Index >= 0 && Index < 7);
      Counts[Index]++;
    }
    EXPECT_EQ(1, Counts[0]); // a
    EXPECT_EQ(0, Counts[1]); // b
    EXPECT_EQ(1, Counts[2]); // c
    EXPECT_EQ(0, Counts[3]); // d
    EXPECT_EQ(1, Counts[4]); // e
    EXPECT_EQ(0, Counts[5]); // f
    EXPECT_EQ(1, Counts[6]); // g
  }
}

#ifdef LLVM_ON_UNIX
TEST(VirtualFileSystemTest, BrokenSymlinkRealFSRecursiveIteration) {
  ScopedDir TestDirectory("virtual-file-system-test", /*Unique*/ true);
  IntrusiveRefCntPtr<vfs::FileSystem> FS = vfs::getRealFileSystem();

  ScopedLink _a("no_such_file", TestDirectory + "/a");
  ScopedDir _b(TestDirectory + "/b");
  ScopedLink _ba("no_such_file", TestDirectory + "/b/a");
  ScopedDir _bb(TestDirectory + "/b/b");
  ScopedLink _bc("no_such_file", TestDirectory + "/b/c");
  ScopedLink _c("no_such_file", TestDirectory + "/c");
  ScopedDir _d(TestDirectory + "/d");
  ScopedDir _dd(TestDirectory + "/d/d");
  ScopedDir _ddd(TestDirectory + "/d/d/d");
  ScopedLink _e("no_such_file", TestDirectory + "/e");

  std::vector<std::string> VisitedBrokenSymlinks;
  std::vector<std::string> VisitedNonBrokenSymlinks;
  std::error_code EC;
  for (vfs::recursive_directory_iterator I(*FS, Twine(TestDirectory), EC), E;
       I != E; I.increment(EC)) {
    EXPECT_FALSE(EC);
    (FS->status(I->path()) ? VisitedNonBrokenSymlinks : VisitedBrokenSymlinks)
        .push_back(I->path());
  }

  // Check visited file names.
  EXPECT_THAT(VisitedBrokenSymlinks,
              UnorderedElementsAre(StringRef(_a), StringRef(_ba),
                                   StringRef(_bc), StringRef(_c),
                                   StringRef(_e)));
  EXPECT_THAT(VisitedNonBrokenSymlinks,
              UnorderedElementsAre(StringRef(_b), StringRef(_bb), StringRef(_d),
                                   StringRef(_dd), StringRef(_ddd)));
}
#endif

template <typename DirIter>
static void checkContents(DirIter I, ArrayRef<StringRef> ExpectedOut) {
  std::error_code EC;
  SmallVector<StringRef, 4> Expected(ExpectedOut.begin(), ExpectedOut.end());
  SmallVector<std::string, 4> InputToCheck;

  // Do not rely on iteration order to check for contents, sort both
  // content vectors before comparison.
  for (DirIter E; !EC && I != E; I.increment(EC))
    InputToCheck.push_back(I->path());

  llvm::sort(InputToCheck);
  llvm::sort(Expected);
  EXPECT_EQ(InputToCheck.size(), Expected.size());

  unsigned LastElt = std::min(InputToCheck.size(), Expected.size());
  for (unsigned Idx = 0; Idx != LastElt; ++Idx)
    EXPECT_EQ(StringRef(InputToCheck[Idx]), Expected[Idx]);
}

TEST(VirtualFileSystemTest, OverlayIteration) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Upper);

  std::error_code EC;
  checkContents(O->dir_begin("/", EC), ArrayRef<StringRef>());

  Lower->addRegularFile("/file1");
  checkContents(O->dir_begin("/", EC), ArrayRef<StringRef>("/file1"));

  Upper->addRegularFile("/file2");
  checkContents(O->dir_begin("/", EC), {"/file2", "/file1"});

  Lower->addDirectory("/dir1");
  Lower->addRegularFile("/dir1/foo");
  Upper->addDirectory("/dir2");
  Upper->addRegularFile("/dir2/foo");
  checkContents(O->dir_begin("/dir2", EC), ArrayRef<StringRef>("/dir2/foo"));
  checkContents(O->dir_begin("/", EC), {"/dir2", "/file2", "/dir1", "/file1"});
}

TEST(VirtualFileSystemTest, OverlayRecursiveIteration) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Middle(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Middle);
  O->pushOverlay(Upper);

  std::error_code EC;
  checkContents(vfs::recursive_directory_iterator(*O, "/", EC),
                ArrayRef<StringRef>());

  Lower->addRegularFile("/file1");
  checkContents(vfs::recursive_directory_iterator(*O, "/", EC),
                ArrayRef<StringRef>("/file1"));

  Upper->addDirectory("/dir");
  Upper->addRegularFile("/dir/file2");
  checkContents(vfs::recursive_directory_iterator(*O, "/", EC),
                {"/dir", "/dir/file2", "/file1"});

  Lower->addDirectory("/dir1");
  Lower->addRegularFile("/dir1/foo");
  Lower->addDirectory("/dir1/a");
  Lower->addRegularFile("/dir1/a/b");
  Middle->addDirectory("/a");
  Middle->addDirectory("/a/b");
  Middle->addDirectory("/a/b/c");
  Middle->addRegularFile("/a/b/c/d");
  Middle->addRegularFile("/hiddenByUp");
  Upper->addDirectory("/dir2");
  Upper->addRegularFile("/dir2/foo");
  Upper->addRegularFile("/hiddenByUp");
  checkContents(vfs::recursive_directory_iterator(*O, "/dir2", EC),
                ArrayRef<StringRef>("/dir2/foo"));
  checkContents(vfs::recursive_directory_iterator(*O, "/", EC),
                {"/dir", "/dir/file2", "/dir2", "/dir2/foo", "/hiddenByUp",
                 "/a", "/a/b", "/a/b/c", "/a/b/c/d", "/dir1", "/dir1/a",
                 "/dir1/a/b", "/dir1/foo", "/file1"});
}

TEST(VirtualFileSystemTest, ThreeLevelIteration) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Middle(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Middle);
  O->pushOverlay(Upper);

  std::error_code EC;
  checkContents(O->dir_begin("/", EC), ArrayRef<StringRef>());

  Middle->addRegularFile("/file2");
  checkContents(O->dir_begin("/", EC), ArrayRef<StringRef>("/file2"));

  Lower->addRegularFile("/file1");
  Upper->addRegularFile("/file3");
  checkContents(O->dir_begin("/", EC), {"/file3", "/file2", "/file1"});
}

TEST(VirtualFileSystemTest, HiddenInIteration) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Middle(new DummyFileSystem());
  IntrusiveRefCntPtr<DummyFileSystem> Upper(new DummyFileSystem());
  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(Middle);
  O->pushOverlay(Upper);

  std::error_code EC;
  Lower->addRegularFile("/onlyInLow");
  Lower->addDirectory("/hiddenByMid");
  Lower->addDirectory("/hiddenByUp");
  Middle->addRegularFile("/onlyInMid");
  Middle->addRegularFile("/hiddenByMid");
  Middle->addDirectory("/hiddenByUp");
  Upper->addRegularFile("/onlyInUp");
  Upper->addRegularFile("/hiddenByUp");
  checkContents(
      O->dir_begin("/", EC),
      {"/hiddenByUp", "/onlyInUp", "/hiddenByMid", "/onlyInMid", "/onlyInLow"});

  // Make sure we get the top-most entry
  {
    std::error_code EC;
    vfs::directory_iterator I = O->dir_begin("/", EC), E;
    for (; !EC && I != E; I.increment(EC))
      if (I->path() == "/hiddenByUp")
        break;
    ASSERT_NE(E, I);
    EXPECT_EQ(sys::fs::file_type::regular_file, I->type());
  }
  {
    std::error_code EC;
    vfs::directory_iterator I = O->dir_begin("/", EC), E;
    for (; !EC && I != E; I.increment(EC))
      if (I->path() == "/hiddenByMid")
        break;
    ASSERT_NE(E, I);
    EXPECT_EQ(sys::fs::file_type::regular_file, I->type());
  }
}

TEST(ProxyFileSystemTest, Basic) {
  IntrusiveRefCntPtr<vfs::InMemoryFileSystem> Base(
      new vfs::InMemoryFileSystem());
  vfs::ProxyFileSystem PFS(Base);

  Base->addFile("/a", 0, MemoryBuffer::getMemBuffer("test"));

  auto Stat = PFS.status("/a");
  ASSERT_FALSE(Stat.getError());

  auto File = PFS.openFileForRead("/a");
  ASSERT_FALSE(File.getError());
  EXPECT_EQ("test", (*(*File)->getBuffer("ignored"))->getBuffer());

  std::error_code EC;
  vfs::directory_iterator I = PFS.dir_begin("/", EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ("/a", I->path());
  I.increment(EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ(vfs::directory_iterator(), I);

  ASSERT_FALSE(PFS.setCurrentWorkingDirectory("/"));

  auto PWD = PFS.getCurrentWorkingDirectory();
  ASSERT_FALSE(PWD.getError());
  ASSERT_EQ("/", *PWD);

  SmallString<16> Path;
  ASSERT_FALSE(PFS.getRealPath("a", Path));
  ASSERT_EQ("/a", Path);

  bool Local = true;
  ASSERT_FALSE(PFS.isLocal("/a", Local));
  EXPECT_FALSE(Local);
}

class InMemoryFileSystemTest : public ::testing::Test {
protected:
  llvm::vfs::InMemoryFileSystem FS;
  llvm::vfs::InMemoryFileSystem NormalizedFS;

  InMemoryFileSystemTest()
      : FS(/*UseNormalizedPaths=*/false),
        NormalizedFS(/*UseNormalizedPaths=*/true) {}
};

MATCHER_P2(IsHardLinkTo, FS, Target, "") {
  StringRef From = arg;
  StringRef To = Target;
  auto OpenedFrom = FS->openFileForRead(From);
  auto OpenedTo = FS->openFileForRead(To);
  return !OpenedFrom.getError() && !OpenedTo.getError() &&
         (*OpenedFrom)->status()->getUniqueID() ==
             (*OpenedTo)->status()->getUniqueID();
}

TEST_F(InMemoryFileSystemTest, IsEmpty) {
  auto Stat = FS.status("/a");
  ASSERT_EQ(Stat.getError(), errc::no_such_file_or_directory) << FS.toString();
  Stat = FS.status("/");
  ASSERT_EQ(Stat.getError(), errc::no_such_file_or_directory) << FS.toString();
}

TEST_F(InMemoryFileSystemTest, WindowsPath) {
  FS.addFile("c:/windows/system128/foo.cpp", 0, MemoryBuffer::getMemBuffer(""));
  auto Stat = FS.status("c:");
#if !defined(_WIN32)
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << FS.toString();
#endif
  Stat = FS.status("c:/windows/system128/foo.cpp");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << FS.toString();
  FS.addFile("d:/windows/foo.cpp", 0, MemoryBuffer::getMemBuffer(""));
  Stat = FS.status("d:/windows/foo.cpp");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << FS.toString();
}

TEST_F(InMemoryFileSystemTest, OverlayFile) {
  FS.addFile("/a", 0, MemoryBuffer::getMemBuffer("a"));
  NormalizedFS.addFile("/a", 0, MemoryBuffer::getMemBuffer("a"));
  auto Stat = FS.status("/");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << FS.toString();
  Stat = FS.status("/.");
  ASSERT_FALSE(Stat);
  Stat = NormalizedFS.status("/.");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << FS.toString();
  Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_EQ("/a", Stat->getName());
}

TEST_F(InMemoryFileSystemTest, OverlayFileNoOwn) {
  auto Buf = MemoryBuffer::getMemBuffer("a");
  FS.addFileNoOwn("/a", 0, Buf.get());
  auto Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_EQ("/a", Stat->getName());
}

TEST_F(InMemoryFileSystemTest, OpenFileForRead) {
  FS.addFile("/a", 0, MemoryBuffer::getMemBuffer("a"));
  FS.addFile("././c", 0, MemoryBuffer::getMemBuffer("c"));
  FS.addFile("./d/../d", 0, MemoryBuffer::getMemBuffer("d"));
  NormalizedFS.addFile("/a", 0, MemoryBuffer::getMemBuffer("a"));
  NormalizedFS.addFile("././c", 0, MemoryBuffer::getMemBuffer("c"));
  NormalizedFS.addFile("./d/../d", 0, MemoryBuffer::getMemBuffer("d"));
  auto File = FS.openFileForRead("/a");
  ASSERT_EQ("a", (*(*File)->getBuffer("ignored"))->getBuffer());
  File = FS.openFileForRead("/a"); // Open again.
  ASSERT_EQ("a", (*(*File)->getBuffer("ignored"))->getBuffer());
  File = NormalizedFS.openFileForRead("/././a"); // Open again.
  ASSERT_EQ("a", (*(*File)->getBuffer("ignored"))->getBuffer());
  File = FS.openFileForRead("/");
  ASSERT_EQ(File.getError(), errc::invalid_argument) << FS.toString();
  File = FS.openFileForRead("/b");
  ASSERT_EQ(File.getError(), errc::no_such_file_or_directory) << FS.toString();
  File = FS.openFileForRead("./c");
  ASSERT_FALSE(File);
  File = FS.openFileForRead("e/../d");
  ASSERT_FALSE(File);
  File = NormalizedFS.openFileForRead("./c");
  ASSERT_EQ("c", (*(*File)->getBuffer("ignored"))->getBuffer());
  File = NormalizedFS.openFileForRead("e/../d");
  ASSERT_EQ("d", (*(*File)->getBuffer("ignored"))->getBuffer());
}

TEST_F(InMemoryFileSystemTest, DuplicatedFile) {
  ASSERT_TRUE(FS.addFile("/a", 0, MemoryBuffer::getMemBuffer("a")));
  ASSERT_FALSE(FS.addFile("/a/b", 0, MemoryBuffer::getMemBuffer("a")));
  ASSERT_TRUE(FS.addFile("/a", 0, MemoryBuffer::getMemBuffer("a")));
  ASSERT_FALSE(FS.addFile("/a", 0, MemoryBuffer::getMemBuffer("b")));
}

TEST_F(InMemoryFileSystemTest, DirectoryIteration) {
  FS.addFile("/a", 0, MemoryBuffer::getMemBuffer(""));
  FS.addFile("/b/c", 0, MemoryBuffer::getMemBuffer(""));

  std::error_code EC;
  vfs::directory_iterator I = FS.dir_begin("/", EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ("/a", I->path());
  I.increment(EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ("/b", I->path());
  I.increment(EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ(vfs::directory_iterator(), I);

  I = FS.dir_begin("/b", EC);
  ASSERT_FALSE(EC);
  // When on Windows, we end up with "/b\\c" as the name.  Convert to Posix
  // path for the sake of the comparison.
  ASSERT_EQ("/b/c", getPosixPath(I->path()));
  I.increment(EC);
  ASSERT_FALSE(EC);
  ASSERT_EQ(vfs::directory_iterator(), I);
}

TEST_F(InMemoryFileSystemTest, WorkingDirectory) {
  FS.setCurrentWorkingDirectory("/b");
  FS.addFile("c", 0, MemoryBuffer::getMemBuffer(""));

  auto Stat = FS.status("/b/c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_EQ("/b/c", Stat->getName());
  ASSERT_EQ("/b", *FS.getCurrentWorkingDirectory());

  Stat = FS.status("c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();

  NormalizedFS.setCurrentWorkingDirectory("/b/c");
  NormalizedFS.setCurrentWorkingDirectory(".");
  ASSERT_EQ("/b/c",
            getPosixPath(NormalizedFS.getCurrentWorkingDirectory().get()));
  NormalizedFS.setCurrentWorkingDirectory("..");
  ASSERT_EQ("/b",
            getPosixPath(NormalizedFS.getCurrentWorkingDirectory().get()));
}

TEST_F(InMemoryFileSystemTest, IsLocal) {
  FS.setCurrentWorkingDirectory("/b");
  FS.addFile("c", 0, MemoryBuffer::getMemBuffer(""));

  std::error_code EC;
  bool IsLocal = true;
  EC = FS.isLocal("c", IsLocal);
  ASSERT_FALSE(EC);
  ASSERT_FALSE(IsLocal);
}

#if !defined(_WIN32)
TEST_F(InMemoryFileSystemTest, GetRealPath) {
  SmallString<16> Path;
  EXPECT_EQ(FS.getRealPath("b", Path), errc::operation_not_permitted);

  auto GetRealPath = [this](StringRef P) {
    SmallString<16> Output;
    auto EC = FS.getRealPath(P, Output);
    EXPECT_FALSE(EC);
    return Output.str().str();
  };

  FS.setCurrentWorkingDirectory("a");
  EXPECT_EQ(GetRealPath("b"), "a/b");
  EXPECT_EQ(GetRealPath("../b"), "b");
  EXPECT_EQ(GetRealPath("b/./c"), "a/b/c");

  FS.setCurrentWorkingDirectory("/a");
  EXPECT_EQ(GetRealPath("b"), "/a/b");
  EXPECT_EQ(GetRealPath("../b"), "/b");
  EXPECT_EQ(GetRealPath("b/./c"), "/a/b/c");
}
#endif // _WIN32

TEST_F(InMemoryFileSystemTest, AddFileWithUser) {
  FS.addFile("/a/b/c", 0, MemoryBuffer::getMemBuffer("abc"), 0xFEEDFACE);
  auto Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  ASSERT_EQ(0xFEEDFACE, Stat->getUser());
  Stat = FS.status("/a/b");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  ASSERT_EQ(0xFEEDFACE, Stat->getUser());
  Stat = FS.status("/a/b/c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isRegularFile());
  ASSERT_EQ(sys::fs::perms::all_all, Stat->getPermissions());
  ASSERT_EQ(0xFEEDFACE, Stat->getUser());
}

TEST_F(InMemoryFileSystemTest, AddFileWithGroup) {
  FS.addFile("/a/b/c", 0, MemoryBuffer::getMemBuffer("abc"), None, 0xDABBAD00);
  auto Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  ASSERT_EQ(0xDABBAD00, Stat->getGroup());
  Stat = FS.status("/a/b");
  ASSERT_TRUE(Stat->isDirectory());
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_EQ(0xDABBAD00, Stat->getGroup());
  Stat = FS.status("/a/b/c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isRegularFile());
  ASSERT_EQ(sys::fs::perms::all_all, Stat->getPermissions());
  ASSERT_EQ(0xDABBAD00, Stat->getGroup());
}

TEST_F(InMemoryFileSystemTest, AddFileWithFileType) {
  FS.addFile("/a/b/c", 0, MemoryBuffer::getMemBuffer("abc"), None, None,
             sys::fs::file_type::socket_file);
  auto Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  Stat = FS.status("/a/b");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  Stat = FS.status("/a/b/c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_EQ(sys::fs::file_type::socket_file, Stat->getType());
  ASSERT_EQ(sys::fs::perms::all_all, Stat->getPermissions());
}

TEST_F(InMemoryFileSystemTest, AddFileWithPerms) {
  FS.addFile("/a/b/c", 0, MemoryBuffer::getMemBuffer("abc"), None, None, None,
             sys::fs::perms::owner_read | sys::fs::perms::owner_write);
  auto Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  ASSERT_EQ(sys::fs::perms::owner_read | sys::fs::perms::owner_write |
                sys::fs::perms::owner_exe,
            Stat->getPermissions());
  Stat = FS.status("/a/b");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  ASSERT_EQ(sys::fs::perms::owner_read | sys::fs::perms::owner_write |
                sys::fs::perms::owner_exe,
            Stat->getPermissions());
  Stat = FS.status("/a/b/c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isRegularFile());
  ASSERT_EQ(sys::fs::perms::owner_read | sys::fs::perms::owner_write,
            Stat->getPermissions());
}

TEST_F(InMemoryFileSystemTest, AddDirectoryThenAddChild) {
  FS.addFile("/a", 0, MemoryBuffer::getMemBuffer(""), /*User=*/None,
             /*Group=*/None, sys::fs::file_type::directory_file);
  FS.addFile("/a/b", 0, MemoryBuffer::getMemBuffer("abc"), /*User=*/None,
             /*Group=*/None, sys::fs::file_type::regular_file);
  auto Stat = FS.status("/a");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isDirectory());
  Stat = FS.status("/a/b");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n" << FS.toString();
  ASSERT_TRUE(Stat->isRegularFile());
}

// Test that the name returned by status() is in the same form as the path that
// was requested (to match the behavior of RealFileSystem).
TEST_F(InMemoryFileSystemTest, StatusName) {
  NormalizedFS.addFile("/a/b/c", 0, MemoryBuffer::getMemBuffer("abc"),
                       /*User=*/None,
                       /*Group=*/None, sys::fs::file_type::regular_file);
  NormalizedFS.setCurrentWorkingDirectory("/a/b");

  // Access using InMemoryFileSystem::status.
  auto Stat = NormalizedFS.status("../b/c");
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n"
                                << NormalizedFS.toString();
  ASSERT_TRUE(Stat->isRegularFile());
  ASSERT_EQ("../b/c", Stat->getName());

  // Access using InMemoryFileAdaptor::status.
  auto File = NormalizedFS.openFileForRead("../b/c");
  ASSERT_FALSE(File.getError()) << File.getError() << "\n"
                                << NormalizedFS.toString();
  Stat = (*File)->status();
  ASSERT_FALSE(Stat.getError()) << Stat.getError() << "\n"
                                << NormalizedFS.toString();
  ASSERT_TRUE(Stat->isRegularFile());
  ASSERT_EQ("../b/c", Stat->getName());

  // Access using a directory iterator.
  std::error_code EC;
  llvm::vfs::directory_iterator It = NormalizedFS.dir_begin("../b", EC);
  // When on Windows, we end up with "../b\\c" as the name.  Convert to Posix
  // path for the sake of the comparison.
  ASSERT_EQ("../b/c", getPosixPath(It->path()));
}

TEST_F(InMemoryFileSystemTest, AddHardLinkToFile) {
  StringRef FromLink = "/path/to/FROM/link";
  StringRef Target = "/path/to/TO/file";
  FS.addFile(Target, 0, MemoryBuffer::getMemBuffer("content of target"));
  EXPECT_TRUE(FS.addHardLink(FromLink, Target));
  EXPECT_THAT(FromLink, IsHardLinkTo(&FS, Target));
  EXPECT_TRUE(FS.status(FromLink)->getSize() == FS.status(Target)->getSize());
  EXPECT_TRUE(FS.getBufferForFile(FromLink)->get()->getBuffer() ==
              FS.getBufferForFile(Target)->get()->getBuffer());
}

TEST_F(InMemoryFileSystemTest, AddHardLinkInChainPattern) {
  StringRef Link0 = "/path/to/0/link";
  StringRef Link1 = "/path/to/1/link";
  StringRef Link2 = "/path/to/2/link";
  StringRef Target = "/path/to/target";
  FS.addFile(Target, 0, MemoryBuffer::getMemBuffer("content of target file"));
  EXPECT_TRUE(FS.addHardLink(Link2, Target));
  EXPECT_TRUE(FS.addHardLink(Link1, Link2));
  EXPECT_TRUE(FS.addHardLink(Link0, Link1));
  EXPECT_THAT(Link0, IsHardLinkTo(&FS, Target));
  EXPECT_THAT(Link1, IsHardLinkTo(&FS, Target));
  EXPECT_THAT(Link2, IsHardLinkTo(&FS, Target));
}

TEST_F(InMemoryFileSystemTest, AddHardLinkToAFileThatWasNotAddedBefore) {
  EXPECT_FALSE(FS.addHardLink("/path/to/link", "/path/to/target"));
}

TEST_F(InMemoryFileSystemTest, AddHardLinkFromAFileThatWasAddedBefore) {
  StringRef Link = "/path/to/link";
  StringRef Target = "/path/to/target";
  FS.addFile(Target, 0, MemoryBuffer::getMemBuffer("content of target"));
  FS.addFile(Link, 0, MemoryBuffer::getMemBuffer("content of link"));
  EXPECT_FALSE(FS.addHardLink(Link, Target));
}

TEST_F(InMemoryFileSystemTest, AddSameHardLinkMoreThanOnce) {
  StringRef Link = "/path/to/link";
  StringRef Target = "/path/to/target";
  FS.addFile(Target, 0, MemoryBuffer::getMemBuffer("content of target"));
  EXPECT_TRUE(FS.addHardLink(Link, Target));
  EXPECT_FALSE(FS.addHardLink(Link, Target));
}

TEST_F(InMemoryFileSystemTest, AddFileInPlaceOfAHardLinkWithSameContent) {
  StringRef Link = "/path/to/link";
  StringRef Target = "/path/to/target";
  StringRef Content = "content of target";
  EXPECT_TRUE(FS.addFile(Target, 0, MemoryBuffer::getMemBuffer(Content)));
  EXPECT_TRUE(FS.addHardLink(Link, Target));
  EXPECT_TRUE(FS.addFile(Link, 0, MemoryBuffer::getMemBuffer(Content)));
}

TEST_F(InMemoryFileSystemTest, AddFileInPlaceOfAHardLinkWithDifferentContent) {
  StringRef Link = "/path/to/link";
  StringRef Target = "/path/to/target";
  StringRef Content = "content of target";
  StringRef LinkContent = "different content of link";
  EXPECT_TRUE(FS.addFile(Target, 0, MemoryBuffer::getMemBuffer(Content)));
  EXPECT_TRUE(FS.addHardLink(Link, Target));
  EXPECT_FALSE(FS.addFile(Link, 0, MemoryBuffer::getMemBuffer(LinkContent)));
}

TEST_F(InMemoryFileSystemTest, AddHardLinkToADirectory) {
  StringRef Dir = "path/to/dummy/dir";
  StringRef Link = "/path/to/link";
  StringRef File = "path/to/dummy/dir/target";
  StringRef Content = "content of target";
  EXPECT_TRUE(FS.addFile(File, 0, MemoryBuffer::getMemBuffer(Content)));
  EXPECT_FALSE(FS.addHardLink(Link, Dir));
}

TEST_F(InMemoryFileSystemTest, AddHardLinkFromADirectory) {
  StringRef Dir = "path/to/dummy/dir";
  StringRef Target = "path/to/dummy/dir/target";
  StringRef Content = "content of target";
  EXPECT_TRUE(FS.addFile(Target, 0, MemoryBuffer::getMemBuffer(Content)));
  EXPECT_FALSE(FS.addHardLink(Dir, Target));
}

TEST_F(InMemoryFileSystemTest, AddHardLinkUnderAFile) {
  StringRef CommonContent = "content string";
  FS.addFile("/a/b", 0, MemoryBuffer::getMemBuffer(CommonContent));
  FS.addFile("/c/d", 0, MemoryBuffer::getMemBuffer(CommonContent));
  EXPECT_FALSE(FS.addHardLink("/c/d/e", "/a/b"));
}

TEST_F(InMemoryFileSystemTest, RecursiveIterationWithHardLink) {
  std::error_code EC;
  FS.addFile("/a/b", 0, MemoryBuffer::getMemBuffer("content string"));
  EXPECT_TRUE(FS.addHardLink("/c/d", "/a/b"));
  auto I = vfs::recursive_directory_iterator(FS, "/", EC);
  ASSERT_FALSE(EC);
  std::vector<std::string> Nodes;
  for (auto E = vfs::recursive_directory_iterator(); !EC && I != E;
       I.increment(EC)) {
    Nodes.push_back(getPosixPath(I->path()));
  }
  EXPECT_THAT(Nodes, testing::UnorderedElementsAre("/a", "/a/b", "/c", "/c/d"));
}

// NOTE: in the tests below, we use '//root/' as our root directory, since it is
// a legal *absolute* path on Windows as well as *nix.
class VFSFromYAMLTest : public ::testing::Test {
public:
  int NumDiagnostics;

  void SetUp() override { NumDiagnostics = 0; }

  static void CountingDiagHandler(const SMDiagnostic &, void *Context) {
    VFSFromYAMLTest *Test = static_cast<VFSFromYAMLTest *>(Context);
    ++Test->NumDiagnostics;
  }

  IntrusiveRefCntPtr<vfs::FileSystem>
  getFromYAMLRawString(StringRef Content,
                       IntrusiveRefCntPtr<vfs::FileSystem> ExternalFS) {
    std::unique_ptr<MemoryBuffer> Buffer = MemoryBuffer::getMemBuffer(Content);
    return getVFSFromYAML(std::move(Buffer), CountingDiagHandler, "", this,
                          ExternalFS);
  }

  IntrusiveRefCntPtr<vfs::FileSystem> getFromYAMLString(
      StringRef Content,
      IntrusiveRefCntPtr<vfs::FileSystem> ExternalFS = new DummyFileSystem()) {
    std::string VersionPlusContent("{\n  'version':0,\n");
    VersionPlusContent += Content.slice(Content.find('{') + 1, StringRef::npos);
    return getFromYAMLRawString(VersionPlusContent, ExternalFS);
  }

  // This is intended as a "XFAIL" for windows hosts.
  bool supportsSameDirMultipleYAMLEntries() {
    Triple Host(Triple::normalize(sys::getProcessTriple()));
    return !Host.isOSWindows();
  }
};

TEST_F(VFSFromYAMLTest, BasicVFSFromYAML) {
  IntrusiveRefCntPtr<vfs::FileSystem> FS;
  FS = getFromYAMLString("");
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString("[]");
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString("'string'");
  EXPECT_EQ(nullptr, FS.get());
  EXPECT_EQ(3, NumDiagnostics);
}

TEST_F(VFSFromYAMLTest, MappedFiles) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("//root/foo/bar/a");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'file1',\n"
      "                  'external-contents': '//root/foo/bar/a'\n"
      "                },\n"
      "                {\n"
      "                  'type': 'file',\n"
      "                  'name': 'file2',\n"
      "                  'external-contents': '//root/foo/b'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(FS);

  // file
  ErrorOr<vfs::Status> S = O->status("//root/file1");
  ASSERT_FALSE(S.getError());
  EXPECT_EQ("//root/foo/bar/a", S->getName());
  EXPECT_TRUE(S->IsVFSMapped);

  ErrorOr<vfs::Status> SLower = O->status("//root/foo/bar/a");
  EXPECT_EQ("//root/foo/bar/a", SLower->getName());
  EXPECT_TRUE(S->equivalent(*SLower));
  EXPECT_FALSE(SLower->IsVFSMapped);

  // file after opening
  auto OpenedF = O->openFileForRead("//root/file1");
  ASSERT_FALSE(OpenedF.getError());
  auto OpenedS = (*OpenedF)->status();
  ASSERT_FALSE(OpenedS.getError());
  EXPECT_EQ("//root/foo/bar/a", OpenedS->getName());
  EXPECT_TRUE(OpenedS->IsVFSMapped);

  // directory
  S = O->status("//root/");
  ASSERT_FALSE(S.getError());
  EXPECT_TRUE(S->isDirectory());
  EXPECT_TRUE(S->equivalent(*O->status("//root/"))); // non-volatile UniqueID

  // broken mapping
  EXPECT_EQ(O->status("//root/file2").getError(),
            llvm::errc::no_such_file_or_directory);
  EXPECT_EQ(0, NumDiagnostics);
}

TEST_F(VFSFromYAMLTest, CaseInsensitive) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("//root/foo/bar/a");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'case-sensitive': 'false',\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'XX',\n"
      "                  'external-contents': '//root/foo/bar/a'\n"
      "                }\n"
      "              ]\n"
      "}]}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(FS);

  ErrorOr<vfs::Status> S = O->status("//root/XX");
  ASSERT_FALSE(S.getError());

  ErrorOr<vfs::Status> SS = O->status("//root/xx");
  ASSERT_FALSE(SS.getError());
  EXPECT_TRUE(S->equivalent(*SS));
  SS = O->status("//root/xX");
  EXPECT_TRUE(S->equivalent(*SS));
  SS = O->status("//root/Xx");
  EXPECT_TRUE(S->equivalent(*SS));
  EXPECT_EQ(0, NumDiagnostics);
}

TEST_F(VFSFromYAMLTest, CaseSensitive) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("//root/foo/bar/a");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'case-sensitive': 'true',\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'XX',\n"
      "                  'external-contents': '//root/foo/bar/a'\n"
      "                }\n"
      "              ]\n"
      "}]}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(FS);

  ErrorOr<vfs::Status> SS = O->status("//root/xx");
  EXPECT_EQ(SS.getError(), llvm::errc::no_such_file_or_directory);
  SS = O->status("//root/xX");
  EXPECT_EQ(SS.getError(), llvm::errc::no_such_file_or_directory);
  SS = O->status("//root/Xx");
  EXPECT_EQ(SS.getError(), llvm::errc::no_such_file_or_directory);
  EXPECT_EQ(0, NumDiagnostics);
}

TEST_F(VFSFromYAMLTest, IllegalVFSFile) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());

  // invalid YAML at top-level
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString("{]", Lower);
  EXPECT_EQ(nullptr, FS.get());
  // invalid YAML in roots
  FS = getFromYAMLString("{ 'roots':[}", Lower);
  // invalid YAML in directory
  FS = getFromYAMLString(
      "{ 'roots':[ { 'name': 'foo', 'type': 'directory', 'contents': [}",
      Lower);
  EXPECT_EQ(nullptr, FS.get());

  // invalid configuration
  FS = getFromYAMLString("{ 'knobular': 'true', 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString("{ 'case-sensitive': 'maybe', 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());

  // invalid roots
  FS = getFromYAMLString("{ 'roots':'' }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString("{ 'roots':{} }", Lower);
  EXPECT_EQ(nullptr, FS.get());

  // invalid entries
  FS = getFromYAMLString(
      "{ 'roots':[ { 'type': 'other', 'name': 'me', 'contents': '' }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString("{ 'roots':[ { 'type': 'file', 'name': [], "
                         "'external-contents': 'other' }",
                         Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'roots':[ { 'type': 'file', 'name': 'me', 'external-contents': [] }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'roots':[ { 'type': 'file', 'name': 'me', 'external-contents': {} }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'roots':[ { 'type': 'directory', 'name': 'me', 'contents': {} }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'roots':[ { 'type': 'directory', 'name': 'me', 'contents': '' }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'roots':[ { 'thingy': 'directory', 'name': 'me', 'contents': [] }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());

  // missing mandatory fields
  FS = getFromYAMLString("{ 'roots':[ { 'type': 'file', 'name': 'me' }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'roots':[ { 'type': 'file', 'external-contents': 'other' }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString("{ 'roots':[ { 'name': 'me', 'contents': [] }", Lower);
  EXPECT_EQ(nullptr, FS.get());

  // duplicate keys
  FS = getFromYAMLString("{ 'roots':[], 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLString(
      "{ 'case-sensitive':'true', 'case-sensitive':'true', 'roots':[] }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS =
      getFromYAMLString("{ 'roots':[{'name':'me', 'name':'you', 'type':'file', "
                        "'external-contents':'blah' } ] }",
                        Lower);
  EXPECT_EQ(nullptr, FS.get());

  // missing version
  FS = getFromYAMLRawString("{ 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());

  // bad version number
  FS = getFromYAMLRawString("{ 'version':'foo', 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLRawString("{ 'version':-1, 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  FS = getFromYAMLRawString("{ 'version':100000, 'roots':[] }", Lower);
  EXPECT_EQ(nullptr, FS.get());
  EXPECT_EQ(24, NumDiagnostics);
}

TEST_F(VFSFromYAMLTest, UseExternalName) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("//root/external/file");

  IntrusiveRefCntPtr<vfs::FileSystem> FS =
      getFromYAMLString("{ 'roots': [\n"
                        "  { 'type': 'file', 'name': '//root/A',\n"
                        "    'external-contents': '//root/external/file'\n"
                        "  },\n"
                        "  { 'type': 'file', 'name': '//root/B',\n"
                        "    'use-external-name': true,\n"
                        "    'external-contents': '//root/external/file'\n"
                        "  },\n"
                        "  { 'type': 'file', 'name': '//root/C',\n"
                        "    'use-external-name': false,\n"
                        "    'external-contents': '//root/external/file'\n"
                        "  }\n"
                        "] }",
                        Lower);
  ASSERT_TRUE(nullptr != FS.get());

  // default true
  EXPECT_EQ("//root/external/file", FS->status("//root/A")->getName());
  // explicit
  EXPECT_EQ("//root/external/file", FS->status("//root/B")->getName());
  EXPECT_EQ("//root/C", FS->status("//root/C")->getName());

  // global configuration
  FS = getFromYAMLString("{ 'use-external-names': false,\n"
                         "  'roots': [\n"
                         "  { 'type': 'file', 'name': '//root/A',\n"
                         "    'external-contents': '//root/external/file'\n"
                         "  },\n"
                         "  { 'type': 'file', 'name': '//root/B',\n"
                         "    'use-external-name': true,\n"
                         "    'external-contents': '//root/external/file'\n"
                         "  },\n"
                         "  { 'type': 'file', 'name': '//root/C',\n"
                         "    'use-external-name': false,\n"
                         "    'external-contents': '//root/external/file'\n"
                         "  }\n"
                         "] }",
                         Lower);
  ASSERT_TRUE(nullptr != FS.get());

  // default
  EXPECT_EQ("//root/A", FS->status("//root/A")->getName());
  // explicit
  EXPECT_EQ("//root/external/file", FS->status("//root/B")->getName());
  EXPECT_EQ("//root/C", FS->status("//root/C")->getName());
}

TEST_F(VFSFromYAMLTest, MultiComponentPath) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("//root/other");

  // file in roots
  IntrusiveRefCntPtr<vfs::FileSystem> FS =
      getFromYAMLString("{ 'roots': [\n"
                        "  { 'type': 'file', 'name': '//root/path/to/file',\n"
                        "    'external-contents': '//root/other' }]\n"
                        "}",
                        Lower);
  ASSERT_TRUE(nullptr != FS.get());
  EXPECT_FALSE(FS->status("//root/path/to/file").getError());
  EXPECT_FALSE(FS->status("//root/path/to").getError());
  EXPECT_FALSE(FS->status("//root/path").getError());
  EXPECT_FALSE(FS->status("//root/").getError());

  // at the start
  FS = getFromYAMLString(
      "{ 'roots': [\n"
      "  { 'type': 'directory', 'name': '//root/path/to',\n"
      "    'contents': [ { 'type': 'file', 'name': 'file',\n"
      "                    'external-contents': '//root/other' }]}]\n"
      "}",
      Lower);
  ASSERT_TRUE(nullptr != FS.get());
  EXPECT_FALSE(FS->status("//root/path/to/file").getError());
  EXPECT_FALSE(FS->status("//root/path/to").getError());
  EXPECT_FALSE(FS->status("//root/path").getError());
  EXPECT_FALSE(FS->status("//root/").getError());

  // at the end
  FS = getFromYAMLString(
      "{ 'roots': [\n"
      "  { 'type': 'directory', 'name': '//root/',\n"
      "    'contents': [ { 'type': 'file', 'name': 'path/to/file',\n"
      "                    'external-contents': '//root/other' }]}]\n"
      "}",
      Lower);
  ASSERT_TRUE(nullptr != FS.get());
  EXPECT_FALSE(FS->status("//root/path/to/file").getError());
  EXPECT_FALSE(FS->status("//root/path/to").getError());
  EXPECT_FALSE(FS->status("//root/path").getError());
  EXPECT_FALSE(FS->status("//root/").getError());
}

TEST_F(VFSFromYAMLTest, TrailingSlashes) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addRegularFile("//root/other");

  // file in roots
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'roots': [\n"
      "  { 'type': 'directory', 'name': '//root/path/to////',\n"
      "    'contents': [ { 'type': 'file', 'name': 'file',\n"
      "                    'external-contents': '//root/other' }]}]\n"
      "}",
      Lower);
  ASSERT_TRUE(nullptr != FS.get());
  EXPECT_FALSE(FS->status("//root/path/to/file").getError());
  EXPECT_FALSE(FS->status("//root/path/to").getError());
  EXPECT_FALSE(FS->status("//root/path").getError());
  EXPECT_FALSE(FS->status("//root/").getError());
}

TEST_F(VFSFromYAMLTest, DirectoryIteration) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addDirectory("//root/foo");
  Lower->addDirectory("//root/foo/bar");
  Lower->addRegularFile("//root/foo/bar/a");
  Lower->addRegularFile("//root/foo/bar/b");
  Lower->addRegularFile("//root/file3");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'file1',\n"
      "                  'external-contents': '//root/foo/bar/a'\n"
      "                },\n"
      "                {\n"
      "                  'type': 'file',\n"
      "                  'name': 'file2',\n"
      "                  'external-contents': '//root/foo/bar/b'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(FS);

  std::error_code EC;
  checkContents(O->dir_begin("//root/", EC),
                {"//root/file1", "//root/file2", "//root/file3", "//root/foo"});

  checkContents(O->dir_begin("//root/foo/bar", EC),
                {"//root/foo/bar/a", "//root/foo/bar/b"});
}

TEST_F(VFSFromYAMLTest, DirectoryIterationSameDirMultipleEntries) {
  // https://llvm.org/bugs/show_bug.cgi?id=27725
  if (!supportsSameDirMultipleYAMLEntries())
    return;

  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/zab");
  Lower->addDirectory("//root/baz");
  Lower->addRegularFile("//root/zab/a");
  Lower->addRegularFile("//root/zab/b");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/baz/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'x',\n"
      "                  'external-contents': '//root/zab/a'\n"
      "                }\n"
      "              ]\n"
      "},\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/baz/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'y',\n"
      "                  'external-contents': '//root/zab/b'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(FS);

  std::error_code EC;

  checkContents(O->dir_begin("//root/baz/", EC),
                {"//root/baz/x", "//root/baz/y"});
}

TEST_F(VFSFromYAMLTest, RecursiveDirectoryIterationLevel) {

  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/a");
  Lower->addDirectory("//root/a/b");
  Lower->addDirectory("//root/a/b/c");
  Lower->addRegularFile("//root/a/b/c/file");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/a/b/c/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'file',\n"
      "                  'external-contents': '//root/a/b/c/file'\n"
      "                }\n"
      "              ]\n"
      "},\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  IntrusiveRefCntPtr<vfs::OverlayFileSystem> O(
      new vfs::OverlayFileSystem(Lower));
  O->pushOverlay(FS);

  std::error_code EC;

  // Test recursive_directory_iterator level()
  vfs::recursive_directory_iterator I = vfs::recursive_directory_iterator(
                                        *O, "//root", EC),
                                    E;
  ASSERT_FALSE(EC);
  for (int l = 0; I != E; I.increment(EC), ++l) {
    ASSERT_FALSE(EC);
    EXPECT_EQ(I.level(), l);
  }
  EXPECT_EQ(I, E);
}

TEST_F(VFSFromYAMLTest, RelativePaths) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  // Filename at root level without a parent directory.
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'roots': [\n"
      "  { 'type': 'file', 'name': 'file-not-in-directory.h',\n"
      "    'external-contents': '//root/external/file'\n"
      "  }\n"
      "] }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());

  // Relative file path.
  FS = getFromYAMLString("{ 'roots': [\n"
                         "  { 'type': 'file', 'name': 'relative/file/path.h',\n"
                         "    'external-contents': '//root/external/file'\n"
                         "  }\n"
                         "] }",
                         Lower);
  EXPECT_EQ(nullptr, FS.get());

  // Relative directory path.
  FS = getFromYAMLString(
      "{ 'roots': [\n"
      "  { 'type': 'directory', 'name': 'relative/directory/path.h',\n"
      "    'contents': []\n"
      "  }\n"
      "] }",
      Lower);
  EXPECT_EQ(nullptr, FS.get());

  EXPECT_EQ(3, NumDiagnostics);
}

TEST_F(VFSFromYAMLTest, NonFallthroughDirectoryIteration) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addRegularFile("//root/a");
  Lower->addRegularFile("//root/b");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'fallthrough': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'c',\n"
      "                  'external-contents': '//root/a'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  std::error_code EC;
  checkContents(FS->dir_begin("//root/", EC),
                {"//root/c"});
}

TEST_F(VFSFromYAMLTest, DirectoryIterationWithDuplicates) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addRegularFile("//root/a");
  Lower->addRegularFile("//root/b");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'a',\n"
      "                  'external-contents': '//root/a'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
	  Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  std::error_code EC;
  checkContents(FS->dir_begin("//root/", EC),
                {"//root/a", "//root/b"});
}

TEST_F(VFSFromYAMLTest, DirectoryIterationErrorInVFSLayer) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addDirectory("//root/foo");
  Lower->addRegularFile("//root/foo/a");
  Lower->addRegularFile("//root/foo/b");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'bar/a',\n"
      "                  'external-contents': '//root/foo/a'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  std::error_code EC;
  checkContents(FS->dir_begin("//root/foo", EC),
                {"//root/foo/a", "//root/foo/b"});
}

TEST_F(VFSFromYAMLTest, GetRealPath) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//dir/");
  Lower->addRegularFile("/foo");
  Lower->addSymlink("/link");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'bar',\n"
      "                  'external-contents': '/link'\n"
      "                }\n"
      "              ]\n"
      "},\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//dir/',\n"
      "  'contents': []\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  // Regular file present in underlying file system.
  SmallString<16> RealPath;
  EXPECT_FALSE(FS->getRealPath("/foo", RealPath));
  EXPECT_EQ(RealPath.str(), "/foo");

  // File present in YAML pointing to symlink in underlying file system.
  EXPECT_FALSE(FS->getRealPath("//root/bar", RealPath));
  EXPECT_EQ(RealPath.str(), "/symlink");

  // Directories should fall back to the underlying file system is possible.
  EXPECT_FALSE(FS->getRealPath("//dir/", RealPath));
  EXPECT_EQ(RealPath.str(), "//dir/");

  // Try a non-existing file.
  EXPECT_EQ(FS->getRealPath("/non_existing", RealPath),
            errc::no_such_file_or_directory);
}

TEST_F(VFSFromYAMLTest, WorkingDirectory) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addDirectory("//root/foo");
  Lower->addRegularFile("//root/foo/a");
  Lower->addRegularFile("//root/foo/b");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/bar',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'a',\n"
      "                  'external-contents': '//root/foo/a'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);
  std::error_code EC = FS->setCurrentWorkingDirectory("//root/bar");
  ASSERT_FALSE(EC);

  llvm::ErrorOr<std::string> WorkingDir = FS->getCurrentWorkingDirectory();
  ASSERT_TRUE(WorkingDir);
  EXPECT_EQ(*WorkingDir, "//root/bar");

  llvm::ErrorOr<vfs::Status> Status = FS->status("./a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->isStatusKnown());
  EXPECT_FALSE(Status->isDirectory());
  EXPECT_TRUE(Status->isRegularFile());
  EXPECT_FALSE(Status->isSymlink());
  EXPECT_FALSE(Status->isOther());
  EXPECT_TRUE(Status->exists());

  EC = FS->setCurrentWorkingDirectory("bogus");
  ASSERT_TRUE(EC);
  WorkingDir = FS->getCurrentWorkingDirectory();
  ASSERT_TRUE(WorkingDir);
  EXPECT_EQ(*WorkingDir, "//root/bar");

  EC = FS->setCurrentWorkingDirectory("//root/");
  ASSERT_FALSE(EC);
  WorkingDir = FS->getCurrentWorkingDirectory();
  ASSERT_TRUE(WorkingDir);
  EXPECT_EQ(*WorkingDir, "//root/");

  EC = FS->setCurrentWorkingDirectory("bar");
  ASSERT_FALSE(EC);
  WorkingDir = FS->getCurrentWorkingDirectory();
  ASSERT_TRUE(WorkingDir);
  EXPECT_EQ(*WorkingDir, "//root/bar");
}

TEST_F(VFSFromYAMLTest, WorkingDirectoryFallthrough) {
  IntrusiveRefCntPtr<DummyFileSystem> Lower(new DummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addDirectory("//root/foo");
  Lower->addRegularFile("//root/foo/a");
  Lower->addRegularFile("//root/foo/b");
  Lower->addRegularFile("//root/c");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/bar',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'a',\n"
      "                  'external-contents': '//root/foo/a'\n"
      "                }\n"
      "              ]\n"
      "},\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/bar/baz',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'a',\n"
      "                  'external-contents': '//root/foo/a'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);
  std::error_code EC = FS->setCurrentWorkingDirectory("//root/");
  ASSERT_FALSE(EC);
  ASSERT_TRUE(FS.get() != nullptr);

  llvm::ErrorOr<vfs::Status> Status = FS->status("bar/a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

  Status = FS->status("foo/a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

  EC = FS->setCurrentWorkingDirectory("//root/bar");
  ASSERT_FALSE(EC);

  Status = FS->status("./a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

  Status = FS->status("./b");
  ASSERT_TRUE(Status.getError());

  Status = FS->status("./c");
  ASSERT_TRUE(Status.getError());

  EC = FS->setCurrentWorkingDirectory("//root/");
  ASSERT_FALSE(EC);

  Status = FS->status("c");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

  Status = FS->status("./bar/baz/a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

  EC = FS->setCurrentWorkingDirectory("//root/bar");
  ASSERT_FALSE(EC);

  Status = FS->status("./baz/a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

#if !defined(_WIN32)
  Status = FS->status("../bar/baz/a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());
#endif
}

TEST_F(VFSFromYAMLTest, WorkingDirectoryFallthroughInvalid) {
  IntrusiveRefCntPtr<ErrorDummyFileSystem> Lower(new ErrorDummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addDirectory("//root/foo");
  Lower->addRegularFile("//root/foo/a");
  Lower->addRegularFile("//root/foo/b");
  Lower->addRegularFile("//root/c");
  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLString(
      "{ 'use-external-names': false,\n"
      "  'roots': [\n"
      "{\n"
      "  'type': 'directory',\n"
      "  'name': '//root/bar',\n"
      "  'contents': [ {\n"
      "                  'type': 'file',\n"
      "                  'name': 'a',\n"
      "                  'external-contents': '//root/foo/a'\n"
      "                }\n"
      "              ]\n"
      "}\n"
      "]\n"
      "}",
      Lower);
  ASSERT_TRUE(FS.get() != nullptr);
  std::error_code EC = FS->setCurrentWorkingDirectory("//root/");
  ASSERT_FALSE(EC);
  ASSERT_TRUE(FS.get() != nullptr);

  llvm::ErrorOr<vfs::Status> Status = FS->status("bar/a");
  ASSERT_FALSE(Status.getError());
  EXPECT_TRUE(Status->exists());

  Status = FS->status("foo/a");
  ASSERT_TRUE(Status.getError());
}

TEST_F(VFSFromYAMLTest, YAMLVFSWriterTest) {
  ScopedDir TestDirectory("virtual-file-system-test", /*Unique*/ true);
  ScopedDir _a(TestDirectory + "/a");
  ScopedFile _ab(TestDirectory + "/a/b", "");
  ScopedDir _c(TestDirectory + "/c");
  ScopedFile _cd(TestDirectory + "/c/d", "");
  ScopedDir _e(TestDirectory + "/e");
  ScopedDir _ef(TestDirectory + "/e/f");
  ScopedDir _g(TestDirectory + "/g");
  ScopedFile _h(TestDirectory + "/h", "");

  vfs::YAMLVFSWriter VFSWriter;
  VFSWriter.addDirectoryMapping(_a.Path, "//root/a");
  VFSWriter.addFileMapping(_ab.Path, "//root/a/b");
  VFSWriter.addFileMapping(_cd.Path, "//root/c/d");
  VFSWriter.addDirectoryMapping(_e.Path, "//root/e");
  VFSWriter.addDirectoryMapping(_ef.Path, "//root/e/f");
  VFSWriter.addFileMapping(_g.Path, "//root/g");
  VFSWriter.addDirectoryMapping(_h.Path, "//root/h");

  std::string Buffer;
  raw_string_ostream OS(Buffer);
  VFSWriter.write(OS);
  OS.flush();

  IntrusiveRefCntPtr<ErrorDummyFileSystem> Lower(new ErrorDummyFileSystem());
  Lower->addDirectory("//root/");
  Lower->addDirectory("//root/a");
  Lower->addRegularFile("//root/a/b");
  Lower->addDirectory("//root/b");
  Lower->addDirectory("//root/c");
  Lower->addRegularFile("//root/c/d");
  Lower->addDirectory("//root/e");
  Lower->addDirectory("//root/e/f");
  Lower->addDirectory("//root/g");
  Lower->addRegularFile("//root/h");

  IntrusiveRefCntPtr<vfs::FileSystem> FS = getFromYAMLRawString(Buffer, Lower);
  ASSERT_TRUE(FS.get() != nullptr);

  EXPECT_TRUE(FS->exists(_a.Path));
  EXPECT_TRUE(FS->exists(_ab.Path));
  EXPECT_TRUE(FS->exists(_c.Path));
  EXPECT_TRUE(FS->exists(_cd.Path));
  EXPECT_TRUE(FS->exists(_e.Path));
  EXPECT_TRUE(FS->exists(_ef.Path));
  EXPECT_TRUE(FS->exists(_g.Path));
  EXPECT_TRUE(FS->exists(_h.Path));
}
