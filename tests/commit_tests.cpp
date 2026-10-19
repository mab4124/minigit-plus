#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/config.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "test_support.hpp"

namespace {

using minigit::Commit;
using minigit::CommitError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::RepositoryError;
using minigit::Signature;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::read_text;
using minigit::test::ScopedCwd;
using minigit::test::ScopedEnv;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

constexpr const char* kEmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

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

// --- helpers -------------------------------------------------------------------

TEST(CommitMessage, IsCleanedLikeGit) {
  using minigit::clean_commit_message;
  EXPECT_EQ(clean_commit_message("msg"), "msg\n");
  EXPECT_EQ(clean_commit_message("msg\n"), "msg\n");
  EXPECT_EQ(clean_commit_message("\n\n  \nsubject  \n\n\nbody\t\n\n\n"), "subject\n\n\nbody\n");
  EXPECT_EQ(clean_commit_message("a\r\nb\r\n"), "a\nb\n");
  EXPECT_EQ(clean_commit_message("# kept\n"), "# kept\n");
  EXPECT_THROW(clean_commit_message(""), CommitError);
  EXPECT_THROW(clean_commit_message(" \n\t\n"), CommitError);
}

TEST(Timezone, FormatsOffsets) {
  using minigit::format_utc_offset;
  EXPECT_EQ(format_utc_offset(0), "+0000");
  EXPECT_EQ(format_utc_offset(19800), "+0530");   // India
  EXPECT_EQ(format_utc_offset(-28800), "-0800");  // US Pacific
  EXPECT_EQ(format_utc_offset(-12600), "-0330");  // Newfoundland
  EXPECT_EQ(format_utc_offset(50400), "+1400");
}

#ifndef _WIN32
TEST(Timezone, UsesLocalTimezone) {
  {
    ScopedEnv tz("TZ", "UTC0");
    tzset();
    EXPECT_EQ(minigit::local_utc_offset(1700000000), 0);
  }
  {
    // POSIX TZ strings count west as positive: this is UTC+05:30.
    ScopedEnv tz("TZ", "IST-5:30");
    tzset();
    EXPECT_EQ(minigit::local_utc_offset(1700000000), 19800);
    const Signature sig = minigit::make_signature("A", "a@x", 1700000000);
    EXPECT_EQ(sig.timestamp, 1700000000);
    EXPECT_EQ(sig.timezone, "+0530");
  }
  tzset();
}
#endif

// --- create_commit ---------------------------------------------------------------

class CreateCommitTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0530"};

  void stage(const std::string& path, const std::string& text) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    index.stage(path, {"100644", store.write(ObjectType::Blob, text)});
    index.save();
  }

  Commit read_commit(const std::string& oid) const {
    const auto obj = store.read(oid);
    EXPECT_EQ(obj.type, ObjectType::Commit);
    return minigit::parse_commit(obj.content);
  }
};

TEST_F(CreateCommitTest, RootCommitAdvancesBranch) {
  EXPECT_EQ(repo.read_ref("refs/heads/main"), std::nullopt);
  stage("hello.txt", "hello\n");
  const auto result = minigit::create_commit(repo, "first", author);
  EXPECT_TRUE(result.root);
  EXPECT_EQ(result.branch, "refs/heads/main");
  EXPECT_EQ(repo.read_ref("refs/heads/main"), result.oid);
  EXPECT_EQ(read_text(tmp.path() / ".git" / "refs" / "heads" / "main"), result.oid + "\n");

  const Commit c = read_commit(result.oid);
  EXPECT_TRUE(c.parents.empty());
  EXPECT_EQ(c.author, author);
  EXPECT_EQ(c.committer, author);
  EXPECT_EQ(c.message, "first\n");
  EXPECT_TRUE(store.contains(c.tree));
}

TEST_F(CreateCommitTest, SecondCommitRecordsParent) {
  stage("a", "1\n");
  const auto first = minigit::create_commit(repo, "one", author);
  stage("a", "2\n");
  const auto second = minigit::create_commit(repo, "two", author);
  EXPECT_FALSE(second.root);
  EXPECT_EQ(read_commit(second.oid).parents, std::vector<std::string>{first.oid});
  EXPECT_EQ(repo.read_ref("refs/heads/main"), second.oid);
}

TEST_F(CreateCommitTest, CommitsOnTheBranchHeadNames) {
  write_text(tmp.path() / ".git" / "HEAD", "ref: refs/heads/feature/x\n");
  stage("a", "1\n");
  const auto result = minigit::create_commit(repo, "on feature", author);
  EXPECT_EQ(result.branch, "refs/heads/feature/x");
  EXPECT_EQ(repo.read_ref("refs/heads/feature/x"), result.oid);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), std::nullopt);
}

