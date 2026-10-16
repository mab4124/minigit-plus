#include "minigit/cli.hpp"

#include "minigit/repository.hpp"

#include <exception>
#include <functional>
#include <ostream>
#include <string_view>

namespace minigit {
namespace {

constexpr std::string_view kVersion = "0.1.0";

struct CommandContext {
  const std::vector<std::string>& args;  // arguments after the command name
  std::ostream& out;
  std::ostream& err;
};

struct Command {
  std::string_view name;
  std::string_view summary;
  std::function<int(CommandContext&)> run;
};

const std::vector<Command>& commands();

void print_usage(std::ostream& os) {
  os << "usage: minigit <command> [<args>]\n\nCommands:\n";
  for (const auto& cmd : commands()) {
    os << "  " << cmd.name;
    for (auto i = cmd.name.size(); i < 10; ++i) os << ' ';
    os << ' ' << cmd.summary << '\n';
  }
}

int reject_extra_args(CommandContext& ctx, std::string_view name) {
  ctx.err << "minigit " << name << ": unexpected argument '" << ctx.args.front() << "'\n";
  return kExitUsage;
}

int cmd_help(CommandContext& ctx) {
  if (!ctx.args.empty()) return reject_extra_args(ctx, "help");
  print_usage(ctx.out);
  return kExitOk;
}

int cmd_version(CommandContext& ctx) {
  if (!ctx.args.empty()) return reject_extra_args(ctx, "version");
  ctx.out << "minigit version " << kVersion << '\n';
  return kExitOk;
}

int cmd_init(CommandContext& ctx) {
  if (ctx.args.size() > 1) {
    ctx.err << "usage: minigit init [<directory>]\n";
    return kExitUsage;
  }
  const fs::path target = ctx.args.empty() ? fs::current_path() : fs::path(ctx.args.front());
  bool reinitialized = false;
  const Repository repo = Repository::init(target, &reinitialized);
  ctx.out << (reinitialized ? "Reinitialized existing" : "Initialized empty")
          << " MiniGit++ repository in " << repo.git_dir().generic_string() << "/\n";
  return kExitOk;
}

const std::vector<Command>& commands() {
  static const std::vector<Command> table = {
      {"help", "Show this help message", cmd_help},
      {"init", "Create an empty repository", cmd_init},
      {"version", "Show the MiniGit++ version", cmd_version},
  };
  return table;
}

std::string_view canonical_name(std::string_view name) {
  if (name == "-h" || name == "--help") return "help";
  if (name == "--version") return "version";
  return name;
}

}  // namespace

int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    print_usage(err);
    return kExitUsage;
  }

  const std::string_view name = canonical_name(args.front());
  for (const auto& cmd : commands()) {
    if (cmd.name != name) continue;
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    CommandContext ctx{rest, out, err};
    try {
      return cmd.run(ctx);
    } catch (const std::exception& e) {
      err << "fatal: " << e.what() << '\n';
      return kExitError;
    }
  }

  err << "minigit: '" << args.front() << "' is not a minigit command. See 'minigit help'.\n";
  return kExitUsage;
}

}  // namespace minigit
