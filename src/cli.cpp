#include "minigit/cli.hpp"

#include "minigit/add.hpp"
#include "minigit/branch.hpp"
#include "minigit/checkout.hpp"
#include "minigit/config.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/file_util.hpp"
#include "minigit/log.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/status.hpp"
#include "minigit/tree.hpp"

#include <charconv>
#include <ctime>
#include <exception>
#include <functional>
#include <optional>
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
    for (auto i = cmd.name.size(); i < 11; ++i) os << ' ';
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

int cmd_hash_object(CommandContext& ctx) {
  bool write = false;
  std::vector<std::string> files;
  for (const auto& arg : ctx.args) {
    if (arg == "-w") {
      write = true;
    } else if (!arg.empty() && arg.front() == '-') {
      ctx.err << "minigit hash-object: unknown option '" << arg << "'\n";
      return kExitUsage;
    } else {
      files.push_back(arg);
    }
  }
  if (files.size() != 1) {
    ctx.err << "usage: minigit hash-object [-w] <file>\n";
    return kExitUsage;
  }

  // Files are hashed byte for byte: unlike Git, no line-ending or filter
  // conversion is applied.
  const std::string content = read_file(files.front());
  if (write) {
    // Only writing needs a repository, as with `git hash-object`.
    const Repository repo = Repository::discover(fs::current_path());
    ctx.out << ObjectStore(repo.objects_dir()).write(ObjectType::Blob, content) << '\n';
  } else {
    ctx.out << hash_object(ObjectType::Blob, content) << '\n';
  }
  return kExitOk;
}

// The type Git prints for a tree entry in `cat-file -p` output.
std::string_view entry_type(const TreeEntry& entry) {
  if (entry.is_tree()) return "tree";
  if (entry.mode == kModeGitlink) return "commit";
  return "blob";
}

int cmd_cat_file(CommandContext& ctx) {
  if (ctx.args.size() != 2 ||
      (ctx.args[0] != "-t" && ctx.args[0] != "-s" && ctx.args[0] != "-p")) {
    ctx.err << "usage: minigit cat-file (-t | -s | -p) <object>\n";
    return kExitUsage;
  }
  const std::string& flag = ctx.args[0];
  const Repository repo = Repository::discover(fs::current_path());
  std::string oid;
  try {
    oid = resolve_revision(repo, ctx.args[1]);
  } catch (const RevisionError&) {
    // Abbreviated ids are not supported.
    throw ObjectError("not a valid object name '" + ctx.args[1] +
                      "' (expected a full 40-character id, HEAD or a branch/tag name)");
  }
  const RawObject obj = ObjectStore(repo.objects_dir()).read(oid);
  if (flag == "-t") {
    ctx.out << to_string(obj.type) << '\n';
  } else if (flag == "-s") {
    ctx.out << obj.content.size() << '\n';
  } else if (obj.type == ObjectType::Tree) {
    for (const auto& entry : parse_tree(obj.content)) {
      // Git pads modes to six digits here ("040000"), unlike inside the tree.
      for (auto i = entry.mode.size(); i < 6; ++i) ctx.out << '0';
      ctx.out << entry.mode << ' ' << entry_type(entry) << ' ' << entry.oid << '\t' << entry.name
              << '\n';
    }
  } else {
    ctx.out << obj.content;  // blobs and commits are printed verbatim
  }
  return kExitOk;
}

int cmd_add(CommandContext& ctx) {
  std::vector<std::string> pathspecs;
  bool options_done = false;
  for (const auto& arg : ctx.args) {
    if (!options_done && arg == "--") {
      options_done = true;
    } else if (!options_done && !arg.empty() && arg.front() == '-') {
      ctx.err << "minigit add: unknown option '" << arg << "'\n";
      return kExitUsage;
    } else {
      pathspecs.push_back(arg);
    }
  }
  if (pathspecs.empty()) {
    ctx.err << "usage: minigit add [--] <path>...\nNothing specified, nothing added.\n";
    return kExitUsage;
  }

  const fs::path cwd = fs::current_path();
  const Repository repo = Repository::discover(cwd);
  StagingIndex index = StagingIndex::load(repo.index_path());
  const AddResult result = add_paths(repo, index, pathspecs, cwd);
  for (const auto& warning : result.warnings) ctx.err << "warning: " << warning << '\n';
  index.save();  // only reached if every pathspec was staged successfully
  return kExitOk;
}

