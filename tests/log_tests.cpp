#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/log.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "test_support.hpp"

namespace {

using minigit::Commit;
using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::Signature;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::ScopedCwd;
using minigit::test::ScopedEnv;
using minigit::test::TempDir;
using minigit::test::write_text;

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

TEST(GitDate, MatchesGitDefaultFormat) {
  using minigit::format_git_date;
  // Expected strings come from `git log` on commits with these dates.
  EXPECT_EQ(format_git_date(1700000000, "+0530"), "Wed Nov 15 03:43:20 2023 +0530");
  EXPECT_EQ(format_git_date(1699000000, "-0800"), "Fri Nov 3 00:26:40 2023 -0800");
  EXPECT_EQ(format_git_date(0, "+0000"), "Thu Jan 1 00:00:00 1970 +0000");
  EXPECT_EQ(format_git_date(0, "-0100"), "Wed Dec 31 23:00:00 1969 -0100");
  EXPECT_EQ(format_git_date(951782400, "+0000"), "Tue Feb 29 00:00:00 2000 +0000");  // leap day
  EXPECT_EQ(format_git_date(-86400, "+0000"), "Wed Dec 31 00:00:00 1969 +0000");
}

class LogTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0530"};

  std::string commit_file(const std::string& path, const std::string& text,
                          const std::string& message) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    index.stage(path, {"100644", store.write(ObjectType::Blob, text)});
    index.save();
    author.timestamp += 60;
    return minigit::create_commit(repo, message, author).oid;
  }

  // Writes a commit object directly, for shapes `minigit commit` cannot make.
  std::string write_commit(std::vector<std::string> parents, const std::string& message) {
    Commit c;
    c.tree = store.write(ObjectType::Tree, "");
    c.parents = std::move(parents);
    c.author = c.committer = author;
    c.message = message;
    return store.write(ObjectType::Commit, minigit::serialize_commit(c));
  }
};

TEST_F(LogTest, FirstParentLogWalksNewestFirst) {
  const std::string c1 = commit_file("a", "1\n", "one");
  const std::string c2 = commit_file("a", "2\n", "two");
  const std::string c3 = commit_file("a", "3\n", "three");

  const auto all = minigit::first_parent_log(store, c3);
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0].oid, c3);
  EXPECT_EQ(all[1].oid, c2);
  EXPECT_EQ(all[2].oid, c1);
  EXPECT_EQ(all[1].commit.message, "two\n");

  EXPECT_EQ(minigit::first_parent_log(store, c3, 2).size(), 2u);
  EXPECT_EQ(minigit::first_parent_log(store, c3, 0).size(), 0u);
  EXPECT_EQ(minigit::first_parent_log(store, c3, 10).size(), 3u);
  EXPECT_EQ(minigit::first_parent_log(store, c1).size(), 1u);
}

TEST_F(LogTest, MergesFollowOnlyTheFirstParent) {
  const std::string base = write_commit({}, "base\n");
  const std::string side = write_commit({base}, "side\n");
  const std::string mainline = write_commit({base}, "main\n");
  const std::string merge = write_commit({mainline, side}, "merge\n");

  const auto log = minigit::first_parent_log(store, merge);
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0].oid, merge);
  EXPECT_EQ(log[1].oid, mainline);
  EXPECT_EQ(log[2].oid, base);

  std::ostringstream out;
  minigit::print_log(out, {log[0]});
  EXPECT_NE(out.str().find("\nMerge: " + mainline.substr(0, 7) + " " + side.substr(0, 7) + "\n"),
            std::string::npos);
}

TEST_F(LogTest, BrokenHistoryIsAnError) {
  const std::string missing(40, 'b');
  const std::string orphan = write_commit({missing}, "orphan\n");
  EXPECT_THROW(minigit::first_parent_log(store, orphan), ObjectError);
  // The limit stops the walk before it reaches the missing parent.
  EXPECT_EQ(minigit::first_parent_log(store, orphan, 1).size(), 1u);

  const std::string blob = store.write(ObjectType::Blob, "not a commit");
  EXPECT_THROW(minigit::first_parent_log(store, blob), ObjectError);
}

TEST_F(LogTest, PrintsGitMediumFormat) {
  author.timestamp = 1700000000 - 60;
  const std::string c1 = commit_file("a", "1\n", "subj\n\nbody line\n\n  indented");
  const std::string c2 = commit_file("a", "2\n", "two");
  ScopedCwd cwd(tmp.path());
  const auto r = run({"log"});
  ASSERT_EQ(r.code, minigit::kExitOk) << r.err;
  EXPECT_EQ(r.out,
            "commit " + c2 + "\n"
            "Author: Ada Lovelace <ada@example.com>\n"
            "Date:   Wed Nov 15 03:44:20 2023 +0530\n"
            "\n"
            "    two\n"
            "\n"
            "commit " + c1 + "\n"
            "Author: Ada Lovelace <ada@example.com>\n"
            "Date:   Wed Nov 15 03:43:20 2023 +0530\n"
            "\n"
            "    subj\n"
            "    \n"
            "    body line\n"
            "    \n"
            "      indented\n");
}

