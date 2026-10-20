#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/status.hpp"
#include "minigit/tree.hpp"
#include "test_support.hpp"

namespace {

using minigit::ChangeKind;
using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::StatusEntry;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

using Paths = std::vector<std::string>;
using Entries = std::vector<StatusEntry>;

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

// --- flatten_tree ------------------------------------------------------------------

TEST(FlattenTree, ListsNestedFilesWithFullPaths) {
  TempDir tmp;
  const Repository repo = Repository::init(tmp.path());
  const ObjectStore store(repo.objects_dir());
  auto index = minigit::StagingIndex::load(repo.index_path());
  const std::string a = store.write(ObjectType::Blob, "a\n");
  const std::string b = store.write(ObjectType::Blob, "b\n");
  index.stage("top.txt", {"100644", a});
  index.stage("src/deep/x.sh", {"100755", b});
  index.stage("src/y", {"100644", a});

  const std::string tree = minigit::write_tree_from_index(store, index);
  EXPECT_EQ(minigit::flatten_tree(store, tree), index.entries());
  EXPECT_TRUE(minigit::flatten_tree(store, minigit::write_tree_from_index(
                                               store, minigit::StagingIndex::load(tmp.path() / "none")))
                  .empty());

  EXPECT_THROW(minigit::flatten_tree(store, a), ObjectError);           // a blob
  EXPECT_THROW(minigit::flatten_tree(store, std::string(40, 'c')), ObjectError);  // missing
}

// --- compute_status ----------------------------------------------------------------

class StatusTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ScopedCwd cwd{tmp.path()};

  void SetUp() override {
    ASSERT_EQ(run({"config", "user.name", "Ada Lovelace"}).code, minigit::kExitOk);
    ASSERT_EQ(run({"config", "user.email", "ada@example.com"}).code, minigit::kExitOk);
  }

  void write(const std::string& path, const std::string& text) {
    write_text(tmp.path() / path, text);
  }

  void add(const std::string& path) {
    const auto r = run({"add", path});
    ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  }

  void commit(const std::string& message) {
    const auto r = run({"commit", "-m", message});
    ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  }

  minigit::Status status() const { return minigit::compute_status(repo); }

  std::string short_status() const {
    const auto r = run({"status", "--short"});
    EXPECT_EQ(r.code, minigit::kExitOk) << r.err;
    return r.out;
  }

  // `git status --porcelain` for the same HEAD and worktree, with Git's own
  // index rebuilt from ours so that both compare the same three states.
  std::string git_short_status() const {
    EXPECT_EQ(git_in(tmp.path(), "read-tree --empty").status, 0);
    const auto index = minigit::StagingIndex::load(repo.index_path());
    for (const auto& [path, entry] : index.entries()) {
      EXPECT_EQ(git_in(tmp.path(), "update-index --add --cacheinfo " + entry.mode + "," +
                                       entry.oid + ",\"" + path + "\"")
                    .status,
                0)
          << path;
    }
    const auto r = minigit::test::run_command(
        "git -C " + minigit::test::quote(tmp.path()) +
        " -c core.filemode=true -c core.quotepath=false status --porcelain --no-renames");
    EXPECT_EQ(r.status, 0);
    return r.output;
  }
};

TEST_F(StatusTest, EmptyRepositoryIsClean) {
  const auto s = status();
  EXPECT_EQ(s.branch_ref, "refs/heads/main");
  EXPECT_FALSE(s.has_commits);
  EXPECT_TRUE(s.clean());
}

TEST_F(StatusTest, StagedChangesCompareHeadWithIndex) {
  write("keep", "k\n");
  write("change", "1\n");
  write("remove", "r\n");
  add(".");
  commit("base");
  EXPECT_TRUE(status().clean());

  write("change", "2\n");
  write("new", "n\n");
  fs::remove(tmp.path() / "remove");
  add(".");
  const auto s = status();
  EXPECT_TRUE(s.has_commits);
  EXPECT_EQ(s.staged, (Entries{{"change", ChangeKind::Modified},
                               {"new", ChangeKind::Added},
                               {"remove", ChangeKind::Deleted}}));
  EXPECT_TRUE(s.unstaged.empty());
  EXPECT_TRUE(s.untracked.empty());
}

TEST_F(StatusTest, BeforeTheFirstCommitEverythingStagedIsNew) {
  write("a", "a\n");
  write("d/b", "b\n");
  add(".");
  EXPECT_EQ(status().staged, (Entries{{"a", ChangeKind::Added}, {"d/b", ChangeKind::Added}}));
}

