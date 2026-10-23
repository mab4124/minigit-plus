#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <sstream>

#include "minigit/branch.hpp"
#include "minigit/cli.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/history.hpp"
#include "minigit/history_index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"
#include "test_support.hpp"

namespace {

using minigit::HistoryIndex;
using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::Signature;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

using Files = std::map<std::string, std::optional<std::string>>;

class HistoryIndexTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  fs::path db_path = HistoryIndex::default_path(repo);
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0000"};

  // Stages `files` (an empty optional unstages the path) and commits.
  std::string commit(const Files& files, const std::string& message) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    for (const auto& [path, text] : files) {
      if (text) {
        index.stage(path, {"100644", store.write(ObjectType::Blob, *text)});
      } else {
        index.remove(path);
      }
    }
    index.save();
    author.timestamp += 60;
    return minigit::create_commit(repo, message, author).oid;
  }

  // What the index must hold for `oid`: its diff against its first parent.
  std::vector<minigit::FileChange> expected_changes(const std::string& oid) {
    const auto c = minigit::read_commit(store, oid);
    std::optional<std::string> parent_tree;
    if (!c.parents.empty()) parent_tree = minigit::read_commit(store, c.parents[0]).tree;
    return minigit::diff_trees(store, parent_tree, c.tree);
  }

  void expect_matches_objects(const HistoryIndex& index, const std::vector<std::string>& oids) {
    for (const auto& oid : oids) {
      EXPECT_TRUE(index.contains(oid)) << oid;
      EXPECT_EQ(index.changes(oid), expected_changes(oid)) << oid;
    }
  }
};

TEST_F(HistoryIndexTest, DefaultPathIsUnderMinigitDir) {
  EXPECT_EQ(db_path, tmp.path() / ".git" / "minigit" / "history.db");
}

TEST_F(HistoryIndexTest, EmptyRepositoryIndexesNothing) {
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  const auto update = index.update(repo);
  EXPECT_EQ(update.commits_added, 0u);
  EXPECT_EQ(update.total_commits, 0u);
  EXPECT_TRUE(fs::is_regular_file(db_path));
}

TEST_F(HistoryIndexTest, RecordsEveryCommitsChangedPaths) {
  const std::string c1 = commit({{"a.txt", "1"}, {"src/main.cpp", "m"}, {"src/util.cpp", "u"}},
                                "one");
  const std::string c2 = commit({{"a.txt", "2"}, {"src/util.cpp", std::nullopt}}, "two");
  const std::string c3 = commit({{"src/new/deep.txt", "d"}}, "three");

  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  const auto update = index.update(repo);
  EXPECT_EQ(update.commits_added, 3u);
  EXPECT_EQ(update.changes_added, 3u + 2u + 1u);
  EXPECT_EQ(update.total_commits, 3u);
  EXPECT_EQ(index.commit_count(), 3u);
  expect_matches_objects(index, {c1, c2, c3});

  const auto changes = index.changes(c2);
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].path, "a.txt");
  EXPECT_EQ(changes[0].kind, minigit::ChangeKind::Modified);
  EXPECT_EQ(changes[1].kind, minigit::ChangeKind::Deleted);
  EXPECT_FALSE(changes[1].new_entry);
  EXPECT_FALSE(index.contains(std::string(40, 'a')));
  EXPECT_TRUE(index.changes(std::string(40, 'a')).empty());
}

TEST_F(HistoryIndexTest, UpdateIsIncremental) {
  const std::string c1 = commit({{"a", "1"}}, "one");
  {
    HistoryIndex index = HistoryIndex::open_for_update(db_path);
    EXPECT_EQ(index.update(repo).commits_added, 1u);
    // Nothing new: nothing is read or written again.
    EXPECT_EQ(index.update(repo).commits_added, 0u);
  }
  const std::string c2 = commit({{"a", "2"}}, "two");
  const std::string c3 = commit({{"b", "b"}}, "three");

  // Reopened, as a later `minigit index` would.
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  EXPECT_TRUE(index.contains(c1));
  EXPECT_FALSE(index.contains(c2));
  const auto update = index.update(repo);
  EXPECT_EQ(update.commits_added, 2u);
  EXPECT_EQ(update.changes_added, 2u);
  EXPECT_EQ(update.total_commits, 3u);
  expect_matches_objects(index, {c1, c2, c3});
}

