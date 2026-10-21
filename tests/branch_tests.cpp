#include <gtest/gtest.h>

#include <sstream>

#include "minigit/branch.hpp"
#include "minigit/cli.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "test_support.hpp"

namespace {

using minigit::BranchError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::Signature;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

using Names = std::vector<std::string>;

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

class BranchTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  ScopedCwd cwd{tmp.path()};
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0000"};

  std::string commit_file(const std::string& path, const std::string& text,
                          const std::string& message) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    index.stage(path, {"100644", store.write(ObjectType::Blob, text)});
    index.save();
    return minigit::create_commit(repo, message, author).oid;
  }
};

TEST(BranchName, FollowsGitRules) {
  for (const std::string ok : {"main", "feature/x", "v1.2", "a-b_c", "x@y", "@", "UPPER"}) {
    EXPECT_TRUE(minigit::is_valid_branch_name(ok)) << ok;
  }
  for (const std::string bad :
       {"", "-x", "HEAD", "a..b", "a/", "/a", "a//b", "a.", ".a", "a/.b", "x.lock", "a b",
        "a~1", "a^", "a:b", "a?", "a*", "a[", "a\\b", "a@{1}", "a\tb"}) {
    EXPECT_FALSE(minigit::is_valid_branch_name(bad)) << bad;
  }
  EXPECT_EQ(minigit::branch_ref("feature/x"), "refs/heads/feature/x");
  EXPECT_THROW(minigit::branch_ref("a..b"), BranchError);
}

TEST(BranchName, AgreesWithGitCheckRefFormat) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  TempDir tmp;
  for (const std::string name : {"main", "feature/x", "-x", "HEAD", "a..b", "a/", "a.", ".a",
                                 "x.lock", "a~1", "a:b", "a@{1}", "v1.2"}) {
    const bool git_ok =
        git_in(tmp.path(), "check-ref-format --branch \"" + name + "\" 2>&1").status == 0;
    EXPECT_EQ(minigit::is_valid_branch_name(name), git_ok) << name;
  }
}

TEST_F(BranchTest, CreateAndList) {
  EXPECT_TRUE(minigit::list_branches(repo).empty());  // unborn main has no ref
  const std::string oid = commit_file("a", "1\n", "one");
  EXPECT_EQ(minigit::list_branches(repo), Names{"main"});

  minigit::create_branch(repo, "topic", oid);
  minigit::create_branch(repo, "feature/x", oid);
  EXPECT_EQ(minigit::list_branches(repo), (Names{"feature/x", "main", "topic"}));
  EXPECT_EQ(repo.read_ref("refs/heads/feature/x"), oid);
}

TEST_F(BranchTest, CreateRefusesExistingAndClashingNames) {
  const std::string oid = commit_file("a", "1\n", "one");
  minigit::create_branch(repo, "topic", oid);
  minigit::create_branch(repo, "dir/sub", oid);

  EXPECT_THROW(minigit::create_branch(repo, "topic", oid), BranchError);
  EXPECT_THROW(minigit::create_branch(repo, "main", oid), BranchError);
  EXPECT_THROW(minigit::create_branch(repo, "topic/child", oid), BranchError);
  EXPECT_THROW(minigit::create_branch(repo, "dir", oid), BranchError);
  EXPECT_THROW(minigit::create_branch(repo, "bad..name", oid), BranchError);
  EXPECT_EQ(minigit::list_branches(repo), (Names{"dir/sub", "main", "topic"}));
}

TEST_F(BranchTest, CliListsWithCurrentMarked) {
  EXPECT_EQ(run({"branch"}).out, "");
  const std::string first = commit_file("a", "1\n", "one");
  ASSERT_EQ(run({"branch", "zeta"}).code, minigit::kExitOk);
  ASSERT_EQ(run({"branch", "alpha"}).code, minigit::kExitOk);
  const auto r = run({"branch"});
  EXPECT_EQ(r.code, minigit::kExitOk);
  EXPECT_EQ(r.out, "  alpha\n* main\n  zeta\n");
  EXPECT_EQ(repo.read_ref("refs/heads/alpha"), first);
}

TEST_F(BranchTest, CliCreatesFromStartPoint) {
  const std::string first = commit_file("a", "1\n", "one");
  const std::string second = commit_file("a", "2\n", "two");
  ASSERT_EQ(run({"branch", "old", first}).code, minigit::kExitOk);
  ASSERT_EQ(run({"branch", "copy", "old"}).code, minigit::kExitOk);
  ASSERT_EQ(run({"branch", "tip"}).code, minigit::kExitOk);
  EXPECT_EQ(repo.read_ref("refs/heads/old"), first);
  EXPECT_EQ(repo.read_ref("refs/heads/copy"), first);
  EXPECT_EQ(repo.read_ref("refs/heads/tip"), second);
}

TEST_F(BranchTest, CliErrors) {
  auto r = run({"branch", "topic"});  // no commits yet
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a valid object name: 'HEAD'"), std::string::npos) << r.err;
  EXPECT_FALSE(fs::exists(tmp.path() / ".git/refs/heads/topic"));

  const std::string oid = commit_file("a", "1\n", "one");
  r = run({"branch", "bad..name"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a valid branch name"), std::string::npos) << r.err;

  r = run({"branch", "main"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("already exists"), std::string::npos) << r.err;

  r = run({"branch", "x", "nowhere"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a valid object name: 'nowhere'"), std::string::npos) << r.err;

  // A start point that is not a commit is refused.
  const std::string blob = store.write(ObjectType::Blob, "x\n");
  r = run({"branch", "x", blob});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_FALSE(fs::exists(tmp.path() / ".git/refs/heads/x"));

  EXPECT_EQ(run({"branch", "-d", "x"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"branch", "a", "b", "c"}).code, minigit::kExitUsage);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), oid);
}

TEST_F(BranchTest, MatchesGitBranchListing) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  commit_file("a", "1\n", "one");
  for (const std::string name : {"topic", "feature/a", "feature/b", "Zed", "z"}) {
    ASSERT_EQ(run({"branch", name}).code, minigit::kExitOk) << name;
  }
  const auto git = git_in(tmp.path(), "branch --no-color");
  ASSERT_EQ(git.status, 0);
  EXPECT_EQ(run({"branch"}).out, git.output + "\n");
  EXPECT_EQ(git_in(tmp.path(), "rev-parse topic").output, *repo.read_ref("refs/heads/topic"));
}

}  // namespace
