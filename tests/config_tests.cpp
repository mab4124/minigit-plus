#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"
#include "minigit/config.hpp"
#include "minigit/repository.hpp"
#include "test_support.hpp"

namespace {

using minigit::Config;
using minigit::ConfigError;
using minigit::Repository;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::read_text;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());

  Config load() const { return Config::load(repo.config_path()); }
};

TEST_F(ConfigTest, ReadsInitDefaults) {
  const Config config = load();
  EXPECT_EQ(config.get("core.bare"), "false");
  EXPECT_EQ(config.get("core.repositoryformatversion"), "0");
  EXPECT_EQ(config.get("user.name"), std::nullopt);
}

TEST(ConfigFile, MissingFileIsEmpty) {
  TempDir tmp;
  const Config config = Config::load(tmp.path() / "nope");
  EXPECT_EQ(config.get("user.name"), std::nullopt);
}

TEST_F(ConfigTest, SetAppendsSectionAndPreservesExistingLines) {
  Config config = load();
  config.set("user.name", "Ada Lovelace");
  config.set("user.email", "ada@example.com");
  config.save();
  EXPECT_EQ(read_text(repo.config_path()),
            "[core]\n"
            "\trepositoryformatversion = 0\n"
            "\tfilemode = false\n"
            "\tbare = false\n"
            "[user]\n"
            "\tname = Ada Lovelace\n"
            "\temail = ada@example.com\n");
  EXPECT_EQ(load().get("user.name"), "Ada Lovelace");
  EXPECT_EQ(load().get("user.email"), "ada@example.com");
}

TEST_F(ConfigTest, SetReplacesInPlaceAndInsertsIntoExistingSection) {
  write_text(repo.config_path(),
             "# top comment\n"
             "[user]\n"
             "\tname = Old  ; trailing comment\n"
             "\n"
             "[core]\n"
             "\tbare = false\n");
  Config config = load();
  EXPECT_EQ(config.get("user.name"), "Old");
  config.set("user.name", "New");
  config.set("user.email", "n@x");
  config.save();
  EXPECT_EQ(read_text(repo.config_path()),
            "# top comment\n"
            "[user]\n"
            "\tname = New\n"
            "\temail = n@x\n"
            "\n"
            "[core]\n"
            "\tbare = false\n");
}

TEST_F(ConfigTest, ParsesGitSyntax) {
  write_text(repo.config_path(),
             "[User]\n"
             "  NAME=first\n"
             "\tname = \"quoted # not a comment\"  # comment\n"
             "[user \"sub\"]\n"
             "\temail = ignored@x\n"
             "[core]\n"
             "\tflag\n"
             "\tesc = a\\\\b\\\"c\\td\n"
             "\tspaced = \"  keep  \"\n"
             "\tinner = a b  c   \r\n");
  const Config config = load();
  EXPECT_EQ(config.get("user.name"), "quoted # not a comment");  // last one wins
  EXPECT_EQ(config.get("USER.Name"), "quoted # not a comment");
  EXPECT_EQ(config.get("user.email"), std::nullopt);  // only in a subsection
  EXPECT_EQ(config.get("core.flag"), "true");
  EXPECT_EQ(config.get("core.esc"), "a\\b\"c\td");
  EXPECT_EQ(config.get("core.spaced"), "  keep  ");
  EXPECT_EQ(config.get("core.inner"), "a b  c");
}

TEST_F(ConfigTest, SpecialValuesRoundTrip) {
  const std::vector<std::string> values = {"", " lead", "trail ", "a#b", "a;b", "q\"uote",
                                           "back\\slash", "tab\there", "new\nline", "Zoë 李"};
  for (const auto& value : values) {
    Config config = load();
    config.set("user.name", value);
    config.save();
    EXPECT_EQ(load().get("user.name"), value) << value;
  }
}

TEST_F(ConfigTest, RejectsInvalidKeysAndValues) {
  Config config = load();
  for (const char* key : {"name", "user.", ".name", "user.sub.name", "us er.name", "user.1name",
                          "user.na_me"}) {
    EXPECT_THROW(config.set(key, "x"), ConfigError) << key;
    EXPECT_THROW(config.get(key), ConfigError) << key;
  }
  EXPECT_THROW(config.set("user.name", std::string("a\0b", 3)), ConfigError);
}

