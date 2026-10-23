#include <gtest/gtest.h>

#include <sstream>

#include "minigit/checkout.hpp"
#include "minigit/cli.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/status.hpp"
#include "test_support.hpp"

namespace {

using minigit::CheckoutError;
using minigit::ObjectError;
using minigit::Repository;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::read_text;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

using Paths = std::vector<std::string>;

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

class CheckoutTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ScopedCwd cwd{tmp.path()};

  void SetUp() override {
    ASSERT_EQ(run({"config", "user.name", "Ada Lovelace"}).code, minigit::kExitOk);
    ASSERT_EQ(run({"config", "user.email", "ada@example.com"}).code, minigit::kExitOk);
  }

  fs::path at(const std::string& path) const { return tmp.path() / path; }
  void write(const std::string& path, const std::string& text) { write_text(at(path), text); }
  std::string read(const std::string& path) const { return read_text(at(path)); }

  void cli(const std::vector<std::string>& args) {
    const auto r = run(args);
    ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  }
  void add(const std::string& path) { cli({"add", path}); }
  void commit(const std::string& message) { cli({"commit", "-m", message}); }
  void branch(const std::string& name) { cli({"branch", name}); }

  std::string head() const { return repo.head_ref(); }
  Paths staged_paths() const {
    Paths paths;
    const auto index = minigit::StagingIndex::load(repo.index_path());
    for (const auto& [path, entry] : index.entries()) {
      paths.push_back(path);
    }
    return paths;
  }

  // main: "shared" and "main-only"; topic: "shared" changed, "topic-only" added.
  void two_branches() {
    write("shared", "base\n");
    write("main-only", "m\n");
    add(".");
    commit("base");
    branch("topic");
    cli({"checkout", "topic"});
    write("shared", "topic\n");
    write("topic-only", "t\n");
    fs::remove(at("main-only"));
    add(".");
    commit("topic work");
    cli({"checkout", "main"});
  }

  // Runs `checkout` expecting a refusal that mentions `path`, and checks
  // that HEAD did not move.
  std::string refused(const std::string& branch, const std::string& path) {
    const std::string before = head();
    const auto r = run({"checkout", branch});
    EXPECT_EQ(r.code, minigit::kExitError);
    EXPECT_NE(r.err.find("\t" + path + "\n"), std::string::npos) << r.err;
    EXPECT_EQ(head(), before);
    return r.err;
  }
};

TEST_F(CheckoutTest, SwitchUpdatesWorktreeIndexAndHead) {
  two_branches();
  EXPECT_EQ(head(), "refs/heads/main");
  EXPECT_EQ(read("shared"), "base\n");
  EXPECT_TRUE(fs::exists(at("main-only")));
  EXPECT_FALSE(fs::exists(at("topic-only")));

  const auto r = run({"checkout", "topic"});
  ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_EQ(r.err, "Switched to branch 'topic'\n");
  EXPECT_EQ(head(), "refs/heads/topic");
  EXPECT_EQ(read("shared"), "topic\n");
  EXPECT_EQ(read("topic-only"), "t\n");
  EXPECT_FALSE(fs::exists(at("main-only")));
  EXPECT_EQ(staged_paths(), (Paths{"shared", "topic-only"}));
  EXPECT_TRUE(minigit::compute_status(repo).clean());
}

TEST_F(CheckoutTest, ResultListsWrittenAndRemovedFiles) {
  two_branches();
  const auto result = minigit::checkout_branch(repo, "topic");
  EXPECT_FALSE(result.already_on);
  EXPECT_EQ(result.branch_ref, "refs/heads/topic");
  EXPECT_EQ(result.written, (Paths{"shared", "topic-only"}));
  EXPECT_EQ(result.removed, (Paths{"main-only"}));
}

