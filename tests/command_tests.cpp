#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"
#include "test_support.hpp"

namespace {

using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::StagingIndex;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

constexpr const char* kHelloBlob = "ce013625030ba8dba906f756967f9e9ca394464a";

struct CliResult {
  int code;
  std::string out;
  std::string err;
};

CliResult run(const std::vector<std::string>& args) {
  std::ostringstream out, err;
  const int code = minigit::run_cli(args, out, err);
  return {code, out.str(), err.str()};
}

// A fresh repository that is also the current directory.
class CommandTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ScopedCwd cwd{tmp.path()};
  ObjectStore store{repo.objects_dir()};
};

// --- hash-object ----------------------------------------------------------------

TEST_F(CommandTest, HashObjectPrintsIdWithoutWriting) {
  write_text(tmp.path() / "hello.txt", "hello\n");
  const auto r = run({"hash-object", "hello.txt"});
  EXPECT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_EQ(r.out, std::string(kHelloBlob) + "\n");
  EXPECT_FALSE(store.contains(kHelloBlob));
}

TEST_F(CommandTest, HashObjectWriteStoresBlob) {
  write_text(tmp.path() / "hello.txt", "hello\n");
  const auto r = run({"hash-object", "-w", "hello.txt"});
  EXPECT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_EQ(r.out, std::string(kHelloBlob) + "\n");
  EXPECT_EQ(store.read(kHelloBlob).content, "hello\n");
}

TEST_F(CommandTest, HashObjectMatchesGitForBinaryFile) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  std::string binary;
  for (int i = 0; i < 3000; ++i) binary.push_back(static_cast<char>(i * 7 % 256));
  write_text(tmp.path() / "data.bin", binary);
  const auto expected = git_in(tmp.path(), "hash-object data.bin");
  ASSERT_EQ(expected.status, 0);
  EXPECT_EQ(run({"hash-object", "data.bin"}).out, expected.output + "\n");
}

TEST_F(CommandTest, HashObjectErrors) {
  EXPECT_EQ(run({"hash-object"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"hash-object", "a", "b"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"hash-object", "--stdin"}).code, minigit::kExitUsage);
  const auto missing = run({"hash-object", "nope.txt"});
  EXPECT_EQ(missing.code, minigit::kExitError);
  EXPECT_NE(missing.err.find("nope.txt"), std::string::npos);
}

TEST(HashObjectCommand, WriteOutsideRepositoryFails) {
  TempDir tmp;
  if (fs::exists(tmp.path().parent_path() / ".git")) GTEST_SKIP() << "temp dir inside a repo";
  ScopedCwd cwd(tmp.path());
  write_text(tmp.path() / "f", "x");
  EXPECT_EQ(run({"hash-object", "f"}).code, minigit::kExitOk);
  const auto r = run({"hash-object", "-w", "f"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a minigit repository"), std::string::npos);
}

// --- cat-file -------------------------------------------------------------------

TEST_F(CommandTest, CatFileBlob) {
  store.write(ObjectType::Blob, "hello\n");
  EXPECT_EQ(run({"cat-file", "-t", kHelloBlob}).out, "blob\n");
  EXPECT_EQ(run({"cat-file", "-s", kHelloBlob}).out, "6\n");
  EXPECT_EQ(run({"cat-file", "-p", kHelloBlob}).out, "hello\n");
}

TEST_F(CommandTest, CatFileTreeMatchesGit) {
  const std::string blob = store.write(ObjectType::Blob, "hello\n");
  const std::string sub = store.write(ObjectType::Tree, minigit::serialize_tree({{"100644", "x", blob}}));
  const std::string tree = store.write(
      ObjectType::Tree, minigit::serialize_tree({{"100755", "run", blob}, {"40000", "dir", sub}}));

  const auto r = run({"cat-file", "-p", tree});
  ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_EQ(r.out, "040000 tree " + sub + "\tdir\n100755 blob " + blob + "\trun\n");
  EXPECT_EQ(run({"cat-file", "-t", tree}).out, "tree\n");

  if (!git_available()) GTEST_SKIP() << "git not installed";
  const auto expected = git_in(tmp.path(), "cat-file -p " + tree);
  ASSERT_EQ(expected.status, 0);
  EXPECT_EQ(r.out, expected.output + "\n");
}

TEST_F(CommandTest, CatFileCommitIsPrintedVerbatim) {
  const std::string body =
      "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
      "author A <a@x> 1 +0000\ncommitter A <a@x> 1 +0000\n\nmsg\n";
  const std::string oid = store.write(ObjectType::Commit, body);
  EXPECT_EQ(run({"cat-file", "-t", oid}).out, "commit\n");
  EXPECT_EQ(run({"cat-file", "-p", oid}).out, body);
}

TEST_F(CommandTest, CatFileUsageErrors) {
  EXPECT_EQ(run({"cat-file"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"cat-file", kHelloBlob}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"cat-file", "-x", kHelloBlob}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"cat-file", "-p", kHelloBlob, "extra"}).code, minigit::kExitUsage);
}

