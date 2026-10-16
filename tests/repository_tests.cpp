#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/repository.hpp"
#include "test_support.hpp"

namespace {

using minigit::Repository;
using minigit::RepositoryError;
using minigit::test::read_text;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

TEST(Repository, InitCreatesLayout) {
  TempDir tmp;
  bool reinit = true;
  const Repository repo = Repository::init(tmp.path(), &reinit);

  EXPECT_FALSE(reinit);
  EXPECT_EQ(repo.worktree(), tmp.path());
  EXPECT_EQ(read_text(tmp.path() / ".git" / "HEAD"), "ref: refs/heads/main\n");
  EXPECT_TRUE(fs::is_directory(tmp.path() / ".git" / "objects"));
  EXPECT_TRUE(fs::is_directory(tmp.path() / ".git" / "refs" / "heads"));
  EXPECT_TRUE(fs::is_directory(tmp.path() / ".git" / "refs" / "tags"));
  EXPECT_TRUE(fs::is_directory(tmp.path() / ".git" / "minigit"));
  EXPECT_EQ(repo.head_ref(), "refs/heads/main");
}

TEST(Repository, InitCreatesMissingDirectory) {
  TempDir tmp;
  const Repository repo = Repository::init(tmp.path() / "a" / "b");
  EXPECT_TRUE(Repository::is_repository_root(tmp.path() / "a" / "b"));
}

TEST(Repository, ReinitDoesNotTouchExistingState) {
  TempDir tmp;
  Repository::init(tmp.path());
  write_text(tmp.path() / ".git" / "HEAD", "ref: refs/heads/feature\n");

  bool reinit = false;
  const Repository repo = Repository::init(tmp.path(), &reinit);
  EXPECT_TRUE(reinit);
  EXPECT_EQ(repo.head_ref(), "refs/heads/feature");
}

TEST(Repository, InitRefusesBrokenGitDir) {
  TempDir tmp;
  write_text(tmp.path() / ".git" / "stray-file", "x");
  EXPECT_THROW(Repository::init(tmp.path()), RepositoryError);
}

TEST(Repository, DiscoverFindsRootFromSubdirectory) {
  TempDir tmp;
  Repository::init(tmp.path());
  fs::create_directories(tmp.path() / "src" / "deep");

  const Repository repo = Repository::discover(tmp.path() / "src" / "deep");
  EXPECT_EQ(repo.worktree(), tmp.path());
}

TEST(Repository, DiscoverOutsideRepositoryThrows) {
  TempDir tmp;
  // The temp directory's ancestors could in theory contain a repository;
  // skip instead of reporting a false failure in that case.
  if (fs::exists(tmp.path().parent_path() / ".git")) GTEST_SKIP() << "temp dir inside a repo";
  try {
    Repository::discover(tmp.path());
    FAIL() << "expected RepositoryError";
  } catch (const RepositoryError& e) {
    EXPECT_NE(std::string(e.what()).find("not a minigit repository"), std::string::npos);
  }
}

TEST(Repository, MalformedHeadIsReported) {
  TempDir tmp;
  const Repository repo = Repository::init(tmp.path());
  write_text(tmp.path() / ".git" / "HEAD", "garbage\n");
  EXPECT_THROW(repo.head_ref(), RepositoryError);
}

// Real Git acts as an oracle: it must accept our layout as a repository.
TEST(GitOracle, GitRecognizesInitializedRepository) {
  if (!minigit::test::git_available()) GTEST_SKIP() << "git not installed";
  TempDir tmp;
  Repository::init(tmp.path());

  const fs::path out_file = tmp.path() / "branch.txt";
  const std::string cmd = "git -C \"" + tmp.path().string() +
                          "\" symbolic-ref HEAD > \"" + out_file.string() + "\"";
  ASSERT_EQ(std::system(cmd.c_str()), 0);
  EXPECT_EQ(read_text(out_file).substr(0, 15), "refs/heads/main");
}

}  // namespace