TEST_F(ConfigTest, MalformedFilesAreRejected) {
  for (const char* text : {"[user\n", "[user]\n\tname = \"open\n", "[user]\n\tname = a\\\n",
                           "[user]\n\t= x\n", "[user]\n\tname x\n", "[user]\n\tname = \\q\n"}) {
    write_text(repo.config_path(), text);
    EXPECT_THROW(load(), ConfigError) << text;
  }
}

// Real Git must read what we write, and we must read what Git writes.
TEST_F(ConfigTest, InteroperatesWithGitConfig) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  const std::string tricky = " Ada \"The Countess\" #1; back\\slash\ttab ";
  Config config = load();
  config.set("user.name", tricky);
  config.set("user.email", "ada@example.com");
  config.save();
  // `git config --get` decodes the value; -z avoids any output escaping.
  const auto name = minigit::test::run_command("git -C " + minigit::test::quote(tmp.path()) +
                                               " config -z --get user.name");
  ASSERT_EQ(name.status, 0);
  EXPECT_EQ(name.output, tricky + std::string(1, '\0'));
  EXPECT_EQ(git_in(tmp.path(), "config --get user.email").output, "ada@example.com");

  ASSERT_EQ(git_in(tmp.path(), "config user.email \"git#wrote;this@example.com\"").status, 0);
  ASSERT_EQ(git_in(tmp.path(), "config alias.st status").status, 0);
  EXPECT_EQ(load().get("user.email"), "git#wrote;this@example.com");
  EXPECT_EQ(load().get("user.name"), tricky);
  EXPECT_EQ(load().get("alias.st"), "status");
}

// --- minigit config --------------------------------------------------------------

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

class ConfigCommandTest : public ConfigTest {
 protected:
  ScopedCwd cwd{tmp.path()};
};

TEST_F(ConfigCommandTest, SetThenGet) {
  EXPECT_EQ(run({"config", "user.name"}).code, minigit::kExitError);  // unset
  EXPECT_EQ(run({"config", "user.name"}).out, "");
  EXPECT_EQ(run({"config", "user.name", "Ada Lovelace"}).code, minigit::kExitOk);
  EXPECT_EQ(run({"config", "user.email", "ada@example.com"}).code, minigit::kExitOk);
  const auto r = run({"config", "user.name"});
  EXPECT_EQ(r.code, minigit::kExitOk);
  EXPECT_EQ(r.out, "Ada Lovelace\n");
  EXPECT_EQ(run({"config", "user.email"}).out, "ada@example.com\n");
  EXPECT_EQ(load().get("user.name"), "Ada Lovelace");
}

TEST_F(ConfigCommandTest, WorksFromSubdirectory) {
  fs::create_directories(tmp.path() / "sub" / "dir");
  ScopedCwd inner(tmp.path() / "sub" / "dir");
  EXPECT_EQ(run({"config", "user.name", "Sub"}).code, minigit::kExitOk);
  EXPECT_EQ(load().get("user.name"), "Sub");
}

TEST_F(ConfigCommandTest, UsageErrors) {
  EXPECT_EQ(run({"config"}).code, minigit::kExitUsage);
  EXPECT_EQ(run({"config", "user.name", "a", "b"}).code, minigit::kExitUsage);
  const auto r = run({"config", "user.mail", "x"});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("unsupported key 'user.mail'"), std::string::npos);
  EXPECT_EQ(load().get("user.mail"), std::nullopt);
}

TEST_F(ConfigCommandTest, MalformedConfigIsReported) {
  write_text(repo.config_path(), "[user\n");
  const auto r = run({"config", "user.name"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("fatal: config line 1"), std::string::npos);
}

TEST(ConfigCommand, OutsideRepositoryFails) {
  TempDir tmp;
  if (fs::exists(tmp.path().parent_path() / ".git")) GTEST_SKIP() << "temp dir inside a repo";
  ScopedCwd cwd(tmp.path());
  const auto r = run({"config", "user.name", "x"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("not a minigit repository"), std::string::npos);
}

}  // namespace