TEST_F(CommandTest, CatFileReportsInvalidAndMissingObjects) {
  const auto bad = run({"cat-file", "-p", "ce0136"});
  EXPECT_EQ(bad.code, minigit::kExitError);
  EXPECT_NE(bad.err.find("not a valid object name"), std::string::npos);

  const auto missing = run({"cat-file", "-t", kHelloBlob});
  EXPECT_EQ(missing.code, minigit::kExitError);
  EXPECT_NE(missing.err.find("not found"), std::string::npos);
}

TEST_F(CommandTest, CatFileReportsMalformedObjects) {
  // Corrupt zlib data.
  const std::string oid = store.write(ObjectType::Blob, "hello\n");
  write_text(store.object_path(oid), "not zlib");
  const auto corrupt = run({"cat-file", "-p", oid});
  EXPECT_EQ(corrupt.code, minigit::kExitError);
  EXPECT_NE(corrupt.err.find("corrupt"), std::string::npos);

  // A tree object whose content is not a valid tree.
  const std::string bad_tree = store.write(ObjectType::Tree, "garbage");
  const auto r = run({"cat-file", "-p", bad_tree});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("fatal: tree entry"), std::string::npos);
  // -t and -s only need the header, so they still work.
  EXPECT_EQ(run({"cat-file", "-s", bad_tree}).out, "7\n");
}

// --- add ------------------------------------------------------------------------

class AddTest : public CommandTest {
 protected:
  StagingIndex index() const { return StagingIndex::load(repo.index_path()); }

  std::vector<std::string> staged_paths() const {
    const StagingIndex staged = index();
    std::vector<std::string> paths;
    for (const auto& [path, entry] : staged.entries()) paths.push_back(path);
    return paths;
  }
};

TEST_F(AddTest, StagesSingleFileAndWritesBlob) {
  write_text(tmp.path() / "hello.txt", "hello\n");
  const auto r = run({"add", "hello.txt"});
  ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_TRUE(r.out.empty());
  const StagingIndex staged = index();
  const auto* entry = staged.find("hello.txt");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->oid, kHelloBlob);
  EXPECT_EQ(entry->mode, "100644");
  EXPECT_EQ(store.read(kHelloBlob).content, "hello\n");
}

TEST_F(AddTest, StagesMultipleFilesAndNestedDirectories) {
  write_text(tmp.path() / "a.txt", "a");
  write_text(tmp.path() / "b.txt", "b");
  write_text(tmp.path() / "src" / "lib" / "deep.cpp", "deep");
  write_text(tmp.path() / "src" / "main.cpp", "main");
  ASSERT_EQ(run({"add", "a.txt", "b.txt", "src"}).code, minigit::kExitOk);
  EXPECT_EQ(staged_paths(),
            (std::vector<std::string>{"a.txt", "b.txt", "src/lib/deep.cpp", "src/main.cpp"}));
}

