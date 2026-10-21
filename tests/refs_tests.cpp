#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "test_support.hpp"

namespace {

using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::RepositoryError;
using minigit::RevisionError;
using minigit::Signature;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;

constexpr const char* kEmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

class RefsTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0000"};

  std::string commit_file(const std::string& path, const std::string& text,
                          const std::string& message) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    index.stage(path, {"100644", store.write(ObjectType::Blob, text)});
    index.save();
    return minigit::create_commit(repo, message, author).oid;
  }
};

TEST_F(RefsTest, ResolveHeadOnUnbornBranch) {
  const auto head = minigit::resolve_head(repo);
  EXPECT_EQ(head.branch_ref, "refs/heads/main");
  EXPECT_EQ(head.commit, std::nullopt);
}

TEST_F(RefsTest, ResolveHeadFollowsBranch) {
  const std::string oid = commit_file("a", "1\n", "one");
  const auto head = minigit::resolve_head(repo);
  EXPECT_EQ(head.branch_ref, "refs/heads/main");
  EXPECT_EQ(head.commit, oid);

  write_text(tmp.path() / ".git" / "HEAD", "ref: refs/heads/other\n");
  EXPECT_EQ(minigit::resolve_head(repo).commit, std::nullopt);
}

TEST_F(RefsTest, ResolveHeadRejectsDetachedAndCorruptState) {
  const std::string oid = commit_file("a", "1\n", "one");
  write_text(tmp.path() / ".git" / "HEAD", oid + "\n");
  EXPECT_THROW(minigit::resolve_head(repo), RepositoryError);

  write_text(tmp.path() / ".git" / "HEAD", "ref: refs/heads/main\n");
  write_text(tmp.path() / ".git" / "refs" / "heads" / "main", "not-an-id\n");
  EXPECT_THROW(minigit::resolve_head(repo), RepositoryError);
}

TEST(ShortBranchName, StripsOnlyTheHeadsPrefix) {
  EXPECT_EQ(minigit::short_branch_name("refs/heads/main"), "main");
  EXPECT_EQ(minigit::short_branch_name("refs/heads/feature/x"), "feature/x");
  EXPECT_EQ(minigit::short_branch_name("refs/tags/v1"), "refs/tags/v1");
}

TEST_F(RefsTest, ResolveRevisionNames) {
  const std::string first = commit_file("a", "1\n", "one");
  const std::string second = commit_file("a", "2\n", "two");
  repo.write_ref("refs/heads/old", first);

  EXPECT_EQ(minigit::resolve_revision(repo, "HEAD"), second);
  EXPECT_EQ(minigit::resolve_revision(repo, "main"), second);
  EXPECT_EQ(minigit::resolve_revision(repo, "refs/heads/main"), second);
  EXPECT_EQ(minigit::resolve_revision(repo, "old"), first);
  EXPECT_EQ(minigit::resolve_revision(repo, first), first);
  // A full id is returned without checking it exists, like `git rev-parse`.
  EXPECT_EQ(minigit::resolve_revision(repo, kEmptyTree), kEmptyTree);
}

TEST_F(RefsTest, TagsWinOverBranchesOfTheSameName) {
  const std::string first = commit_file("a", "1\n", "one");
  const std::string second = commit_file("a", "2\n", "two");
  repo.write_ref("refs/heads/v1", second);
  repo.write_ref("refs/tags/v1", first);
  EXPECT_EQ(minigit::resolve_revision(repo, "v1"), first);
}

TEST_F(RefsTest, ResolveRevisionErrors) {
  EXPECT_THROW(minigit::resolve_revision(repo, "HEAD"), RevisionError);  // no commits yet
  commit_file("a", "1\n", "one");
  for (const std::string bad : {"nope", "", "a..b", "main~1", "main^", "ce0136", "HEAD~1",
                                "../../HEAD", "refs/heads/../x"}) {
    EXPECT_THROW(minigit::resolve_revision(repo, bad), RevisionError) << bad;
  }
}

TEST_F(RefsTest, ReadCommitChecksTheType) {
  const std::string oid = commit_file("a", "1\n", "one");
  EXPECT_EQ(minigit::read_commit(store, oid).message, "one\n");
  const std::string blob = store.write(ObjectType::Blob, "x");
  EXPECT_THROW(minigit::read_commit(store, blob), ObjectError);
  EXPECT_THROW(minigit::read_commit(store, kEmptyTree), ObjectError);  // missing
}

