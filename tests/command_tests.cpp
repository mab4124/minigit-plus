#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/tree.hpp"
#include "test_support.hpp"

namespace {

using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
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

}  // namespace