TEST_F(CheckoutTest, AlreadyOnBranchChangesNothing) {
  two_branches();
  write("shared", "dirty\n");
  const auto r = run({"checkout", "main"});
  EXPECT_EQ(r.code, minigit::kExitOk);
  EXPECT_EQ(r.err, "Already on 'main'\n");
  EXPECT_EQ(read("shared"), "dirty\n");
}

TEST_F(CheckoutTest, RefusesLocalModificationsToChangedFiles) {
  two_branches();
  write("shared", "edited\n");
  const std::string err = refused("topic", "shared");
  EXPECT_NE(err.find("local changes"), std::string::npos) << err;
  EXPECT_EQ(read("shared"), "edited\n");
  EXPECT_TRUE(fs::exists(at("main-only")));  // nothing was touched
}

TEST_F(CheckoutTest, RefusesStagedChangesToChangedFiles) {
  two_branches();
  write("shared", "staged\n");
  add("shared");
  write("shared", "base\n");  // the worktree matches HEAD again; the index does not
  const std::string err = refused("topic", "shared");
  EXPECT_NE(err.find("staged changes"), std::string::npos) << err;
}

TEST_F(CheckoutTest, RefusesStagedDeletionOfChangedFile) {
  two_branches();
  fs::remove(at("main-only"));
  add("main-only");
  refused("topic", "main-only");
}

TEST_F(CheckoutTest, RefusesToOverwriteUntrackedFile) {
  two_branches();
  write("topic-only", "precious\n");
  const std::string err = refused("topic", "topic-only");
  EXPECT_NE(err.find("untracked"), std::string::npos) << err;
  EXPECT_EQ(read("topic-only"), "precious\n");
}

TEST_F(CheckoutTest, KeepsChangesToPathsBothBranchesShare) {
  write("same", "same\n");
  write("other", "1\n");
  add(".");
  commit("base");
  branch("topic");
  write("other", "2\n");
  add("other");
  commit("main changes other");

  write("same", "edited\n");
  write("new-staged", "n\n");
  add("new-staged");
  write("untracked", "u\n");
  cli({"checkout", "topic"});

  EXPECT_EQ(read("same"), "edited\n");
  EXPECT_EQ(read("other"), "1\n");
  EXPECT_EQ(read("untracked"), "u\n");
  EXPECT_EQ(staged_paths(), (Paths{"new-staged", "other", "same"}));
  const auto s = minigit::compute_status(repo);
  EXPECT_EQ(s.staged, (std::vector<minigit::StatusEntry>{{"new-staged", minigit::StatusKind::Added}}));
  EXPECT_EQ(s.unstaged, (std::vector<minigit::StatusEntry>{{"same", minigit::StatusKind::Modified}}));
}

TEST_F(CheckoutTest, MissingTrackedFileIsRestored) {
  two_branches();
  fs::remove(at("shared"));
  cli({"checkout", "topic"});
  EXPECT_EQ(read("shared"), "topic\n");
}

