#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <optional>
#include <random>
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

// Index answers and traversal answers must be identical.
void expect_same_history(const std::vector<minigit::FileHistoryEntry>& traversal,
                         const std::vector<minigit::FileHistoryEntry>& indexed,
                         const std::string& context) {
  ASSERT_EQ(indexed.size(), traversal.size()) << context;
  for (std::size_t i = 0; i < traversal.size(); ++i) {
    EXPECT_EQ(indexed[i].oid, traversal[i].oid) << context << " #" << i;
    EXPECT_EQ(indexed[i].commit, traversal[i].commit) << context << " #" << i;
    EXPECT_EQ(indexed[i].change, traversal[i].change) << context << " #" << i;
  }
}

TEST_F(HistoryIndexTest, FileHistoryFromIndexOrderAndLimit) {
  const std::string c1 = commit({{"f.txt", "1"}, {"other", "o"}}, "add");
  commit({{"other", "o2"}}, "unrelated");
  const std::string c3 = commit({{"f.txt", "2"}}, "modify");
  const std::string c4 = commit({{"f.txt", std::nullopt}}, "delete");
  const std::string c5 = commit({{"f.txt", "3"}}, "re-add");
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  index.update(repo);

  const auto history = index.file_history(store, c5, "f.txt", 100);
  ASSERT_TRUE(history);
  std::vector<std::string> ids;
  for (const auto& e : *history) ids.push_back(e.oid);
  EXPECT_EQ(ids, (std::vector<std::string>{c5, c4, c3, c1}));
  EXPECT_EQ((*history)[1].change.kind, minigit::ChangeKind::Deleted);
  EXPECT_EQ((*history)[3].commit.message, "add\n");
  EXPECT_EQ(index.file_history(store, c5, "f.txt", 2)->size(), 2u);
  EXPECT_TRUE(index.file_history(store, c5, "f.txt", 0)->empty());
  EXPECT_TRUE(index.file_history(store, c5, "never", 10)->empty());
  // Starting further back hides newer commits.
  EXPECT_EQ(index.file_history(store, c3, "f.txt", 100)->size(), 2u);
  // A commit the index has never seen: the caller must traverse.
  EXPECT_FALSE(index.file_history(store, std::string(40, 'b'), "f.txt", 100));
}

TEST_F(HistoryIndexTest, FileHistoryFromIndexSkipsOtherBranches) {
  const std::string base = commit({{"f", "1"}}, "base");
  const std::string m1 = commit({{"f", "main 1"}}, "main 1");
  const std::string m2 = commit({{"f", "main 2"}}, "main 2");
  minigit::create_branch(repo, "side", base);
  repo.set_head("refs/heads/side");
  const std::string s1 = commit({{"f", "side 1"}}, "side 1");  // same depth as m1
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  index.update(repo);

  auto ids = [&](const std::string& start) {
    std::vector<std::string> out;
    const auto history = index.file_history(store, start, "f", 100);
    for (const auto& e : history.value()) out.push_back(e.oid);
    return out;
  };
  EXPECT_EQ(ids(m2), (std::vector<std::string>{m2, m1, base}));
  EXPECT_EQ(ids(s1), (std::vector<std::string>{s1, base}));
}