TEST_F(StatusTest, UnstagedChangesCompareIndexWithWorktree) {
  write("mod", "1\n");
  write("del", "d\n");
  write("same", "s\n");
  write("dir/inner", "i\n");
  add(".");
  commit("base");

  write("mod", "2\n");
  fs::remove(tmp.path() / "del");
  fs::remove_all(tmp.path() / "dir");
  const auto s = status();
  EXPECT_TRUE(s.staged.empty());
  EXPECT_EQ(s.unstaged, (Entries{{"del", ChangeKind::Deleted},
                                 {"dir/inner", ChangeKind::Deleted},
                                 {"mod", ChangeKind::Modified}}));
}

TEST_F(StatusTest, SameSizeEditsAreDetectedByContent) {
  write("f", "aaaa\n");
  add("f");
  commit("base");
  write("f", "bbbb\n");  // no stat cache, so the content hash decides
  EXPECT_EQ(status().unstaged, (Entries{{"f", ChangeKind::Modified}}));
  write("f", "aaaa\n");
  EXPECT_TRUE(status().clean());
}

#ifndef _WIN32
TEST_F(StatusTest, ExecutableBitChangesAreModifications) {
  write("run.sh", "echo hi\n");
  add("run.sh");
  commit("base");
  fs::permissions(tmp.path() / "run.sh", fs::perms::owner_exec, fs::perm_options::add);
  EXPECT_EQ(status().unstaged, (Entries{{"run.sh", ChangeKind::Modified}}));
  add("run.sh");
  EXPECT_EQ(status().staged, (Entries{{"run.sh", ChangeKind::Modified}}));
  EXPECT_TRUE(status().unstaged.empty());
}

TEST_F(StatusTest, SymbolicLinksAreTypeChangesOrUntracked) {
  write("f", "x\n");
  write("d/g", "y\n");
  add(".");
  commit("base");
  fs::remove(tmp.path() / "f");
  fs::create_symlink("d/g", tmp.path() / "f");
  fs::create_symlink(tmp.path() / "d", tmp.path() / "link");
  const auto s = status();
  EXPECT_EQ(s.unstaged, (Entries{{"f", ChangeKind::TypeChanged}}));
  EXPECT_EQ(s.untracked, Paths{"link"});

  // A tracked file reached through a symbolic link is no longer in the worktree.
  fs::rename(tmp.path() / "d", tmp.path() / "real");
  fs::create_symlink("real", tmp.path() / "d");
  EXPECT_EQ(status().unstaged,
            (Entries{{"d/g", ChangeKind::Deleted}, {"f", ChangeKind::TypeChanged}}));
}
#endif

TEST_F(StatusTest, FileReplacedByDirectoryIsDeletedPlusUntracked) {
  write("x", "file\n");
  add("x");
  commit("base");
  fs::remove(tmp.path() / "x");
  write("x/inside", "now a dir\n");
  const auto s = status();
  EXPECT_EQ(s.unstaged, (Entries{{"x", ChangeKind::Deleted}}));
  EXPECT_EQ(s.untracked, Paths{"x/"});
}

TEST_F(StatusTest, UntrackedDirectoriesAreCollapsed) {
  write("tracked/a", "a\n");
  add("tracked");
  commit("base");
  write("tracked/new", "n\n");
  write("tracked/sub/deeper/f", "f\n");
  write("fresh/one", "1\n");
  write("fresh/two/three", "3\n");
  write("loose.txt", "l\n");
  fs::create_directories(tmp.path() / "empty" / "nested");
  write("nested-repo/.git/HEAD", "ref: refs/heads/main\n");  // only metadata: not listed
  EXPECT_EQ(status().untracked,
            (Paths{"fresh/", "loose.txt", "tracked/new", "tracked/sub/"}));
}

TEST_F(StatusTest, StagedDeletionStillOnDiskIsUntrackedAgain) {
  write("f", "x\n");
  add("f");
  commit("base");
  auto index = minigit::StagingIndex::load(repo.index_path());
  index.remove("f");
  index.save();
  const auto s = status();
  EXPECT_EQ(s.staged, (Entries{{"f", ChangeKind::Deleted}}));
  EXPECT_EQ(s.untracked, Paths{"f"});
}

TEST_F(StatusTest, ShortFormatCombinesStagedAndUnstagedCodes) {
  write("both", "1\n");
  write("gone", "g\n");
  add(".");
  commit("base");
  write("both", "2\n");
  write("added-then-deleted", "x\n");
  add(".");
  write("both", "3\n");
  fs::remove(tmp.path() / "added-then-deleted");
  fs::remove(tmp.path() / "gone");
  write("zzz", "u\n");
  EXPECT_EQ(short_status(), "AD added-then-deleted\nMM both\n D gone\n?? zzz\n");
}

