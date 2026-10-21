#include <gtest/gtest.h>

#include <sstream>

#include "minigit/checkout.hpp"
#include "minigit/cli.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/status.hpp"
#include "test_support.hpp"

namespace {

using minigit::CheckoutError;
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
  EXPECT_EQ(s.staged, (std::vector<minigit::StatusEntry>{{"new-staged", minigit::ChangeKind::Added}}));
  EXPECT_EQ(s.unstaged, (std::vector<minigit::StatusEntry>{{"same", minigit::ChangeKind::Modified}}));
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

}  // namespace