TEST_F(CreateCommitTest, RejectsEmptyCommits) {
  EXPECT_THROW(minigit::create_commit(repo, "nothing staged", author), CommitError);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), std::nullopt);

  stage("a", "1\n");
  const auto first = minigit::create_commit(repo, "one", author);
  EXPECT_THROW(minigit::create_commit(repo, "same tree", author), CommitError);
  stage("a", "1\n");  // restaging identical content changes nothing
  EXPECT_THROW(minigit::create_commit(repo, "same tree", author), CommitError);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), first.oid);
}

TEST_F(CreateCommitTest, DeletingEveryFileIsACommit) {
  stage("a", "1\n");
  minigit::create_commit(repo, "one", author);
  auto index = minigit::StagingIndex::load(repo.index_path());
  index.remove("a");
  index.save();
  const auto result = minigit::create_commit(repo, "remove all", author);
  EXPECT_EQ(read_commit(result.oid).tree, kEmptyTree);
}

TEST_F(CreateCommitTest, EmptyMessageIsRejectedBeforeAnythingIsWritten) {
  stage("a", "1\n");
  EXPECT_THROW(minigit::create_commit(repo, "  \n", author), CommitError);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), std::nullopt);
}

TEST_F(CreateCommitTest, FailuresLeaveTheBranchUnchanged) {
  stage("a", "1\n");
  const auto first = minigit::create_commit(repo, "one", author);
  stage("a", "2\n");

  // A signature that cannot be serialized fails after the tree is written.
  Signature bad = author;
  bad.name = "Evil <name>";
  EXPECT_THROW(minigit::create_commit(repo, "bad", bad), minigit::ObjectError);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), first.oid);

  // A branch pointing at a missing object is reported, not overwritten.
  const std::string missing(40, 'a');
  repo.write_ref("refs/heads/main", missing);
  EXPECT_THROW(minigit::create_commit(repo, "two", author), minigit::ObjectError);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), missing);

  // So is a branch pointing at a tree instead of a commit.
  repo.write_ref("refs/heads/main", kEmptyTree);
  store.write(ObjectType::Tree, "");
  EXPECT_THROW(minigit::create_commit(repo, "two", author), CommitError);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), kEmptyTree);
}

TEST_F(CreateCommitTest, MalformedRefsAreErrors) {
  write_text(tmp.path() / ".git" / "refs" / "heads" / "main", "not an id\n");
  stage("a", "1\n");
  EXPECT_THROW(minigit::create_commit(repo, "x", author), RepositoryError);
  EXPECT_EQ(read_text(tmp.path() / ".git" / "refs" / "heads" / "main"), "not an id\n");

  for (const char* bad : {"main", "refs/heads/../x", "refs/heads/.hidden", "refs/heads/a.lock",
                          "refs/heads/a b", "refs//x", "refs/heads/"}) {
    EXPECT_THROW(repo.read_ref(bad), RepositoryError) << bad;
    EXPECT_THROW(repo.write_ref(bad, kEmptyTree), RepositoryError) << bad;
  }
  EXPECT_THROW(repo.write_ref("refs/heads/x", "abc"), RepositoryError);
}

// --- minigit commit --------------------------------------------------------------

class CommitCommandTest : public CreateCommitTest {
 protected:
  ScopedCwd cwd{tmp.path()};

  void SetUp() override {
    ASSERT_EQ(run({"config", "user.name", "Ada Lovelace"}).code, minigit::kExitOk);
    ASSERT_EQ(run({"config", "user.email", "ada@example.com"}).code, minigit::kExitOk);
  }
};

TEST_F(CommitCommandTest, CommitsStagedFilesAndReportsThem) {
  write_text(tmp.path() / "hello.txt", "hello\n");
  ASSERT_EQ(run({"add", "hello.txt"}).code, minigit::kExitOk);
  const auto r = run({"commit", "-m", "Add hello\n\nWith a body."});
  ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  const auto oid = repo.read_ref("refs/heads/main");
  ASSERT_TRUE(oid);
  EXPECT_EQ(r.out, "[main (root-commit) " + oid->substr(0, 7) + "] Add hello\n");

  const Commit c = read_commit(*oid);
  EXPECT_EQ(c.author.name, "Ada Lovelace");
  EXPECT_EQ(c.author.email, "ada@example.com");
  EXPECT_EQ(c.message, "Add hello\n\nWith a body.\n");

  write_text(tmp.path() / "hello.txt", "hello again\n");
  ASSERT_EQ(run({"add", "hello.txt"}).code, minigit::kExitOk);
  const auto r2 = run({"commit", "-m", "Update"});
  ASSERT_EQ(r2.code, minigit::kExitOk) << r2.err;
  EXPECT_EQ(r2.out, "[main " + repo.read_ref("refs/heads/main")->substr(0, 7) + "] Update\n");
}

