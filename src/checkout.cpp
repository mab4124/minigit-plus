#include "minigit/checkout.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <system_error>

#include "minigit/add.hpp"
#include "minigit/branch.hpp"
#include "minigit/file_util.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"

namespace minigit {
namespace {

using Snapshot = std::map<std::string, IndexEntry>;

std::optional<IndexEntry> lookup(const Snapshot& files, const std::string& path) {
  const auto it = files.find(path);
  if (it == files.end()) return std::nullopt;
  return it->second;
}

bool is_regular_mode(const std::string& mode) {
  return mode == kModeFile || mode == kModeExecutable;
}

// What is on disk at a path, compared with the entry the index expects.
enum class DiskState { Absent, Matches, Differs };

DiskState disk_state(const fs::path& worktree, const std::string& path,
                     const std::optional<IndexEntry>& expected) {
  std::error_code ec;
  for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    // A file "behind" a non-directory parent is not there. Whatever is in
    // the way is examined separately by find_obstructions.
    if (!fs::is_directory(fs::symlink_status(worktree / fs::path(path.substr(0, slash)), ec))) {
      return DiskState::Absent;
    }
  }
  const fs::path abs = worktree / fs::path(path);
  const fs::file_status st = fs::symlink_status(abs, ec);
  if (!fs::exists(st) || fs::is_directory(st)) return DiskState::Absent;
  if (!expected || !fs::is_regular_file(st)) return DiskState::Differs;
  if (hash_object(ObjectType::Blob, read_file(abs)) != expected->oid) return DiskState::Differs;
#ifndef _WIN32
  if (worktree_file_mode(abs) != expected->mode) return DiskState::Differs;
#endif
  return DiskState::Matches;
}

// Paths the checkout refuses to touch, grouped by reason.
struct Conflicts {
  std::vector<std::string> staged;
  std::vector<std::string> local;
  std::vector<std::string> untracked;
  std::vector<std::string> unsupported;

  bool empty() const {
    return staged.empty() && local.empty() && untracked.empty() && unsupported.empty();
  }

  std::string message(const std::string& branch) const {
    std::string text = "cannot switch to branch '" + branch + "':";
    const auto section = [&text](const std::vector<std::string>& paths, const char* what) {
      if (paths.empty()) return;
      text += std::string("\n") + what + "\n";
      for (const auto& path : paths) text += "\t" + path + "\n";
    };
    section(staged, "staged changes to the following files would be lost:");
    section(local, "your local changes to the following files would be overwritten:");
    section(untracked, "the following untracked files would be overwritten or removed:");
    section(unsupported, "the following paths are not regular files (not supported):");
    return text + "Commit your changes (or remove the untracked files) and try again.";
  }
};

class CheckoutPlan {
 public:
  CheckoutPlan(const Repository& repo, const Snapshot& current, const Snapshot& target,
               const StagingIndex& index)
      : repo_(repo), current_(current), target_(target), index_(index) {
    for (const auto& [path, entry] : current_) {
      if (lookup(target_, path) != entry) changed_.push_back(path);
    }
    for (const auto& [path, entry] : target_) {
      if (current_.count(path) == 0) changed_.push_back(path);
    }
    std::sort(changed_.begin(), changed_.end());
  }

  Conflicts check() {
    Conflicts conflicts;
    for (const auto& path : changed_) {
      const auto cur = lookup(current_, path);
      const auto tgt = lookup(target_, path);
      if ((cur && !is_regular_mode(cur->mode)) || (tgt && !is_regular_mode(tgt->mode))) {
        conflicts.unsupported.push_back(path);
        continue;
      }
      const IndexEntry* staged = index_.find(path);
      if (staged ? cur != *staged : cur.has_value()) {
        conflicts.staged.push_back(path);
        continue;
      }
      const DiskState state = disk_state(repo_.worktree(), path, cur);
      if (state == DiskState::Differs) {
        (cur ? conflicts.local : conflicts.untracked).push_back(path);
        continue;
      }
      disk_[path] = state;
      if (tgt) find_obstructions(path, conflicts);
    }
    dedupe(conflicts.staged);
    dedupe(conflicts.untracked);
    return conflicts;
  }

  // Reads every blob the checkout will write, so that a missing or corrupt
  // object fails the checkout before the worktree is modified.
  std::vector<std::pair<std::string, std::string>> load_blobs(const ObjectStore& store) const {
    std::vector<std::pair<std::string, std::string>> blobs;
    for (const auto& path : changed_) {
      const auto tgt = lookup(target_, path);
      if (!tgt) continue;
      RawObject obj = store.read(tgt->oid);
      if (obj.type != ObjectType::Blob) {
        throw ObjectError("object " + tgt->oid + " for '" + path + "' is a " +
                          std::string(to_string(obj.type)) + ", not a blob");
      }
      blobs.emplace_back(path, std::move(obj.content));
    }
    return blobs;
  }