TEST_F(CheckoutTest, ErrorsForUnknownBranchesAndDetachedHead) {
  two_branches();
  auto r = run({"checkout", "nope"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("branch 'nope' not found"), std::string::npos) << r.err;

  const std::string oid = *repo.read_ref("refs/heads/topic");
  r = run({"checkout", oid});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("detach HEAD"), std::string::npos) << r.err;

  r = run({"checkout", "bad..name"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a valid branch name"), std::string::npos) << r.err;

  EXPECT_EQ(run({"checkout"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"checkout", "-b", "x"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"checkout", "main", "topic"}).code, minigit::kExitUsage);
  EXPECT_EQ(head(), "refs/heads/main");
}

TEST_F(CheckoutTest, ExecutableBitFollowsTheBranch) {
#ifdef _WIN32
  GTEST_SKIP() << "no executable bit on Windows";
#else
  write("run.sh", "#!/bin/sh\n");
  add(".");
  commit("plain");
  branch("plain");
  fs::permissions(at("run.sh"), fs::perms::owner_exec, fs::perm_options::add);
  add("run.sh");
  commit("make executable");

  const auto exec = [this] {
    return (fs::status(at("run.sh")).permissions() & fs::perms::owner_exec) != fs::perms::none;
  };
  cli({"checkout", "plain"});
  EXPECT_FALSE(exec());
  cli({"checkout", "main"});
  EXPECT_TRUE(exec());
  EXPECT_TRUE(minigit::compute_status(repo).clean());
#endif
}

TEST_F(CheckoutTest, GitAgreesWithTheResult) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  two_branches();
  cli({"checkout", "topic"});
  // Git reads our HEAD; with its index rebuilt from that commit, a clean
  // `git status` means the worktree holds exactly the branch's files.
  EXPECT_EQ(git_in(tmp.path(), "symbolic-ref HEAD").output, "refs/heads/topic");
  ASSERT_EQ(git_in(tmp.path(), "read-tree HEAD").status, 0);
  const auto st = git_in(tmp.path(), "-c core.filemode=true status --porcelain");
  ASSERT_EQ(st.status, 0);
  EXPECT_EQ(st.output, "");
}

// --- Scenarios: back and forth, nested add/remove, missing refs, unsafe cases ----

TEST_F(CheckoutTest, SwitchingBackAndForthRestoresEachBranch) {
  write("README", "v1\n");
  write("lib/core.cpp", "core v1\n");
  add(".");
  commit("v1");
  branch("feature");

  write("README", "v2\n");
  write("lib/extra.cpp", "extra\n");
  add(".");
  commit("v2 on main");

  cli({"checkout", "feature"});
  write("docs/guide.md", "guide\n");
  fs::remove(at("lib/core.cpp"));
  fs::remove(at("lib"));  // checkout already removed lib/extra.cpp, so it is empty
  add(".");
  commit("feature: docs, drop core");

  const auto expect_main = [this] {
    EXPECT_EQ(head(), "refs/heads/main");
    EXPECT_EQ(read("README"), "v2\n");
    EXPECT_EQ(read("lib/core.cpp"), "core v1\n");
    EXPECT_EQ(read("lib/extra.cpp"), "extra\n");
    EXPECT_FALSE(fs::exists(at("docs")));  // emptied directories are removed
    EXPECT_EQ(staged_paths(), (Paths{"README", "lib/core.cpp", "lib/extra.cpp"}));
  };
  const auto expect_feature = [this] {
    EXPECT_EQ(head(), "refs/heads/feature");
    EXPECT_EQ(read("README"), "v1\n");
    EXPECT_EQ(read("docs/guide.md"), "guide\n");
    EXPECT_FALSE(fs::exists(at("lib")));
    EXPECT_EQ(staged_paths(), (Paths{"README", "docs/guide.md"}));
  };

  expect_feature();
  for (int round = 0; round < 3; ++round) {
    cli({"checkout", "main"});
    expect_main();
    EXPECT_TRUE(minigit::compute_status(repo).clean());
    cli({"checkout", "feature"});
    expect_feature();
    EXPECT_TRUE(minigit::compute_status(repo).clean());
  }

  // Commits keep landing on the checked-out branch.
  write("docs/guide.md", "guide v2\n");
  add(".");
  commit("feature: update guide");
  cli({"checkout", "main"});
  expect_main();
  cli({"checkout", "feature"});
  EXPECT_EQ(read("docs/guide.md"), "guide v2\n");
}

TEST_F(CheckoutTest, FileAndDirectorySwapPlaces) {
  write("thing/inner.txt", "inner\n");
  add(".");
  commit("thing is a directory");
  branch("dir");
  fs::remove_all(at("thing"));
  add("thing");
  write("thing", "now a file\n");
  add("thing");
  commit("thing is a file");

  cli({"checkout", "dir"});
  EXPECT_EQ(read("thing/inner.txt"), "inner\n");
  cli({"checkout", "main"});
  EXPECT_EQ(read("thing"), "now a file\n");
  EXPECT_TRUE(minigit::compute_status(repo).clean());
}

TEST_F(CheckoutTest, RefusesUntrackedFilesInTheWay) {
  write("thing/inner.txt", "inner\n");
  add(".");
  commit("thing is a directory");
  branch("dir");
  fs::remove_all(at("thing"));
  add("thing");
  write("thing", "now a file\n");
  add("thing");
  commit("thing is a file");

  cli({"checkout", "dir"});
  // Back on "dir": "thing/" holds an untracked file the file "thing" would replace.
  write("thing/untracked.txt", "keep me\n");
  refused("main", "thing/untracked.txt");
  EXPECT_EQ(read("thing/untracked.txt"), "keep me\n");
  EXPECT_EQ(read("thing/inner.txt"), "inner\n");
  fs::remove(at("thing/untracked.txt"));
  cli({"checkout", "main"});

  // On "main": a branch that adds "nested/file" while "nested" is an untracked file.
  branch("nested");
  cli({"checkout", "nested"});
  write("nested/file", "n\n");
  add(".");
  commit("add nested/file");
  cli({"checkout", "main"});
  write("nested", "untracked file\n");
  refused("nested", "nested");
  EXPECT_EQ(read("nested"), "untracked file\n");
}

TEST_F(CheckoutTest, RefusesStagedFileWhereTheTargetHasOne) {
  two_branches();
  write("topic-only", "t\n");  // same content as on topic, but only staged here
  add("topic-only");
  const std::string err = refused("topic", "topic-only");
  EXPECT_NE(err.find("staged changes"), std::string::npos) << err;
}

TEST_F(CheckoutTest, RefusalReportsEveryConflict) {
  two_branches();
  write("shared", "edited\n");
  write("topic-only", "untracked\n");
  const std::string err = refused("topic", "shared");
  EXPECT_NE(err.find("\ttopic-only\n"), std::string::npos) << err;
  EXPECT_EQ(read("shared"), "edited\n");
  EXPECT_EQ(read("topic-only"), "untracked\n");
  EXPECT_EQ(staged_paths(), (Paths{"main-only", "shared"}));
}

TEST_F(CheckoutTest, RefusesSymbolicLinksAmongChangedPaths) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  two_branches();
  // MiniGit++ cannot stage symbolic links, so record one with real Git on
  // a commit of its own and point a branch at it.
  const minigit::ObjectStore store(repo.objects_dir());
  const std::string target = store.write(minigit::ObjectType::Blob, "shared");
  ASSERT_EQ(git_in(tmp.path(), "read-tree HEAD").status, 0);
  ASSERT_EQ(git_in(tmp.path(), "update-index --add --cacheinfo 120000," + target + ",link").status,
            0);
  const auto tree = git_in(tmp.path(), "write-tree");
  ASSERT_EQ(tree.status, 0);
  const auto commit = git_in(tmp.path(), "-c user.name=A -c user.email=a@b commit-tree " +
                                             tree.output + " -p HEAD -m link");
  ASSERT_EQ(commit.status, 0);
  repo.write_ref("refs/heads/links", commit.output);

  const std::string err = refused("links", "link");
  EXPECT_NE(err.find("not regular files"), std::string::npos) << err;
  EXPECT_FALSE(fs::exists(fs::symlink_status(at("link"))));
}