// A pseudo-random history over a small set of paths, with several branches
// and hand-made merges, checked path by path against traversal from every
// commit. MINIGIT_LONG_TESTS=1 (set by the nightly workflow) makes it larger.
TEST_F(HistoryIndexTest, IndexAndTraversalAgreeOnGeneratedHistory) {
  const bool long_run = std::getenv("MINIGIT_LONG_TESTS") != nullptr;
  const int kCommits = long_run ? 1500 : 120;
  const std::vector<std::string> paths = {"a.txt",   "b.txt",       "src/main.cpp", "src/util.cpp",
                                          "src/lib/x.h", "docs/readme", "p",        "p/q"};
  std::mt19937 rng(20261023);
  auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };

  std::vector<std::string> branches = {"main"};
  std::vector<std::string> all_commits;
  int merges = 0;
  commit({{"a.txt", "start"}}, "root");
  all_commits.push_back(*repo.read_ref("refs/heads/main"));
  // Indexed halfway through and again at the end, so the incremental path is
  // covered too.
  HistoryIndex index = HistoryIndex::open_for_update(db_path);
  for (int i = 0; i < kCommits; ++i) {
    if (i == kCommits / 2) index.update(repo);
    const auto roll = pick(12);
    if (roll == 0 && branches.size() < 6) {
      // Start a new branch at the current commit and switch to it.
      const std::string name = "b" + std::to_string(i);
      minigit::create_branch(repo, name, minigit::resolve_head(repo).commit.value());
      repo.set_head("refs/heads/" + name);
      branches.push_back(name);
      continue;
    }
    if (roll == 1) {
      repo.set_head("refs/heads/" + branches[pick(branches.size())]);
    }
    if ((roll == 2 || roll == 3) && branches.size() > 1) {
      // Merge another branch's tip: the tree of one side, two parents.
      const std::string head = minigit::resolve_head(repo).commit.value();
      const std::string other =
          repo.read_ref("refs/heads/" + branches[pick(branches.size())]).value();
      if (other != head) {
        minigit::Commit merge = minigit::read_commit(store, other);
        merge.parents = {head, other};
        author.timestamp += 60;
        merge.author = merge.committer = author;
        merge.message = "merge\n";
        const std::string oid =
            store.write(ObjectType::Commit, minigit::serialize_commit(merge));
        repo.write_ref(minigit::resolve_head(repo).branch_ref, oid);
        // Keep the staging index in step with the new tree.
        auto staged = minigit::StagingIndex::load(repo.index_path());
        for (const auto& path : staged.paths_under("")) staged.remove(path);
        for (const auto& [path, entry] : minigit::read_commit_snapshot(store, oid)) {
          staged.stage(path, entry);
        }
        staged.save();
        all_commits.push_back(oid);
        ++merges;
        continue;
      }
    }
    Files files;
    const auto n = 1 + pick(3);
    for (std::size_t k = 0; k < n; ++k) {
      const std::string& path = paths[pick(paths.size())];
      if (pick(5) == 0) {
        files[path] = std::nullopt;
      } else {
        files[path] = "v" + std::to_string(pick(4));  // values repeat: modify back and forth
      }
    }
    // p and p/q cannot both exist: staging one drops the other.
    if (files.contains("p") && files.contains("p/q")) files.erase("p");
    try {
      all_commits.push_back(commit(files, "commit " + std::to_string(i)));
    } catch (const minigit::CommitError&) {
      // Nothing changed (e.g. the same values were staged again).
    }
  }

  index.update(repo);
  ASSERT_GT(index.commit_count(), 1u);
  // The generator really produced the shapes this test is about.
  EXPECT_GT(branches.size(), 2u);
  EXPECT_GT(merges, 3);

  std::vector<std::string> queries = paths;
  queries.insert(queries.end(), {"src", "src/lib", "missing.txt"});
  std::size_t checked = 0;
  for (const auto& start : all_commits) {
    if (!index.contains(start)) continue;  // only reachable through a merge
    for (const auto& path : queries) {
      for (const std::size_t limit : {std::size_t{1}, std::size_t{3}, std::size_t{100000}}) {
        if (!long_run && limit == 3 && pick(4) != 0) continue;
        const auto indexed = index.file_history(store, start, path, limit);
        ASSERT_TRUE(indexed);
        expect_same_history(minigit::file_history(store, start, path, limit), *indexed,
                            start + " " + path + " limit " + std::to_string(limit));
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 100u);
}

TEST_F(HistoryIndexTest, WithIndexFallsBackWhenStaleMissingOrBroken) {
  const std::string c1 = commit({{"f", "1"}}, "one");
  bool used = true;
  // No index yet.
  auto history = minigit::file_history_with_index(db_path, store, c1, "f", 10, &used);
  EXPECT_FALSE(used);
  EXPECT_EQ(history.size(), 1u);
  EXPECT_FALSE(fs::exists(db_path));  // reading never creates it

  HistoryIndex::open_for_update(db_path).update(repo);
  history = minigit::file_history_with_index(db_path, store, c1, "f", 10, &used);
  EXPECT_TRUE(used);
  EXPECT_EQ(history.size(), 1u);

  // A commit made after indexing is not covered: traverse, and see it.
  const std::string c2 = commit({{"f", "2"}}, "two");
  history = minigit::file_history_with_index(db_path, store, c2, "f", 10, &used);
  EXPECT_FALSE(used);
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].oid, c2);

  HistoryIndex::open_for_update(db_path).update(repo);
  expect_same_history(minigit::file_history(store, c2, "f", 10),
                      minigit::file_history_with_index(db_path, store, c2, "f", 10, &used),
                      "after update");
  EXPECT_TRUE(used);

  write_text(db_path, "garbage garbage garbage garbage garbage garbage garbage garbage");
  history = minigit::file_history_with_index(db_path, store, c2, "f", 10, &used);
  EXPECT_FALSE(used);
  EXPECT_EQ(history.size(), 2u);
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