TEST_F(HistoryIndexTest, OnlyNewCommitsAreRead) {
  const std::string c1 = commit({{"a", "1"}}, "one");
  commit({{"a", "2"}}, "two");
  HistoryIndex::open_for_update(db_path).update(repo);
  const std::string c3 = commit({{"a", "3"}}, "three");
  // An update reads the new commits and their parent, never older history:
  // it still succeeds with the root commit's object gone.
  fs::remove(store.object_path(c1));
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  EXPECT_EQ(index.update(repo).commits_added, 1u);
  expect_matches_objects(index, {c3});
}

TEST_F(HistoryIndexTest, IndexesEveryBranch) {
  const std::string base = commit({{"f", "1"}}, "base");
  const std::string m1 = commit({{"f", "main"}}, "on main");
  minigit::create_branch(repo, "side", base);
  repo.set_head("refs/heads/side");
  // The staging index still holds main's files; commit a different tree.
  const std::string s1 = commit({{"g", "side"}}, "on side");
  const std::string s2 = commit({{"f", "side"}}, "side again");

  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  const auto update = index.update(repo);
  EXPECT_EQ(update.commits_added, 4u);  // the shared base once
  expect_matches_objects(index, {base, m1, s1, s2});

  // A new branch at an indexed commit adds nothing.
  minigit::create_branch(repo, "copy", s2);
  EXPECT_EQ(index.update(repo).commits_added, 0u);
}

TEST_F(HistoryIndexTest, FollowsFirstParentsOfMerges) {
  const std::string base = commit({{"f", "1"}}, "base");
  // A side commit and a merge with `base` as first parent, written by hand
  // since MiniGit++ has no merge command.
  minigit::Commit side;
  side.tree = store.write(
      ObjectType::Tree,
      minigit::serialize_tree({{"100644", "f", store.write(ObjectType::Blob, "side")}}));
  side.parents = {base};
  side.author = side.committer = author;
  side.message = "side\n";
  const std::string side_oid = store.write(ObjectType::Commit, minigit::serialize_commit(side));
  minigit::Commit merge = side;
  merge.parents = {base, side_oid};
  merge.message = "merge\n";
  const std::string merge_oid = store.write(ObjectType::Commit, minigit::serialize_commit(merge));
  repo.write_ref("refs/heads/main", merge_oid);

  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  EXPECT_EQ(index.update(repo).commits_added, 2u);
  EXPECT_FALSE(index.contains(side_oid));  // only reachable as a second parent
  expect_matches_objects(index, {base, merge_oid});
  ASSERT_EQ(index.changes(merge_oid).size(), 1u);
}

TEST_F(HistoryIndexTest, MissingObjectWritesNothing) {
  const std::string c1 = commit({{"a", "1"}}, "one");
  HistoryIndex::open_for_update(db_path).update(repo);
  const std::string c2 = commit({{"a", "2"}}, "two");
  const std::string c3 = commit({{"a", "3"}}, "three");
  fs::remove(store.object_path(minigit::read_commit(store, c2).tree));

  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  EXPECT_THROW(index.update(repo), ObjectError);
  EXPECT_EQ(index.commit_count(), 1u);
  EXPECT_TRUE(index.contains(c1));
  EXPECT_FALSE(index.contains(c3));
}

TEST_F(HistoryIndexTest, RebuildGivesTheSameIndex) {
  const std::string c1 = commit({{"a", "1"}, {"d/b", "b"}}, "one");
  const std::string c2 = commit({{"a", "2"}}, "two");
  HistoryIndex::open_for_update(db_path).update(repo);
  const std::string c3 = commit({{"d/b", std::nullopt}}, "three");

  HistoryIndex rebuilt = HistoryIndex::open_for_update(db_path, /*rebuild=*/true);
  // Until the rebuild completes, the old index is still in place.
  {
    auto old = HistoryIndex::open_existing(db_path);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->commit_count(), 2u);
  }
  const auto update = rebuilt.update(repo);
  EXPECT_EQ(update.commits_added, 3u);
  EXPECT_EQ(update.total_commits, 3u);
  expect_matches_objects(rebuilt, {c1, c2, c3});
  EXPECT_FALSE(fs::exists(db_path.string() + ".rebuild"));

  // The rebuilt index is the one on disk and stays usable for updates.
  const std::string c4 = commit({{"e", "e"}}, "four");
  EXPECT_EQ(rebuilt.update(repo).commits_added, 1u);
  auto reopened = HistoryIndex::open_existing(db_path);
  ASSERT_TRUE(reopened);
  expect_matches_objects(*reopened, {c1, c2, c3, c4});
}