int cmd_config(CommandContext& ctx) {
  if (ctx.args.empty() || ctx.args.size() > 2) {
    ctx.err << "usage: minigit config <key> [<value>]\n";
    return kExitUsage;
  }
  const std::string& key = ctx.args[0];
  // Only the keys MiniGit++ itself reads are accepted, so a typo such as
  // "user.mail" is reported instead of being silently stored.
  if (key != "user.name" && key != "user.email") {
    ctx.err << "minigit config: unsupported key '" << key
            << "' (supported: user.name, user.email)\n";
    return kExitUsage;
  }

  const Repository repo = Repository::discover(fs::current_path());
  Config config = Config::load(repo.config_path());
  if (ctx.args.size() == 1) {
    const auto value = config.get(key);
    if (!value) return kExitError;  // unset: no output, like `git config`
    ctx.out << *value << '\n';
    return kExitOk;
  }
  config.set(key, ctx.args[1]);
  config.save();
  return kExitOk;
}

int cmd_checkout(CommandContext& ctx) {
  for (const auto& arg : ctx.args) {
    if (!arg.empty() && arg.front() == '-') {
      ctx.err << "minigit checkout: unknown option '" << arg << "'\n";
      return kExitUsage;
    }
  }
  if (ctx.args.size() != 1) {
    ctx.err << "usage: minigit checkout <branch>\n";
    return kExitUsage;
  }
  const Repository repo = Repository::discover(fs::current_path());
  const CheckoutResult result = checkout_branch(repo, ctx.args[0]);
  // Like Git, these messages go to stderr.
  ctx.err << (result.already_on ? "Already on '" : "Switched to branch '") << ctx.args[0]
          << "'\n";
  return kExitOk;
}

int cmd_commit(CommandContext& ctx) {
  // Several -m options become separate paragraphs, as in Git.
  std::string message;
  bool have_message = false;
  for (std::size_t i = 0; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    if (arg != "-m" || i + 1 == ctx.args.size()) {
      if (arg != "-m") ctx.err << "minigit commit: unexpected argument '" << arg << "'\n";
      ctx.err << "usage: minigit commit -m <message>\n";
      return kExitUsage;
    }
    if (have_message) message += "\n\n";
    message += ctx.args[++i];
    have_message = true;
  }
  if (!have_message) {
    ctx.err << "usage: minigit commit -m <message>\n";
    return kExitUsage;
  }

  const Repository repo = Repository::discover(fs::current_path());
  const Config config = Config::load(repo.config_path());
  const auto name = config.get("user.name");
  const auto email = config.get("user.email");
  if (!name || name->empty() || !email) {
    throw CommitError(
        "author identity unknown; run\n"
        "  minigit config user.name \"Your Name\"\n"
        "  minigit config user.email \"you@example.com\"");
  }

  const CommitResult result =
      create_commit(repo, message, make_signature(*name, *email, std::time(nullptr)));
  const std::string branch = short_branch_name(result.branch);
  const std::string cleaned = clean_commit_message(message);
  ctx.out << '[' << branch << (result.root ? " (root-commit) " : " ") << result.oid.substr(0, 7)
          << "] " << cleaned.substr(0, cleaned.find('\n')) << '\n';
  return kExitOk;
}

// Parses a non-negative decimal count such as the N in `log -n N`.
std::optional<std::size_t> parse_count(std::string_view text) {
  std::size_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return value;
}