TEST_F(CommitCommandTest, MultipleMessagesBecomeParagraphs) {
  write_text(tmp.path() / "f", "x\n");
  ASSERT_EQ(run({"add", "f"}).code, minigit::kExitOk);
  ASSERT_EQ(run({"commit", "-m", "subject", "-m", "body"}).code, minigit::kExitOk);
  EXPECT_EQ(read_commit(*repo.read_ref("refs/heads/main")).message, "subject\n\nbody\n");
}

TEST_F(CommitCommandTest, NothingToCommit) {
  auto r = run({"commit", "-m", "empty"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("nothing to commit"), std::string::npos);

  write_text(tmp.path() / "f", "x\n");
  ASSERT_EQ(run({"add", "f"}).code, minigit::kExitOk);
  ASSERT_EQ(run({"commit", "-m", "one"}).code, minigit::kExitOk);
  r = run({"commit", "-m", "again"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("nothing to commit"), std::string::npos);
}

TEST_F(CommitCommandTest, RequiresIdentity) {
  write_text(repo.config_path(), "[core]\n\tbare = false\n");
  write_text(tmp.path() / "f", "x\n");
  ASSERT_EQ(run({"add", "f"}).code, minigit::kExitOk);
  const auto r = run({"commit", "-m", "who am I"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("author identity unknown"), std::string::npos);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), std::nullopt);
}

TEST_F(CommitCommandTest, UsageErrors) {
  EXPECT_EQ(run({"commit"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"commit", "-m"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"commit", "message"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"commit", "-a", "-m", "x"}).code, minigit::kExitUsage);
  const auto r = run({"commit", "-m", "   "});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("empty commit message"), std::string::npos);
}

// --- history checked against real Git ---------------------------------------------

class CommitHistoryTest : public CommitCommandTest {
 protected:
  // Writes `text` to `path` and stages it with `minigit add`.
  void edit(const std::string& path, const std::string& text) {
    write_text(tmp.path() / path, text);
    const auto r = run({"add", path});
    ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  }

  std::string commit(const std::string& message) {
    const auto r = run({"commit", "-m", message});
    EXPECT_EQ(r.code, minigit::kExitOk) << r.err;
    return repo.read_ref("refs/heads/main").value_or("");
  }
};

TEST_F(CommitHistoryTest, InitialAndSubsequentCommitsFormAParentChain) {
  edit("README.md", "# demo\n");
  edit("src/main.cpp", "int main() {}\n");
  const std::string c1 = commit("Initial commit");
  edit("src/main.cpp", "int main() { return 0; }\n");
  edit("src/util/helpers.hpp", "#pragma once\n");
  const std::string c2 = commit("Add helpers");
  fs::remove(tmp.path() / "README.md");
  ASSERT_EQ(run({"add", "README.md"}).code, minigit::kExitOk);
  const std::string c3 = commit("Remove README");

  // Walk first parents from the branch tip with our own parser.
  std::vector<std::string> chain;
  std::vector<std::string> messages;
  for (std::optional<std::string> oid = repo.read_ref("refs/heads/main"); oid;) {
    chain.push_back(*oid);
    const Commit c = read_commit(*oid);
    messages.push_back(c.message);
    oid = c.parents.empty() ? std::nullopt : std::optional<std::string>(c.parents.front());
  }
  EXPECT_EQ(chain, (std::vector<std::string>{c3, c2, c1}));
  EXPECT_EQ(messages, (std::vector<std::string>{"Remove README\n", "Add helpers\n",
                                                "Initial commit\n"}));
  EXPECT_EQ(read_commit(c1).parents.size(), 0u);
  EXPECT_EQ(read_commit(c2).parents, std::vector<std::string>{c1});
  EXPECT_EQ(read_commit(c3).parents, std::vector<std::string>{c2});
}

