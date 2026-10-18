#include "minigit/cli.hpp"

#include "minigit/add.hpp"
#include "minigit/file_util.hpp"
#include "minigit/hash.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"

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
  const std::string& oid = ctx.args[1];
  if (!is_valid_oid(oid)) {
    // Abbreviated ids and ref names are not supported yet.
    throw ObjectError("not a valid object name '" + oid + "' (expected a full 40-character id)");
  }

  const Repository repo = Repository::discover(fs::current_path());
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

const std::vector<Command>& commands() {
  static const std::vector<Command> table = {
      {"add", "Stage file contents for the next commit", cmd_add},
      {"cat-file", "Show an object's type, size or contents", cmd_cat_file},
      {"hash-object", "Compute a blob id, optionally storing it (-w)", cmd_hash_object},
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