int cmd_log(CommandContext& ctx) {
  std::optional<std::size_t> limit;
  for (std::size_t i = 0; i < ctx.args.size(); ++i) {
    const std::string& arg = ctx.args[i];
    std::string_view value;
    if (arg == "-n" && i + 1 < ctx.args.size()) {
      value = ctx.args[++i];
    } else if (arg.rfind("-n", 0) == 0 && arg.size() > 2) {
      value = std::string_view(arg).substr(2);
    } else if (arg.rfind("--max-count=", 0) == 0) {
      value = std::string_view(arg).substr(12);
    } else {
      if (arg != "-n") ctx.err << "minigit log: unexpected argument '" << arg << "'\n";
      ctx.err << "usage: minigit log [-n <count>]\n";
      return kExitUsage;
    }
    limit = parse_count(value);
    if (!limit) {
      ctx.err << "minigit log: '" << value << "' is not a valid commit count\n";
      return kExitUsage;
    }
  }

  const Repository repo = Repository::discover(fs::current_path());
  const HeadState head = resolve_head(repo);
  if (!head.commit) {
    ctx.err << "fatal: your current branch '" << short_branch_name(head.branch_ref)
            << "' does not have any commits yet\n";
    return kExitError;
  }
  print_log(ctx.out, first_parent_log(ObjectStore(repo.objects_dir()), *head.commit, limit));
  return kExitOk;
}

int cmd_status(CommandContext& ctx) {
  bool short_format = false;
  for (const auto& arg : ctx.args) {
    if (arg == "-s" || arg == "--short" || arg == "--porcelain") {
      short_format = true;
    } else {
      ctx.err << "minigit status: unexpected argument '" << arg << "'\n"
              << "usage: minigit status [-s | --short | --porcelain]\n";
      return kExitUsage;
    }
  }

  const Repository repo = Repository::discover(fs::current_path());
  const Status status = compute_status(repo);
  if (short_format) {
    print_status_short(ctx.out, status);
  } else {
    print_status_long(ctx.out, status);
  }
  return kExitOk;
}

int cmd_branch(CommandContext& ctx) {
  for (const auto& arg : ctx.args) {
    if (!arg.empty() && arg.front() == '-') {
      ctx.err << "minigit branch: unknown option '" << arg << "'\n";
      return kExitUsage;
    }
  }
  if (ctx.args.size() > 2) {
    ctx.err << "usage: minigit branch [<name> [<start-point>]]\n";
    return kExitUsage;
  }

  const Repository repo = Repository::discover(fs::current_path());
  if (ctx.args.empty()) {
    // An unborn current branch has no ref yet, so like Git it is not listed.
    const std::string current = short_branch_name(resolve_head(repo).branch_ref);
    for (const auto& name : list_branches(repo)) {
      ctx.out << (name == current ? "* " : "  ") << name << '\n';
    }
    return kExitOk;
  }

  const std::string& name = ctx.args[0];
  if (!is_valid_branch_name(name)) throw BranchError("'" + name + "' is not a valid branch name");
  const std::string start = ctx.args.size() == 2 ? ctx.args[1] : "HEAD";
  std::string oid;
  try {
    oid = resolve_revision(repo, start);
  } catch (const RevisionError&) {
    throw BranchError("not a valid object name: '" + start + "'");
  }
  read_commit(ObjectStore(repo.objects_dir()), oid);  // the start point must be a commit
  create_branch(repo, name, oid);
  return kExitOk;
}

const std::vector<Command>& commands() {
  static const std::vector<Command> table = {
      {"add", "Stage file contents for the next commit", cmd_add},
      {"branch", "List branches or create one", cmd_branch},
      {"cat-file", "Show an object's type, size or contents", cmd_cat_file},
      {"checkout", "Switch to another branch", cmd_checkout},
      {"commit", "Record the staged files as a new commit", cmd_commit},
      {"config", "Get or set user.name / user.email", cmd_config},
      {"hash-object", "Compute a blob id, optionally storing it (-w)", cmd_hash_object},
      {"help", "Show this help message", cmd_help},
      {"init", "Create an empty repository", cmd_init},
      {"log", "Show the commit history of the current branch", cmd_log},
      {"status", "Show staged, modified, deleted and untracked files", cmd_status},
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
