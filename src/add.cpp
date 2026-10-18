#include "minigit/add.hpp"

#include <system_error>

#include "minigit/file_util.hpp"
#include "minigit/object_store.hpp"
#include "minigit/tree.hpp"

namespace minigit {
namespace {

// A pathspec resolved to a worktree-relative path ("" means the whole
// worktree) plus its absolute location.
struct ResolvedPath {
  std::string spec;
  std::string rel;
  fs::path abs;
};

ResolvedPath resolve(const Repository& repo, const fs::path& cwd, const std::string& spec) {
  fs::path abs = fs::path(spec).is_absolute() ? fs::path(spec) : cwd / spec;
  abs = abs.lexically_normal();
  if (abs.filename().empty()) abs = abs.parent_path();  // "dir/" -> "dir"

  const fs::path rel = abs.lexically_relative(repo.worktree());
  if (rel.empty() || *rel.begin() == "..") {
    throw AddError("'" + spec + "' is outside the repository at '" +
                   repo.worktree().generic_string() + "'");
  }

  // Lexical normalization does not see symbolic links, so refuse to walk
  // through one: "link/file" could point anywhere, including outside.
  std::string rel_str;
  fs::path prefix = repo.worktree();
  for (const auto& part : rel) {
    if (part == ".") continue;
    if (part == ".git") throw AddError("'" + spec + "' is inside repository metadata (.git)");
    if (!rel_str.empty()) {
      std::error_code ec;
      if (fs::is_symlink(prefix, ec)) {
        throw AddError("'" + spec + "' is beyond a symbolic link");
      }
      rel_str += '/';
    }
    prefix /= part;
    rel_str += part.generic_string();
  }
  return {spec, rel_str, abs};
}

std::string join(const std::string& dir, const std::string& name) {
  return dir.empty() ? name : dir + '/' + name;
}

class Adder {
 public:
  Adder(const Repository& repo, StagingIndex& index)
      : worktree_(repo.worktree()), store_(repo.objects_dir()), index_(index) {}

  void add(const ResolvedPath& p) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p.abs, ec);
    if (fs::is_symlink(st)) {
      throw AddError("'" + p.spec + "' is a symbolic link; only regular files are supported");
    } else if (fs::is_regular_file(st)) {
      stage_file(p.rel, p.abs);
    } else if (fs::is_directory(st)) {
      add_directory(p.rel, p.abs);
    } else if (!fs::exists(st)) {
      // Pre-checked by add_paths: the path is tracked, so stage its deletion.
      remove_missing(p.rel);
    } else {
      throw AddError("'" + p.spec + "' is not a regular file or directory");
    }
  }

  AddResult& result() { return result_; }

 private:
  void add_directory(const std::string& rel, const fs::path& abs) {
    for (auto it = fs::recursive_directory_iterator(abs); it != fs::recursive_directory_iterator();
         ++it) {
      const fs::path& path = it->path();
      const std::string child =
          join(rel, path.lexically_relative(abs).generic_string());
      const fs::file_status st = it->symlink_status();
      if (path.filename() == ".git") {
        // The repository's own metadata, or a nested repository.
        it.disable_recursion_pending();
      } else if (fs::is_symlink(st)) {
        result_.warnings.push_back("skipping symbolic link '" + child + "'");
      } else if (fs::is_regular_file(st)) {
        stage_file(child, path);
      } else if (!fs::is_directory(st)) {
        result_.warnings.push_back("skipping special file '" + child + "'");
      }
    }
    remove_missing(rel);
  }

  // Unstages tracked paths at or below `rel` that no longer exist on disk.
  void remove_missing(const std::string& rel) {
    for (const auto& path : index_.paths_under(rel)) {
      std::error_code ec;
      if (fs::exists(fs::symlink_status(worktree_ / fs::path(path), ec))) continue;
      index_.remove(path);
      result_.removed.push_back(path);
    }
  }

  void stage_file(const std::string& rel, const fs::path& abs) {
    const std::string oid = store_.write(ObjectType::Blob, read_file(abs));
    index_.stage(rel, {std::string(file_mode(abs)), oid});
    result_.staged.push_back(rel);
  }

  static std::string_view file_mode(const fs::path& abs) {
#ifdef _WIN32
    (void)abs;
    return kModeFile;  // Windows has no executable bit
#else
    const auto perms = fs::status(abs).permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none ? kModeExecutable : kModeFile;
#endif
  }

  fs::path worktree_;
  ObjectStore store_;
  StagingIndex& index_;
  AddResult result_;
};

}  // namespace

AddResult add_paths(const Repository& repo, StagingIndex& index,
                    const std::vector<std::string>& pathspecs, const fs::path& cwd) {
  std::vector<ResolvedPath> resolved;
  for (const auto& spec : pathspecs) resolved.push_back(resolve(repo, cwd, spec));

  // Stage into a copy so that a failure halfway leaves `index` untouched.
  StagingIndex updated = index;
  Adder adder(repo, updated);
  for (const auto& p : resolved) {
    // Check every pathspec matches something before doing any work: a file
    // on disk, or a tracked path whose deletion should be staged.
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p.abs, ec)) && index.paths_under(p.rel).empty()) {
      throw AddError("pathspec '" + p.spec + "' did not match any files");
    }
  }
  for (const auto& p : resolved) adder.add(p);
  index = std::move(updated);
  return std::move(adder.result());
}

}  // namespace minigit
