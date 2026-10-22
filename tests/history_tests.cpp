#include <gtest/gtest.h>

#include <map>
#include <optional>

#include "minigit/commit.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"
#include "test_support.hpp"

namespace {

using minigit::ChangeKind;
using minigit::Commit;
using minigit::FileChange;
using minigit::IndexEntry;
using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::Signature;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::TempDir;

using Files = std::map<std::string, std::optional<std::string>>;

class HistoryTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0000"};

  std::string blob(const std::string& text) { return store.write(ObjectType::Blob, text); }

  // Stages `files` (an empty optional unstages the path) and commits.
  std::string commit(const Files& files, const std::string& message) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    for (const auto& [path, text] : files) {
      if (text) {
        index.stage(path, {"100644", blob(*text)});
      } else {
        index.remove(path);
      }
    }
    index.save();
    author.timestamp += 60;
    return minigit::create_commit(repo, message, author).oid;
  }

  std::string tree_of(const std::string& commit_oid) {
    return minigit::parse_commit(store.read(commit_oid).content).tree;
  }

  std::vector<std::string> history_ids(const std::string& start, const std::string& path,
                                       std::size_t limit = 100) {
    std::vector<std::string> ids;
    for (const auto& e : minigit::file_history(store, start, path, limit)) ids.push_back(e.oid);
    return ids;
  }

  // "M\tpath" lines like `git diff --name-status`.
  static std::string name_status(const std::vector<FileChange>& changes) {
    std::string out;
    for (const auto& c : changes) {
      if (!out.empty()) out += '\n';
      out += c.kind == ChangeKind::Added ? 'A' : c.kind == ChangeKind::Deleted ? 'D' : 'M';
      out += '\t' + c.path;
    }
    return out;
  }
};

TEST(ChangeKindTest, Names) {
  EXPECT_EQ(minigit::to_string(ChangeKind::Added), "added");
  EXPECT_EQ(minigit::to_string(ChangeKind::Modified), "modified");
  EXPECT_EQ(minigit::to_string(ChangeKind::Deleted), "deleted");
}

TEST_F(HistoryTest, DiffAgainstNothingListsEveryFile) {
  const std::string c1 = commit({{"b.txt", "b"}, {"a/x.txt", "x"}}, "one");
  const auto changes = minigit::diff_trees(store, std::nullopt, tree_of(c1));
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0], (FileChange{"a/x.txt", ChangeKind::Added, std::nullopt,
                                    IndexEntry{"100644", blob("x")}}));
  EXPECT_EQ(changes[1].path, "b.txt");
}

TEST_F(HistoryTest, DiffSameTreeIsEmpty) {
  const std::string c1 = commit({{"a.txt", "a"}}, "one");
  EXPECT_TRUE(minigit::diff_trees(store, tree_of(c1), tree_of(c1)).empty());
}

TEST_F(HistoryTest, DiffReportsAddedModifiedDeletedInPathOrder) {
  const std::string c1 =
      commit({{"keep.txt", "k"}, {"mod.txt", "1"}, {"gone.txt", "g"}, {"d/sub/f", "f"}}, "one");
  const std::string c2 = commit(
      {{"mod.txt", "2"}, {"gone.txt", std::nullopt}, {"new.txt", "n"}, {"d/sub/f", "F"}}, "two");
  const auto changes = minigit::diff_trees(store, tree_of(c1), tree_of(c2));
  EXPECT_EQ(name_status(changes), "M\td/sub/f\nD\tgone.txt\nM\tmod.txt\nA\tnew.txt");
  EXPECT_EQ(changes[2].old_entry->oid, blob("1"));
  EXPECT_EQ(changes[2].new_entry->oid, blob("2"));
  EXPECT_FALSE(changes[1].new_entry);
}