TEST_F(LogTest, LimitOptions) {
  commit_file("a", "1\n", "one");
  commit_file("a", "2\n", "two");
  const std::string c3 = commit_file("a", "3\n", "three");
  ScopedCwd cwd(tmp.path());
  const auto count = [](const std::string& out) {
    std::size_t n = 0;
    for (std::size_t pos = 0; (pos = out.find("commit ", pos)) != std::string::npos; ++pos) {
      if (pos == 0 || out[pos - 1] == '\n') ++n;
    }
    return n;
  };
  EXPECT_EQ(count(run({"log"}).out), 3u);
  EXPECT_EQ(count(run({"log", "-n", "2"}).out), 2u);
  EXPECT_EQ(count(run({"log", "-n1"}).out), 1u);
  EXPECT_EQ(count(run({"log", "--max-count=2"}).out), 2u);
  EXPECT_EQ(count(run({"log", "-n", "99"}).out), 3u);
  const auto none = run({"log", "-n", "0"});
  EXPECT_EQ(none.code, minigit::kExitOk);
  EXPECT_EQ(none.out, "");
  EXPECT_EQ(run({"log", "-n", "1"}).out.rfind("commit " + c3 + "\n", 0), 0u);
  // The last option wins, as in Git.
  EXPECT_EQ(count(run({"log", "-n", "1", "-n", "3"}).out), 3u);
}

TEST_F(LogTest, UsageErrors) {
  commit_file("a", "1\n", "one");
  ScopedCwd cwd(tmp.path());
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{{"log", "-n"},
                                             {"log", "-n", "x"},
                                             {"log", "-n", "-1"},
                                             {"log", "-n", "2x"},
                                             {"log", "--max-count="},
                                             {"log", "--oneline"},
                                             {"log", "main"}}) {
    const auto r = run(args);
    EXPECT_EQ(r.code, minigit::kExitUsage) << args.back();
    EXPECT_EQ(r.out, "");
  }
}

TEST_F(LogTest, EmptyBranchIsReported) {
  ScopedCwd cwd(tmp.path());
  const auto r = run({"log"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("current branch 'main' does not have any commits yet"), std::string::npos);
}

// Our commits, printed by us and by Git, give byte-identical output.
TEST_F(LogTest, OutputMatchesGitLogOnOurCommits) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  commit_file("README.md", "# demo\n", "Initial commit");
  author.timezone = "-0330";
  commit_file("src/main.cpp", "int main() {}\n", "Add main\n\nExplain why.\n");
  author.timezone = "+0000";
  commit_file("src/main.cpp", "int main() { return 0; }\n", "Return zero");

  ScopedCwd cwd(tmp.path());
  for (const std::string& n : {std::string(""), std::string(" -n 2")}) {
    const auto ours = run(n.empty() ? std::vector<std::string>{"log"}
                                    : std::vector<std::string>{"log", "-n", "2"});
    const auto theirs = git_in(tmp.path(), "log" + n);
    ASSERT_EQ(theirs.status, 0);
    EXPECT_EQ(ours.out, theirs.output + "\n") << n;
  }
}

// History written entirely by real Git, including a merge, reads the same.
TEST(LogOracle, OutputMatchesGitLogOnGitHistory) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  TempDir tmp;
  ScopedEnv an("GIT_AUTHOR_NAME", "Grace Hopper"), ae("GIT_AUTHOR_EMAIL", "grace@example.com"),
      cn("GIT_COMMITTER_NAME", "Grace Hopper"), ce("GIT_COMMITTER_EMAIL", "grace@example.com"),
      cd("GIT_COMMITTER_DATE", "1700000000 +0100");
  const std::string git = "-c init.defaultBranch=main -c commit.gpgsign=false ";
  const auto step = [&](const std::string& date, const std::string& args) {
    ScopedEnv ad("GIT_AUTHOR_DATE", date);
    ASSERT_EQ(git_in(tmp.path(), git + args).status, 0) << args;
  };
  ASSERT_EQ(git_in(tmp.path(), git + "init -q").status, 0);
  write_text(tmp.path() / "a.txt", "1\n");
  step("1600000000 +0200", "add a.txt");
  step("1600000000 +0200", "commit -q -m base");
  step("1600000000 +0200", "checkout -q -b side");
  write_text(tmp.path() / "b.txt", "side\n");
  step("1600000000 +0200", "add b.txt");
  step("1600003600 -0700", "commit -q -m \"side work\"");
  step("1600003600 -0700", "checkout -q main");
  write_text(tmp.path() / "a.txt", "2\n");
  step("1600007200 +0545", "commit -q -a -m \"main work\"");
  step("1600010800 +0000", "merge -q --no-ff --no-edit side");

  ScopedCwd cwd(tmp.path());
  const auto ours = run({"log"});
  ASSERT_EQ(ours.code, minigit::kExitOk) << ours.err;
  const auto theirs = git_in(tmp.path(), "log --first-parent");
  ASSERT_EQ(theirs.status, 0);
  EXPECT_EQ(ours.out, theirs.output + "\n");
  EXPECT_NE(ours.out.find("Merge: "), std::string::npos);
}

}  // namespace