TEST_F(CommitHistoryTest, GitReadsOurCommits) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  edit("a.txt", "one\n");
  edit("dir/b.txt", "bee\n");
  const std::string c1 = commit("First");
  edit("a.txt", "two\n");
  const std::string c2 = commit("Second\n\nWith a body line.");
  fs::remove_all(tmp.path() / "dir");
  ASSERT_EQ(run({"add", "dir"}).code, minigit::kExitOk);
  const std::string c3 = commit("Third");

  // git cat-file shows exactly the bytes we stored.
  for (const auto& oid : {c1, c2, c3}) {
    const auto shown = minigit::test::run_command("git -C " + minigit::test::quote(tmp.path()) +
                                                  " cat-file -p " + oid);
    ASSERT_EQ(shown.status, 0) << oid;
    EXPECT_EQ(shown.output, store.read(oid).content);
    EXPECT_EQ(git_in(tmp.path(), "cat-file -t " + oid).output, "commit");
  }

  // git log follows our parent chain from HEAD (resolved through our ref).
  EXPECT_EQ(git_in(tmp.path(), "rev-parse HEAD").output, c3);
  EXPECT_EQ(git_in(tmp.path(), "log --format=%H").output, c3 + "\n" + c2 + "\n" + c1);
  EXPECT_EQ(git_in(tmp.path(), "log \"--format=%an|%ae|%cn|%s\"").output,
            "Ada Lovelace|ada@example.com|Ada Lovelace|Third\n"
            "Ada Lovelace|ada@example.com|Ada Lovelace|Second\n"
            "Ada Lovelace|ada@example.com|Ada Lovelace|First");
  EXPECT_EQ(git_in(tmp.path(), "log -1 --skip=1 --format=%b").output, "With a body line.");
  EXPECT_EQ(git_in(tmp.path(), "rev-list --count HEAD").output, "3");

  // The recorded snapshots are what Git sees in each commit.
  EXPECT_EQ(git_in(tmp.path(), "ls-tree -r --name-only " + c1).output, "a.txt\ndir/b.txt");
  EXPECT_EQ(git_in(tmp.path(), "show " + c2 + ":a.txt").output, "two");
  EXPECT_EQ(git_in(tmp.path(), "ls-tree -r --name-only " + c3).output, "a.txt");
  EXPECT_EQ(git_in(tmp.path(), "diff --name-status " + c1 + " " + c2).output, "M\ta.txt");
  EXPECT_EQ(git_in(tmp.path(), "diff --name-status " + c2 + " " + c3).output, "D\tdir/b.txt");

  // And the repository passes Git's own consistency check.
  EXPECT_EQ(git_in(tmp.path(), "fsck --strict --no-dangling").status, 0);
}

// The same tree, parent, identity and time give the same commit id in Git.
TEST_F(CommitHistoryTest, CommitIdsMatchGitCommitTree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  ScopedEnv an("GIT_AUTHOR_NAME", author.name), ae("GIT_AUTHOR_EMAIL", author.email),
      ad("GIT_AUTHOR_DATE", "1700000000 +0530"), cn("GIT_COMMITTER_NAME", author.name),
      ce("GIT_COMMITTER_EMAIL", author.email), cd("GIT_COMMITTER_DATE", "1700000000 +0530");

  stage("a", "1\n");
  const auto first = minigit::create_commit(repo, "first", author);
  const std::string tree1 = read_commit(first.oid).tree;
  EXPECT_EQ(git_in(tmp.path(), "commit-tree " + tree1 + " -m first").output, first.oid);

  stage("b/c", "2\n");
  const auto second = minigit::create_commit(repo, "second", author);
  const std::string tree2 = read_commit(second.oid).tree;
  EXPECT_EQ(git_in(tmp.path(), "commit-tree " + tree2 + " -p " + first.oid + " -m second").output,
            second.oid);
}

TEST_F(CommitHistoryTest, HistorySurvivesSeparateProcesses) {
  const auto exe = [&](const std::string& args) {
    return minigit::test::run_command("cd " + minigit::test::quote(tmp.path()) + " && " +
                                      minigit::test::quote(MINIGIT_EXE) + " " + args);
  };
  write_text(tmp.path() / "f.txt", "v1\n");
  ASSERT_EQ(exe("add f.txt").status, 0);
  ASSERT_EQ(exe("commit -m \"first\"").status, 0);
  const std::string c1 = repo.read_ref("refs/heads/main").value_or("");
  write_text(tmp.path() / "f.txt", "v2\n");
  ASSERT_EQ(exe("add f.txt").status, 0);
  ASSERT_EQ(exe("commit -m \"second\"").status, 0);
  const std::string c2 = repo.read_ref("refs/heads/main").value_or("");
  ASSERT_NE(c1, c2);
  EXPECT_EQ(read_commit(c2).parents, std::vector<std::string>{c1});
  EXPECT_NE(exe("commit -m \"nothing new\"").status, 0);
  EXPECT_EQ(repo.read_ref("refs/heads/main"), c2);
}

TEST(CommitCommand, OutsideRepositoryFails) {
  TempDir tmp;
  if (fs::exists(tmp.path().parent_path() / ".git")) GTEST_SKIP() << "temp dir inside a repo";
  ScopedCwd cwd(tmp.path());
  const auto r = run({"commit", "-m", "x"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a minigit repository"), std::string::npos);
}

}  // namespace