TEST_F(HistoryTest, DiffModeChangeIsModification) {
  const std::string c1 = commit({{"run.sh", "echo"}}, "one");
  auto index = minigit::StagingIndex::load(repo.index_path());
  index.stage("run.sh", {"100755", blob("echo")});
  index.save();
  author.timestamp += 60;
  const std::string c2 = minigit::create_commit(repo, "exec", author).oid;
  const auto changes = minigit::diff_trees(store, tree_of(c1), tree_of(c2));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].kind, ChangeKind::Modified);
  EXPECT_EQ(changes[0].old_entry->mode, "100644");
  EXPECT_EQ(changes[0].new_entry->mode, "100755");
}

TEST_F(HistoryTest, DiffFileReplacedByDirectory) {
  const std::string c1 = commit({{"a", "file"}, {"a.txt", "t"}}, "one");
  const std::string c2 = commit({{"a/inner", "i"}}, "two");  // staging a/inner drops file a
  EXPECT_EQ(name_status(minigit::diff_trees(store, tree_of(c1), tree_of(c2))), "D\ta\nA\ta/inner");
  EXPECT_EQ(name_status(minigit::diff_trees(store, tree_of(c2), tree_of(c1))), "A\ta\nD\ta/inner");
}

TEST_F(HistoryTest, DiffSkipsUnchangedSubtreesWithoutReadingThem) {
  const std::string c1 = commit({{"big/x", "x"}, {"top", "1"}}, "one");
  const std::string c2 = commit({{"top", "2"}}, "two");
  // Delete the shared subtree: the diff must still succeed because it never
  // needs to open it.
  const auto big = minigit::lookup_path(store, tree_of(c1), "big/x");
  ASSERT_TRUE(big);
  std::string big_tree;
  for (const auto& e : minigit::parse_tree(store.read(tree_of(c1)).content)) {
    if (e.name == "big") big_tree = e.oid;
  }
  std::filesystem::remove(store.object_path(big_tree));
  EXPECT_EQ(name_status(minigit::diff_trees(store, tree_of(c1), tree_of(c2))), "M\ttop");
}

TEST_F(HistoryTest, DiffMissingTreeThrows) {
  EXPECT_THROW(minigit::diff_trees(store, std::nullopt, std::string(40, 'a')), ObjectError);
}

TEST_F(HistoryTest, LookupPath) {
  const std::string c1 = commit({{"src/main.cpp", "int main"}, {"README", "r"}}, "one");
  const std::string tree = tree_of(c1);
  EXPECT_EQ(minigit::lookup_path(store, tree, "src/main.cpp"),
            (IndexEntry{"100644", blob("int main")}));
  EXPECT_EQ(minigit::lookup_path(store, tree, "README"), (IndexEntry{"100644", blob("r")}));
  EXPECT_FALSE(minigit::lookup_path(store, tree, "src"));  // a directory
  EXPECT_FALSE(minigit::lookup_path(store, tree, "src/missing"));
  EXPECT_FALSE(minigit::lookup_path(store, tree, "README/below"));  // through a file
  EXPECT_FALSE(minigit::lookup_path(store, tree, "nope/main.cpp"));
}

TEST_F(HistoryTest, FileHistoryFollowsAddModifyDeleteReadd) {
  const std::string c1 = commit({{"f.txt", "1"}, {"other", "o"}}, "add");
  commit({{"other", "o2"}}, "unrelated");
  const std::string c3 = commit({{"f.txt", "2"}}, "modify");
  const std::string c4 = commit({{"f.txt", std::nullopt}}, "delete");
  commit({{"other", "o3"}}, "unrelated again");
  const std::string c6 = commit({{"f.txt", "3"}}, "re-add");

  const auto history = minigit::file_history(store, c6, "f.txt", 100);
  ASSERT_EQ(history.size(), 4u);
  EXPECT_EQ(history[0].oid, c6);
  EXPECT_EQ(history[0].change.kind, ChangeKind::Added);
  EXPECT_EQ(history[1].oid, c4);
  EXPECT_EQ(history[1].change.kind, ChangeKind::Deleted);
  EXPECT_EQ(history[1].change.old_entry->oid, blob("2"));
  EXPECT_EQ(history[2].oid, c3);
  EXPECT_EQ(history[2].change.kind, ChangeKind::Modified);
  EXPECT_EQ(history[2].change.old_entry->oid, blob("1"));
  EXPECT_EQ(history[2].change.new_entry->oid, blob("2"));
  EXPECT_EQ(history[3].oid, c1);
  EXPECT_EQ(history[3].change.kind, ChangeKind::Added);
  EXPECT_EQ(history[3].commit.message, "add\n");

  EXPECT_EQ(history_ids(c6, "f.txt", 2), (std::vector<std::string>{c6, c4}));
  EXPECT_TRUE(history_ids(c6, "f.txt", 0).empty());
  EXPECT_TRUE(history_ids(c6, "never.txt").empty());
}