TEST_F(HistoryIndexTest, DeletedIndexIsRebuiltFromObjects) {
  const std::string c1 = commit({{"a", "1"}}, "one");
  const std::string c2 = commit({{"a", "2"}}, "two");
  HistoryIndex::open_for_update(db_path).update(repo);
  fs::remove(db_path);
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  EXPECT_EQ(index.update(repo).commits_added, 2u);
  expect_matches_objects(index, {c1, c2});
}

TEST_F(HistoryIndexTest, AbandonedRebuildKeepsTheOldIndex) {
  const std::string c1 = commit({{"a", "1"}}, "one");
  HistoryIndex::open_for_update(db_path).update(repo);
  { HistoryIndex abandoned = HistoryIndex::open_for_update(db_path, true); }
  EXPECT_FALSE(fs::exists(db_path.string() + ".rebuild"));
  auto index = HistoryIndex::open_existing(db_path);
  ASSERT_TRUE(index);
  EXPECT_TRUE(index->contains(c1));
}

TEST_F(HistoryIndexTest, CorruptFileIsReplaced) {
  const std::string c1 = commit({{"a", "1"}}, "one");
  write_text(db_path, "this is not a database, just some text that is long enough");
  EXPECT_FALSE(HistoryIndex::open_existing(db_path));
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  EXPECT_EQ(index.update(repo).commits_added, 1u);
  expect_matches_objects(index, {c1});
  EXPECT_TRUE(HistoryIndex::open_existing(db_path));
}

TEST_F(HistoryIndexTest, OpenExistingNeverCreatesAFile) {
  EXPECT_FALSE(HistoryIndex::open_existing(db_path));
  EXPECT_FALSE(fs::exists(db_path));
}

TEST(HistoryIndexCommand, IndexesAndRebuilds) {
  TempDir tmp;
  ScopedCwd cwd(tmp.path());
  auto run = [](const std::vector<std::string>& args, std::string* out_text = nullptr) {
    std::ostringstream out, err;
    const int code = minigit::run_cli(args, out, err);
    if (out_text) *out_text = out.str();
    return code;
  };
  ASSERT_EQ(run({"init"}), 0);
  std::string out;
  ASSERT_EQ(run({"index"}, &out), 0);
  EXPECT_EQ(out,
            "Updated the history index: 0 new commits, 0 changed paths; 0 commits indexed in "
            ".git/minigit/history.db\n");

  ASSERT_EQ(run({"config", "user.name", "Ada"}), 0);
  ASSERT_EQ(run({"config", "user.email", "ada@example.com"}), 0);
  write_text(tmp.path() / "a.txt", "a");
  write_text(tmp.path() / "src" / "b.txt", "b");
  ASSERT_EQ(run({"add", "."}), 0);
  ASSERT_EQ(run({"commit", "-m", "one"}), 0);
  ASSERT_EQ(run({"index"}, &out), 0);
  EXPECT_EQ(out,
            "Updated the history index: 1 new commit, 2 changed paths; 1 commit indexed in "
            ".git/minigit/history.db\n");
  ASSERT_EQ(run({"index"}, &out), 0);
  EXPECT_NE(out.find(": 0 new commits"), std::string::npos);
  ASSERT_EQ(run({"index", "--rebuild"}, &out), 0);
  EXPECT_EQ(out,
            "Rebuilt the history index: 1 new commit, 2 changed paths; 1 commit indexed in "
            ".git/minigit/history.db\n");
}

TEST(HistoryIndexCommand, RejectsBadArguments) {
  TempDir tmp;
  ScopedCwd cwd(tmp.path());
  std::ostringstream out, err;
  EXPECT_EQ(minigit::run_cli({"index", "--fast"}, out, err), minigit::kExitUsage);
  EXPECT_NE(err.str().find("usage: minigit index [--rebuild]"), std::string::npos);
  // Outside a repository.
  EXPECT_EQ(minigit::run_cli({"index"}, out, err), minigit::kExitError);
  EXPECT_FALSE(fs::exists(tmp.path() / ".git"));
}

}  // namespace