TEST_F(StatusTest, LongFormat) {
  auto r = run({"status"});
  EXPECT_EQ(r.out,
            "On branch main\n\nNo commits yet\n\n"
            "nothing to commit (create/copy files and use \"minigit add\" to track)\n");

  write("a", "1\n");
  write("b", "b\n");
  add(".");
  r = run({"status"});
  EXPECT_EQ(r.out,
            "On branch main\n\nNo commits yet\n\n"
            "Changes to be committed:\n"
            "\tnew file:   a\n"
            "\tnew file:   b\n\n");

  commit("base");
  EXPECT_EQ(run({"status"}).out, "On branch main\n\nnothing to commit, working tree clean\n");

  write("a", "2\n");
  fs::remove(tmp.path() / "b");
  write("c", "c\n");
  r = run({"status"});
  EXPECT_EQ(r.code, minigit::kExitOk);
  EXPECT_EQ(r.out,
            "On branch main\n\n"
            "Changes not staged for commit:\n"
            "  (use \"minigit add <file>...\" to update what will be committed)\n"
            "\tmodified:   a\n"
            "\tdeleted:    b\n\n"
            "Untracked files:\n"
            "  (use \"minigit add <file>...\" to include in what will be committed)\n"
            "\tc\n\n"
            "no changes added to commit (use \"minigit add\")\n");

  add("a");
  add("b");
  fs::remove(tmp.path() / "c");
  write("u", "u\n");
  fs::remove(tmp.path() / "a");
  write("a", "2\n");
  r = run({"status"});
  EXPECT_EQ(r.out,
            "On branch main\n\n"
            "Changes to be committed:\n"
            "\tmodified:   a\n"
            "\tdeleted:    b\n\n"
            "Untracked files:\n"
            "  (use \"minigit add <file>...\" to include in what will be committed)\n"
            "\tu\n\n");
}

TEST_F(StatusTest, WorksFromSubdirectoryWithRootRelativePaths) {
  write("sub/f", "1\n");
  add("sub");
  commit("base");
  write("sub/f", "2\n");
  ScopedCwd sub(tmp.path() / "sub");
  EXPECT_EQ(short_status(), " M sub/f\n");
}

TEST_F(StatusTest, UsageAndRepositoryErrors) {
  EXPECT_EQ(run({"status", "-v"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"status", "file"}).code, minigit::kExitUsage);
  write_text(tmp.path() / ".git" / "minigit" / "index", "garbage\n");
  const auto r = run({"status"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("index"), std::string::npos);
}

TEST_F(StatusTest, ShortFormatMatchesGitPorcelain) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  write("README.md", "# demo\n");
  write("src/main.cpp", "int main() {}\n");
  write("src/old.cpp", "old\n");
  write("docs/guide.md", "guide\n");
  add(".");
  commit("base");

  write("README.md", "# demo v2\n");             // staged modification
  add("README.md");
  write("src/main.cpp", "int main() { return 1; }\n");  // unstaged modification
  fs::remove(tmp.path() / "src" / "old.cpp");    // unstaged deletion
  write("src/new.cpp", "new\n");                 // staged addition, then edited
  add("src/new.cpp");
  write("src/new.cpp", "newer\n");
  fs::remove_all(tmp.path() / "docs");           // staged deletion of a directory
  add("docs");
  write("notes.txt", "untracked\n");
  write("build/out/a.o", "obj\n");

  const std::string ours = short_status();
  EXPECT_EQ(ours, git_short_status());
  EXPECT_EQ(ours,
            "M  README.md\n"
            "D  docs/guide.md\n"
            " M src/main.cpp\n"
            "AM src/new.cpp\n"
            " D src/old.cpp\n"
            "?? build/\n"
            "?? notes.txt\n");
}

// --- scenarios from the roadmap, each checked against real Git ----------------------

class StatusScenarioTest : public StatusTest {
 protected:
  void SetUp() override {
    StatusTest::SetUp();
    write("README.md", "# demo\n");
    write("src/app.cpp", "int main() {}\n");
    write("src/util/strings.hpp", "#pragma once\n");
    write("docs/notes.md", "notes\n");
    add(".");
    commit("base");
  }

  // Runs the real executable in its own process, so the result also shows
  // that status reads only what earlier processes left on disk.
  std::string exe_short_status() const {
    const auto r = minigit::test::run_command("cd " + minigit::test::quote(tmp.path()) + " && " +
                                              minigit::test::quote(MINIGIT_EXE) + " status -s");
    EXPECT_EQ(r.status, 0);
    return r.output;
  }

  void expect_short(const std::string& expected) {
    EXPECT_EQ(short_status(), expected);
    EXPECT_EQ(exe_short_status(), expected);
    if (git_available()) EXPECT_EQ(git_short_status(), expected);
  }
};

