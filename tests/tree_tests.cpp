#include <gtest/gtest.h>

#include "minigit/add.hpp"
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
using minigit::TreeEntry;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

constexpr const char* kEmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

class TreeBuildTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  StagingIndex index = StagingIndex::load(repo.index_path());

  // Stages `text` at `path` (writing its blob) and returns the blob id.
  std::string stage(const std::string& path, const std::string& text,
                    std::string_view mode = minigit::kModeFile) {
    const std::string oid = store.write(ObjectType::Blob, text);
    index.stage(path, {std::string(mode), oid});
    return oid;
  }

  std::vector<TreeEntry> read_tree(const std::string& oid) const {
    const auto obj = store.read(oid);
    EXPECT_EQ(obj.type, ObjectType::Tree);
    return minigit::parse_tree(obj.content);
  }

  // The id of the entry called `name` in tree `oid`, or "" if absent.
  std::string child(const std::string& oid, const std::string& name) const {
    for (const auto& e : read_tree(oid)) {
      if (e.name == name) return e.oid;
    }
    return "";
  }
};

TEST_F(TreeBuildTest, EmptyIndexGivesEmptyTree) {
  const std::string root = minigit::write_tree_from_index(store, index);
  EXPECT_EQ(root, kEmptyTree);
  EXPECT_TRUE(store.contains(root));  // written, so a commit can refer to it
}

TEST_F(TreeBuildTest, FlatFilesKeepModes) {
  const std::string a = stage("a.txt", "a\n");
  const std::string run = stage("run.sh", "#!/bin/sh\n", minigit::kModeExecutable);
  const auto entries = read_tree(minigit::write_tree_from_index(store, index));
  EXPECT_EQ(entries, (std::vector<TreeEntry>{{"100644", "a.txt", a}, {"100755", "run.sh", run}}));
}

TEST_F(TreeBuildTest, BuildsNestedDirectories) {
  const std::string top = stage("top", "t\n");
  const std::string deep = stage("src/lib/deep/file.cpp", "d\n");
  const std::string main = stage("src/main.cpp", "m\n");
  const std::string lib = stage("src/lib/util.hpp", "u\n");

  const std::string root = minigit::write_tree_from_index(store, index);
  const auto root_entries = read_tree(root);
  ASSERT_EQ(root_entries.size(), 2u);
  EXPECT_EQ(root_entries[0].name, "src");
  EXPECT_TRUE(root_entries[0].is_tree());
  EXPECT_EQ(root_entries[1], (TreeEntry{"100644", "top", top}));

  const std::string src = child(root, "src");
  const std::string lib_tree = child(src, "lib");
  EXPECT_EQ(child(src, "main.cpp"), main);
  EXPECT_EQ(child(lib_tree, "util.hpp"), lib);
  EXPECT_EQ(child(child(lib_tree, "deep"), "file.cpp"), deep);
  EXPECT_EQ(read_tree(src).size(), 2u);
  EXPECT_EQ(read_tree(lib_tree).size(), 2u);
}

TEST_F(TreeBuildTest, UsesGitOrderingForDirectories) {
  // The directory "a" must sort as "a/": after "a-b" and "a.txt", before "a0".
  stage("a0", "0\n");
  stage("a/x", "x\n");
  stage("a.txt", "t\n");
  stage("a-b", "-\n");
  stage("b", "b\n");
  stage("ab/y", "y\n");
  std::vector<std::string> names;
  for (const auto& e : read_tree(minigit::write_tree_from_index(store, index))) {
    names.push_back(e.name);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"a-b", "a.txt", "a", "a0", "ab", "b"}));
}

TEST_F(TreeBuildTest, SameContentGivesSameIdRegardlessOfStagingOrder) {
  stage("x/1", "1\n");
  stage("y", "y\n");
  stage("x/2", "2\n");
  const std::string first = minigit::write_tree_from_index(store, index);
  EXPECT_EQ(minigit::write_tree_from_index(store, index), first);

  TempDir other_dir;
  const Repository other = Repository::init(other_dir.path());
  const ObjectStore other_store(other.objects_dir());
  StagingIndex other_index = StagingIndex::load(other.index_path());
  for (const auto& [path, text] : std::vector<std::pair<std::string, std::string>>{
           {"x/2", "2\n"}, {"y", "y\n"}, {"x/1", "1\n"}}) {
    other_index.stage(path, {"100644", other_store.write(ObjectType::Blob, text)});
  }
  EXPECT_EQ(minigit::write_tree_from_index(other_store, other_index), first);

  stage("x/1", "changed\n");
  EXPECT_NE(minigit::write_tree_from_index(store, index), first);
}

// Stages a worktree with MiniGit++'s `add` and with real Git, and checks that
// our tree id matches `git write-tree`.
TEST_F(TreeBuildTest, TreeIdMatchesGitWriteTree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  const fs::path w = tmp.path();
  write_text(w / "README.md", "# demo\n");
  write_text(w / "a.txt", "dot\n");
  write_text(w / "a-b", "dash\n");
  write_text(w / "a0", "zero\n");
  write_text(w / "a" / "inner.txt", "inner\n");
  write_text(w / "a" / "b" / "c" / "deep.txt", "deep\n");
  write_text(w / "dir with space" / "file name.txt", "spaces\n");
  write_text(w / "empty", "");
  write_text(w / "bin" / "data.bin", std::string("\0\x01\xff\n", 4));
  fs::create_directories(w / "empty-dir");  // neither Git nor MiniGit++ records it
#ifndef _WIN32
  write_text(w / "bin" / "run.sh", "#!/bin/sh\necho hi\n");
  fs::permissions(w / "bin" / "run.sh", fs::perms::owner_exec, fs::perm_options::add);
#endif

  minigit::add_paths(repo, index, {"."}, w);
  const std::string ours = minigit::write_tree_from_index(store, index);

  // filemode=true so Git records the executable bit as `add` does.
  ASSERT_EQ(git_in(w, "-c core.filemode=true add -A .").status, 0);
  const auto theirs = git_in(w, "write-tree");
  ASSERT_EQ(theirs.status, 0);
  EXPECT_EQ(ours, theirs.output);
  EXPECT_EQ(git_in(w, "ls-tree -r " + ours).output, git_in(w, "ls-tree -r " + theirs.output).output);
}

TEST_F(TreeBuildTest, StagedDeletionDisappearsFromTree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  const fs::path w = tmp.path();
  write_text(w / "keep.txt", "keep\n");
  write_text(w / "gone" / "old.txt", "old\n");
  minigit::add_paths(repo, index, {"."}, w);
  fs::remove_all(w / "gone");
  minigit::add_paths(repo, index, {"gone"}, w);

  const std::string ours = minigit::write_tree_from_index(store, index);
  EXPECT_EQ(child(ours, "gone"), "");
  ASSERT_EQ(git_in(w, "add -A .").status, 0);
  EXPECT_EQ(ours, git_in(w, "write-tree").output);
}

}  // namespace