  // Applies the plan; check() must have returned no conflicts.
  void apply(const std::vector<std::pair<std::string, std::string>>& blobs,
             StagingIndex& index, CheckoutResult& result) const {
    const fs::path& worktree = repo_.worktree();
    for (const auto& path : changed_) {
      if (target_.count(path) != 0) continue;
      if (disk_.at(path) == DiskState::Matches) {
        fs::remove(worktree / fs::path(path));
        result.removed.push_back(path);
      }
      prune_empty_parents(path);
    }
    for (const auto& [path, content] : blobs) {
      const fs::path abs = worktree / fs::path(path);
      std::error_code ec;
      // Only empty directories can remain here: check() refused any file
      // below it that is not being removed.
      if (fs::is_directory(fs::symlink_status(abs, ec))) fs::remove_all(abs);
      fs::create_directories(abs.parent_path());
      write_file_atomic(abs, content);
#ifndef _WIN32
      constexpr auto exec = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
      fs::permissions(abs, exec,
                      target_.at(path).mode == kModeExecutable ? fs::perm_options::add
                                                               : fs::perm_options::remove);
#endif
      result.written.push_back(path);
    }
    for (const auto& path : changed_) {
      if (const auto tgt = lookup(target_, path)) {
        index.stage(path, *tgt);
      } else {
        index.remove(path);
      }
    }
  }

 private:
  // True if the target no longer has `path` but the current commit does, so
  // the (verified clean) file there is about to be removed.
  bool being_removed(const std::string& path) const {
    return current_.count(path) != 0 && target_.count(path) == 0;
  }

  // Records anything that would stop `path` from being written: a file at
  // one of its parent directories, or a directory at `path` holding files
  // that are not being removed. The same applies to staged entries, which
  // index.stage() would otherwise silently replace.
  void find_obstructions(const std::string& path, Conflicts& conflicts) const {
    const fs::path& worktree = repo_.worktree();
    std::error_code ec;
    for (auto slash = path.find('/'); slash != std::string::npos;
         slash = path.find('/', slash + 1)) {
      const std::string parent = path.substr(0, slash);
      if (index_.find(parent) != nullptr && !being_removed(parent)) {
        conflicts.staged.push_back(parent);
      }
      const fs::file_status st = fs::symlink_status(worktree / fs::path(parent), ec);
      if (fs::exists(st) && !fs::is_directory(st) && !being_removed(parent)) {
        conflicts.untracked.push_back(parent);
      }
    }
    for (const auto& below : index_.paths_under(path)) {
      if (below != path && !being_removed(below)) conflicts.staged.push_back(below);
    }
    const fs::path abs = worktree / fs::path(path);
    if (!fs::is_directory(fs::symlink_status(abs, ec))) return;
    for (auto it = fs::recursive_directory_iterator(abs); it != fs::recursive_directory_iterator();
         ++it) {
      if (it->is_directory(ec) && !it->is_symlink(ec)) continue;
      const std::string rel = it->path().lexically_relative(worktree).generic_string();
      if (!being_removed(rel)) conflicts.untracked.push_back(rel);
    }
  }

  void prune_empty_parents(const std::string& path) const {
    std::error_code ec;
    for (fs::path dir = (repo_.worktree() / fs::path(path)).parent_path();
         dir != repo_.worktree() && fs::is_directory(fs::symlink_status(dir, ec)) &&
         fs::is_empty(dir, ec);
         dir = dir.parent_path()) {
      fs::remove(dir, ec);
    }
  }

  static void dedupe(std::vector<std::string>& paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  }

  const Repository& repo_;
  const Snapshot& current_;
  const Snapshot& target_;
  const StagingIndex& index_;
  std::vector<std::string> changed_;  // paths that differ between the commits
  std::map<std::string, DiskState> disk_;
};

}  // namespace

CheckoutResult checkout_branch(const Repository& repo, const std::string& name) {
  const std::string ref = branch_ref(name);
  const auto target_oid = repo.read_ref(ref);
  if (!target_oid) {
    bool is_revision = true;
    try {
      resolve_revision(repo, name);
    } catch (const RevisionError&) {
      is_revision = false;
    }
    if (is_revision) {
      throw CheckoutError("'" + name +
                          "' is not a branch; checking out a commit or tag would detach HEAD, "
                          "which is not supported");
    }
    throw CheckoutError("branch '" + name + "' not found");
  }

  CheckoutResult result;
  result.branch_ref = ref;
  const HeadState head = resolve_head(repo);
  if (head.branch_ref == ref) {
    result.already_on = true;
    return result;
  }

  const ObjectStore store(repo.objects_dir());
  const Snapshot current = head.commit ? read_commit_snapshot(store, *head.commit) : Snapshot{};
  const Snapshot target = read_commit_snapshot(store, *target_oid);
  StagingIndex index = StagingIndex::load(repo.index_path());

  CheckoutPlan plan(repo, current, target, index);
  const Conflicts conflicts = plan.check();
  if (!conflicts.empty()) throw CheckoutError(conflicts.message(name));
  const auto blobs = plan.load_blobs(store);

  plan.apply(blobs, index, result);
  index.save();
  // HEAD is moved last: if anything above fails, HEAD still names the old
  // branch (the worktree may then be partly updated, see README).
  repo.set_head(ref);
  return result;
}

}  // namespace minigit