TEST_F(AddTest, DotStagesWholeWorktreeButSkipsGitDirectories) {
  write_text(tmp.path() / "top.txt", "top");
  write_text(tmp.path() / "vendor" / "lib" / ".git" / "HEAD", "nested repo");
  write_text(tmp.path() / "vendor" / "lib" / "code.c", "code");
  ASSERT_EQ(run({"add", "."}).code, minigit::kExitOk);
  EXPECT_EQ(staged_paths(), (std::vector<std::string>{"top.txt", "vendor/lib/code.c"}));
}

TEST_F(AddTest, PathsAreRelativeToCurrentDirectory) {
  write_text(tmp.path() / "sub" / "file.txt", "x");
  write_text(tmp.path() / "root.txt", "r");
  ScopedCwd in_sub(tmp.path() / "sub");
  ASSERT_EQ(run({"add", "file.txt", "../root.txt"}).code, minigit::kExitOk);
  EXPECT_EQ(staged_paths(), (std::vector<std::string>{"root.txt", "sub/file.txt"}));
}

TEST_F(AddTest, AbsolutePathInsideWorktreeIsAccepted) {
  write_text(tmp.path() / "dir" / "f", "x");
  ASSERT_EQ(run({"add", (tmp.path() / "dir" / "").string()}).code, minigit::kExitOk);
  EXPECT_EQ(staged_paths(), std::vector<std::string>{"dir/f"});
}

TEST_F(AddTest, RejectsPathsOutsideWorktree) {
  TempDir outside;
  write_text(outside.path() / "secret.txt", "s");
  for (const std::string spec : {(outside.path() / "secret.txt").string(), std::string("..")}) {
    const auto r = run({"add", spec});
    EXPECT_EQ(r.code, minigit::kExitError) << spec;
    EXPECT_NE(r.err.find("outside the repository"), std::string::npos) << r.err;
  }
  EXPECT_FALSE(fs::exists(repo.index_path()));
}

TEST_F(AddTest, RejectsPathsInsideGitDirectory) {
  for (const std::string spec : {".git", ".git/HEAD", "sub/../.git/config"}) {
    const auto r = run({"add", spec});
    EXPECT_EQ(r.code, minigit::kExitError) << spec;
    EXPECT_NE(r.err.find("repository metadata"), std::string::npos) << r.err;
  }
}

TEST_F(AddTest, InvalidPathspecStagesNothing) {
  write_text(tmp.path() / "good.txt", "g");
  ASSERT_EQ(run({"add", "good.txt", ".."}).code, minigit::kExitError);
  EXPECT_FALSE(fs::exists(repo.index_path()));
}

#ifndef _WIN32
TEST_F(AddTest, ExecutableBitBecomesMode100755) {
  write_text(tmp.path() / "run.sh", "#!/bin/sh\n");
  fs::permissions(tmp.path() / "run.sh", fs::perms::owner_exec, fs::perm_options::add);
  ASSERT_EQ(run({"add", "run.sh"}).code, minigit::kExitOk);
  ASSERT_NE(index().find("run.sh"), nullptr);
  EXPECT_EQ(index().find("run.sh")->mode, "100755");
}

TEST_F(AddTest, SymbolicLinksAreSkippedOrRejected) {
  TempDir outside;
  write_text(outside.path() / "target.txt", "t");
  write_text(tmp.path() / "dir" / "real.txt", "r");
  fs::create_symlink(outside.path() / "target.txt", tmp.path() / "dir" / "link.txt");
  fs::create_directory_symlink(outside.path(), tmp.path() / "outlink");

  const auto dir = run({"add", "dir"});
  ASSERT_EQ(dir.code, minigit::kExitOk) << dir.err;
  EXPECT_NE(dir.err.find("warning: skipping symbolic link 'dir/link.txt'"), std::string::npos);
  EXPECT_EQ(staged_paths(), std::vector<std::string>{"dir/real.txt"});

  EXPECT_EQ(run({"add", "dir/link.txt"}).code, minigit::kExitError);
  const auto through = run({"add", "outlink/target.txt"});
  EXPECT_EQ(through.code, minigit::kExitError);
  EXPECT_NE(through.err.find("beyond a symbolic link"), std::string::npos) << through.err;
}
#endif