TEST_F(HistoryTest, FileHistoryInSubdirectoryIgnoresSiblings) {
  const std::string c1 = commit({{"src/a.cpp", "a"}, {"src/b.cpp", "b"}}, "one");
  commit({{"src/b.cpp", "b2"}}, "touch sibling");
  const std::string c3 = commit({{"src/a.cpp", "a2"}}, "touch a");
  EXPECT_EQ(history_ids(c3, "src/a.cpp"), (std::vector<std::string>{c3, c1}));
}

TEST_F(HistoryTest, FileHistoryAttributesMergedChangesToTheMerge) {
  const std::string base = commit({{"f", "1"}}, "base");
  // A side commit changing f, and a merge whose first parent is `base`.
  Commit side;
  side.tree =
      store.write(ObjectType::Tree, minigit::serialize_tree({{"100644", "f", blob("side")}}));
  side.parents = {base};
  side.author = side.committer = author;
  side.message = "side\n";
  const std::string side_oid = store.write(ObjectType::Commit, minigit::serialize_commit(side));
  Commit merge = side;
  merge.parents = {base, side_oid};
  merge.message = "merge\n";
  const std::string merge_oid = store.write(ObjectType::Commit, minigit::serialize_commit(merge));

  EXPECT_EQ(history_ids(merge_oid, "f"), (std::vector<std::string>{merge_oid, base}));
}

TEST_F(HistoryTest, FileHistoryMatchesGitOracle) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  commit({{"src/main.cpp", "1"}, {"README", "r"}}, "one");
  commit({{"README", "r2"}}, "two");
  commit({{"src/main.cpp", "2"}, {"src/util.cpp", "u"}}, "three");
  commit({{"src/main.cpp", std::nullopt}}, "four");
  commit({{"src/util.cpp", "u2"}}, "five");
  const std::string head = commit({{"src/main.cpp", "3"}}, "six");

  for (const std::string path : {"src/main.cpp", "README", "src/util.cpp", "missing"}) {
    const auto git = git_in(tmp.path(), "log --format=%H -- " + path);
    ASSERT_EQ(git.status, 0);
    std::string ours;
    for (const auto& id : history_ids(head, path)) ours += (ours.empty() ? "" : "\n") + id;
    EXPECT_EQ(ours, git.output) << path;
  }
}

TEST_F(HistoryTest, DiffMatchesGitOracle) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  const std::string c1 =
      commit({{"a.txt", "a"}, {"dir/b.txt", "b"}, {"dir/sub/c.txt", "c"}, {"z", "z"}}, "one");
  commit({{"a.txt", "A"}, {"dir/sub/c.txt", std::nullopt}, {"dir/new.txt", "n"}}, "two");
  const std::string c3 = commit({{"z", std::nullopt}, {"z/inside", "i"}}, "three");

  for (const auto& [from, to] : {std::pair{c1, c3}, std::pair{c3, c1}}) {
    const auto git = git_in(tmp.path(), "diff --no-renames --name-status " + from + " " + to);
    ASSERT_EQ(git.status, 0);
    EXPECT_EQ(name_status(minigit::diff_trees(store, tree_of(from), tree_of(to))), git.output);
  }
}

}  // namespace