TEST_F(StatusScenarioTest, NoChanges) {
  const auto s = status();
  EXPECT_TRUE(s.clean());
  expect_short("");
  EXPECT_EQ(run({"status"}).out, "On branch main\n\nnothing to commit, working tree clean\n");

  // Rewriting a file with identical content is still no change.
  write("src/app.cpp", "int main() {}\n");
  expect_short("");
  // So is an empty directory, which Git cannot track either.
  fs::create_directories(tmp.path() / "empty");
  expect_short("");
}

TEST_F(StatusScenarioTest, StagedOnly) {
  write("README.md", "# demo v2\n");
  write("src/new.cpp", "void f() {}\n");
  fs::remove(tmp.path() / "docs" / "notes.md");
  add("README.md");
  add("src/new.cpp");
  add("docs");

  const auto s = status();
  EXPECT_EQ(s.staged, (Entries{{"README.md", ChangeKind::Modified},
                               {"docs/notes.md", ChangeKind::Deleted},
                               {"src/new.cpp", ChangeKind::Added}}));
  EXPECT_TRUE(s.unstaged.empty());
  EXPECT_TRUE(s.untracked.empty());
  expect_short("M  README.md\nD  docs/notes.md\nA  src/new.cpp\n");

  // Committing the staged changes makes the tree clean again.
  commit("update");
  expect_short("");
}

TEST_F(StatusScenarioTest, UnstagedOnly) {
  write("src/app.cpp", "int main() { return 2; }\n");
  fs::remove(tmp.path() / "src" / "util" / "strings.hpp");

  const auto s = status();
  EXPECT_TRUE(s.staged.empty());
  EXPECT_EQ(s.unstaged, (Entries{{"src/app.cpp", ChangeKind::Modified},
                                 {"src/util/strings.hpp", ChangeKind::Deleted}}));
  EXPECT_TRUE(s.untracked.empty());
  expect_short(" M src/app.cpp\n D src/util/strings.hpp\n");

  // Restoring the original content removes the change.
  write("src/app.cpp", "int main() {}\n");
  expect_short(" D src/util/strings.hpp\n");
}

TEST_F(StatusScenarioTest, UntrackedOnly) {
  write("todo.txt", "t\n");
  write("src/util/extra.hpp", "e\n");
  write("assets/img/logo.svg", "<svg/>\n");
  const auto s = status();
  EXPECT_TRUE(s.staged.empty());
  EXPECT_TRUE(s.unstaged.empty());
  EXPECT_EQ(s.untracked, (Paths{"assets/", "src/util/extra.hpp", "todo.txt"}));
  expect_short("?? assets/\n?? src/util/extra.hpp\n?? todo.txt\n");
}

TEST_F(StatusScenarioTest, Mixed) {
  write("README.md", "# staged\n");  // staged, then modified again
  add("README.md");
  write("README.md", "# staged and edited\n");
  write("src/app.cpp", "int main() { return 3; }\n");  // unstaged only
  write("src/added.cpp", "added\n");                    // added, then deleted on disk
  add("src/added.cpp");
  fs::remove(tmp.path() / "src" / "added.cpp");
  fs::remove(tmp.path() / "docs" / "notes.md");  // staged deletion
  add("docs/notes.md");
  write("docs/notes.md", "back\n");  // ... but a new file is there again
  write("scratch/tmp.txt", "u\n");   // untracked directory

  const auto s = status();
  EXPECT_EQ(s.staged, (Entries{{"README.md", ChangeKind::Modified},
                               {"docs/notes.md", ChangeKind::Deleted},
                               {"src/added.cpp", ChangeKind::Added}}));
  EXPECT_EQ(s.unstaged, (Entries{{"README.md", ChangeKind::Modified},
                                 {"src/added.cpp", ChangeKind::Deleted},
                                 {"src/app.cpp", ChangeKind::Modified}}));
  EXPECT_EQ(s.untracked, (Paths{"docs/", "scratch/"}));
  expect_short(
      "MM README.md\n"
      "D  docs/notes.md\n"
      "AD src/added.cpp\n"
      " M src/app.cpp\n"
      "?? docs/\n"
      "?? scratch/\n");

  const auto r = run({"status"});
  EXPECT_EQ(r.out,
            "On branch main\n\n"
            "Changes to be committed:\n"
            "\tmodified:   README.md\n"
            "\tdeleted:    docs/notes.md\n"
            "\tnew file:   src/added.cpp\n\n"
            "Changes not staged for commit:\n"
            "  (use \"minigit add <file>...\" to update what will be committed)\n"
            "\tmodified:   README.md\n"
            "\tdeleted:    src/added.cpp\n"
            "\tmodified:   src/app.cpp\n\n"
            "Untracked files:\n"
            "  (use \"minigit add <file>...\" to include in what will be committed)\n"
            "\tdocs/\n"
            "\tscratch/\n\n");
}

}  // namespace