TEST_F(AddTest, StagesDeletionOfRemovedFile) {
  write_text(tmp.path() / "keep.txt", "k");
  write_text(tmp.path() / "gone.txt", "g");
  ASSERT_EQ(run({"add", "."}).code, minigit::kExitOk);
  fs::remove(tmp.path() / "gone.txt");

  const auto r = run({"add", "gone.txt"});
  ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_EQ(staged_paths(), std::vector<std::string>{"keep.txt"});

  // Once unstaged it is no longer tracked, so naming it again is an error.
  EXPECT_EQ(run({"add", "gone.txt"}).code, minigit::kExitError);
}

TEST_F(AddTest, StagesDeletionOfRemovedDirectory) {
  write_text(tmp.path() / "lib" / "a.c", "a");
  write_text(tmp.path() / "lib" / "sub" / "b.c", "b");
  write_text(tmp.path() / "libx.c", "x");  // shares a prefix, must survive
  ASSERT_EQ(run({"add", "."}).code, minigit::kExitOk);
  fs::remove_all(tmp.path() / "lib");

  ASSERT_EQ(run({"add", "lib"}).code, minigit::kExitOk);
  EXPECT_EQ(staged_paths(), std::vector<std::string>{"libx.c"});
}

TEST_F(AddTest, WalkingDirectoryStagesDeletionsBelowIt) {
  write_text(tmp.path() / "src" / "old.cpp", "old");
  write_text(tmp.path() / "src" / "kept.cpp", "kept");
  write_text(tmp.path() / "other.txt", "o");
  ASSERT_EQ(run({"add", "."}).code, minigit::kExitOk);
  fs::remove(tmp.path() / "src" / "old.cpp");
  fs::remove(tmp.path() / "other.txt");
  write_text(tmp.path() / "src" / "new.cpp", "new");

  ASSERT_EQ(run({"add", "src"}).code, minigit::kExitOk);
  // other.txt is outside the pathspec, so its deletion is not staged yet.
  EXPECT_EQ(staged_paths(),
            (std::vector<std::string>{"other.txt", "src/kept.cpp", "src/new.cpp"}));

  ASSERT_EQ(run({"add", "."}).code, minigit::kExitOk);
  EXPECT_EQ(staged_paths(), (std::vector<std::string>{"src/kept.cpp", "src/new.cpp"}));
}

TEST_F(AddTest, DeletionMixedWithMissingUntrackedPathStagesNothing) {
  write_text(tmp.path() / "gone.txt", "g");
  ASSERT_EQ(run({"add", "gone.txt"}).code, minigit::kExitOk);
  fs::remove(tmp.path() / "gone.txt");
  const auto r = run({"add", "gone.txt", "never-existed.txt"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("pathspec 'never-existed.txt' did not match any files"), std::string::npos);
  EXPECT_EQ(staged_paths(), std::vector<std::string>{"gone.txt"});
}

TEST_F(AddTest, UsageErrors) {
  EXPECT_EQ(run({"add"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"add", "-A"}).code, minigit::kExitUsage);
  write_text(tmp.path() / "-dash", "d");
  ASSERT_EQ(run({"add", "--", "-dash"}).code, minigit::kExitOk);
  EXPECT_NE(index().find("-dash"), nullptr);
}

TEST(AddCommand, OutsideRepositoryFails) {
  TempDir tmp;
  if (fs::exists(tmp.path().parent_path() / ".git")) GTEST_SKIP() << "temp dir inside a repo";
  ScopedCwd cwd(tmp.path());
  write_text(tmp.path() / "f", "x");
  const auto r = run({"add", "f"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a minigit repository"), std::string::npos);
}

}  // namespace
