#include <gtest/gtest.h>

#include <sstream>

#include "minigit/cli.hpp"

namespace {

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

TEST(Cli, NoArgumentsPrintsUsageAndFails) {
  const auto r = run({});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("usage: minigit"), std::string::npos);
  EXPECT_TRUE(r.out.empty());
}

TEST(Cli, HelpListsCommands) {
  for (const std::string flag : {"help", "-h", "--help"}) {
    const auto r = run({flag});
    EXPECT_EQ(r.code, minigit::kExitOk) << flag;
    EXPECT_NE(r.out.find("version"), std::string::npos) << flag;
  }
}

TEST(Cli, VersionPrintsVersion) {
  const auto r = run({"--version"});
  EXPECT_EQ(r.code, minigit::kExitOk);
  EXPECT_EQ(r.out, "minigit version 0.1.0\n");
}

TEST(Cli, UnknownCommandIsUsageError) {
  const auto r = run({"frobnicate"});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("'frobnicate' is not a minigit command"), std::string::npos);
}

TEST(Cli, ExtraArgumentsAreRejected) {
  const auto r = run({"version", "--bogus"});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("unexpected argument '--bogus'"), std::string::npos);
}

}  // namespace