TEST_F(RefsTest, ReadCommitSnapshotFlattensTheTree) {
  const std::string first = commit_file("a", "1\n", "one");
  const std::string second = commit_file("dir/sub/b c", "2\n", "two");

  using Snapshot = std::map<std::string, minigit::IndexEntry>;
  const std::string blob1 = minigit::hash_object(ObjectType::Blob, "1\n");
  const std::string blob2 = minigit::hash_object(ObjectType::Blob, "2\n");
  EXPECT_EQ(minigit::read_commit_snapshot(store, first), (Snapshot{{"a", {"100644", blob1}}}));
  EXPECT_EQ(minigit::read_commit_snapshot(store, second),
            (Snapshot{{"a", {"100644", blob1}}, {"dir/sub/b c", {"100644", blob2}}}));

  EXPECT_THROW(minigit::read_commit_snapshot(store, blob1), ObjectError);       // not a commit
  EXPECT_THROW(minigit::read_commit_snapshot(store, kEmptyTree), ObjectError);  // missing
}

TEST_F(RefsTest, ReadCommitSnapshotMatchesGitLsTree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  // A commit made by real Git, with modes MiniGit++ never writes itself.
  write_text(tmp.path() / "top.txt", "top\n");
  write_text(tmp.path() / "src" / "deep" / "x.sh", "#!/bin/sh\n");
  write_text(tmp.path() / "src" / "with space", "s\n");
  write_text(tmp.path() / "src-b", "dash\n");
  ASSERT_EQ(git_in(tmp.path(), "add .").status, 0);
  ASSERT_EQ(git_in(tmp.path(), "update-index --chmod=+x src/deep/x.sh").status, 0);
  const std::string link_blob = store.write(ObjectType::Blob, "top.txt");
  ASSERT_EQ(git_in(tmp.path(), "update-index --add --cacheinfo 120000," + link_blob + ",link")
                .status,
            0);
  ASSERT_EQ(git_in(tmp.path(),
                   "-c user.name=Ada -c user.email=ada@example.com commit -q -m from-git")
                .status,
            0);
  const std::string head = *repo.read_ref("refs/heads/main");

  // `ls-tree -r -z`: "<mode> <type> <id>\t<path>\0" per file, unquoted.
  const auto listing = git_in(tmp.path(), "ls-tree -r -z HEAD");
  ASSERT_EQ(listing.status, 0);
  std::map<std::string, minigit::IndexEntry> expected;
  std::size_t pos = 0;
  while (pos < listing.output.size()) {
    auto end = listing.output.find('\0', pos);
    if (end == std::string::npos) end = listing.output.size();
    const std::string line = listing.output.substr(pos, end - pos);
    const auto tab = line.find('\t');
    ASSERT_NE(tab, std::string::npos) << line;
    expected[line.substr(tab + 1)] = {line.substr(0, line.find(' ')), line.substr(tab - 40, 40)};
    pos = end + 1;
  }
  ASSERT_EQ(expected.size(), 5u);
  EXPECT_EQ(minigit::read_commit_snapshot(store, head), expected);
}

TEST(RefName, Validation) {
  EXPECT_TRUE(Repository::is_valid_ref_name("refs/heads/main"));
  EXPECT_TRUE(Repository::is_valid_ref_name("refs/heads/feature/x-1"));
  for (const std::string bad : {"main", "refs/heads/", "refs//x", "refs/heads/.x",
                                "refs/heads/x.lock", "refs/heads/a b", "refs/heads/a..b",
                                "refs/heads/a@{1}", "refs/heads/a~1", "refs/heads/a:b"}) {
    EXPECT_FALSE(Repository::is_valid_ref_name(bad)) << bad;
  }
}

TEST_F(RefsTest, CatFileAcceptsRefNames) {
  const std::string oid = commit_file("a", "1\n", "one");
  ScopedCwd cwd(tmp.path());
  for (const std::string& name : std::vector<std::string>{"HEAD", "main", "refs/heads/main", oid}) {
    std::ostringstream out, err;
    EXPECT_EQ(minigit::run_cli({"cat-file", "-t", name}, out, err), minigit::kExitOk) << name;
    EXPECT_EQ(out.str(), "commit\n");
  }
  std::ostringstream out, err;
  EXPECT_EQ(minigit::run_cli({"cat-file", "-t", "nope"}, out, err), minigit::kExitError);
  EXPECT_NE(err.str().find("not a valid object name 'nope'"), std::string::npos);
}

}  // namespace