TEST_F(CheckoutTest, MissingAndCorruptRefsAndObjects) {
  two_branches();
  // A branch whose ref file was deleted no longer exists.
  fs::remove(at(".git/refs/heads/topic"));
  auto r = run({"checkout", "topic"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("branch 'topic' not found"), std::string::npos) << r.err;

  // A corrupt ref is an error, not a silent switch.
  write(".git/refs/heads/broken", "not-an-id\n");
  r = run({"checkout", "broken"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("malformed"), std::string::npos) << r.err;

  // A ref naming a commit that is not in the object store.
  repo.write_ref("refs/heads/ghost", std::string(40, 'a'));
  r = run({"checkout", "ghost"});
  EXPECT_EQ(r.code, minigit::kExitError);

  EXPECT_EQ(head(), "refs/heads/main");
  EXPECT_EQ(read("shared"), "base\n");
  EXPECT_TRUE(minigit::compute_status(repo).clean());
}

TEST_F(CheckoutTest, MissingBlobFailsBeforeTouchingTheWorktree) {
  two_branches();
  const minigit::ObjectStore store(repo.objects_dir());
  const auto topic = minigit::read_commit_snapshot(store, *repo.read_ref("refs/heads/topic"));
  const std::string oid = topic.at("topic-only").oid;
  fs::remove(at(".git/objects/" + oid.substr(0, 2) + "/" + oid.substr(2)));

  EXPECT_THROW(minigit::checkout_branch(repo, "topic"), ObjectError);
  EXPECT_EQ(head(), "refs/heads/main");
  EXPECT_EQ(read("shared"), "base\n");
  EXPECT_TRUE(fs::exists(at("main-only")));
  EXPECT_EQ(staged_paths(), (Paths{"main-only", "shared"}));
}

TEST_F(CheckoutTest, LeavingAnUnbornBranch) {
  // HEAD names a branch with no commits while another branch exists.
  write("a", "1\n");
  add(".");
  commit("one");
  repo.set_head("refs/heads/empty");
  write("extra", "untracked\n");
  // "a" is in the index but not in the (empty) current commit, so it counts
  // as staged work that switching to main would replace.
  refused("main", "a");

  auto index = minigit::StagingIndex::load(repo.index_path());
  index.remove("a");
  index.save();
  fs::remove(at("a"));
  cli({"checkout", "main"});
  EXPECT_EQ(read("a"), "1\n");
  EXPECT_EQ(read("extra"), "untracked\n");
}

TEST_F(CheckoutTest, StateSurvivesARestart) {
  two_branches();
  // Drive the real executable so nothing is shared in memory between steps.
  const std::string exe = minigit::test::quote(MINIGIT_EXE);
  ASSERT_EQ(minigit::test::run_command(exe + " checkout topic 2>&1").status, 0);
  const auto st = minigit::test::run_command(exe + " status --short");
  EXPECT_EQ(st.status, 0);
  EXPECT_EQ(st.output, "");
  const auto br = minigit::test::run_command(exe + " branch");
  EXPECT_EQ(br.output, "  main\n* topic\n");
  EXPECT_EQ(read("shared"), "topic\n");
}

TEST_F(CheckoutTest, GitAgreesAfterEverySwitch) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  write("a/b/c.txt", "c\n");
  write("top", "top\n");
  add(".");
  commit("base");
  branch("other");
  write("a/b/d.txt", "d\n");
  fs::remove(at("top"));
  add(".");
  commit("main changes");
  cli({"checkout", "other"});
  write("z/new", "z\n");
  write("a/b/c.txt", "c2\n");
  add(".");
  commit("other changes");

  for (const std::string name : {"main", "other", "main", "other"}) {
    cli({"checkout", name});
    EXPECT_EQ(git_in(tmp.path(), "symbolic-ref --short HEAD").output, name);
    ASSERT_EQ(git_in(tmp.path(), "read-tree HEAD").status, 0);
    const auto st = git_in(tmp.path(), "-c core.filemode=true status --porcelain");
    EXPECT_EQ(st.output, "") << "after checkout " << name;
    // Our index holds exactly the files of the branch's commit.
    const auto files = git_in(tmp.path(), "ls-tree -r --name-only HEAD");
    Paths expected;
    std::istringstream lines(files.output);
    for (std::string line; std::getline(lines, line);) expected.push_back(line);
    EXPECT_EQ(staged_paths(), expected) << name;
  }
}

}  // namespace
