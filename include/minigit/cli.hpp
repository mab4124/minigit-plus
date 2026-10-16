#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace minigit {

// Process exit codes used by every command.
inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;  // the command ran but failed
inline constexpr int kExitUsage = 2;  // the command line itself was invalid

// Runs MiniGit++ with `args` (the command line without the program name).
// Normal output goes to `out`, diagnostics to `err`. Taking streams instead of
// using std::cout directly keeps the CLI easy to test.
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace minigit
