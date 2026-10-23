#include "minigit/status.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <ostream>
#include <system_error>

#include "minigit/add.hpp"
#include "minigit/file_util.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"

namespace minigit {
namespace {

bool is_special_mode(const std::string& mode) {
  return mode == kModeSymlink || mode == kModeGitlink;
}

std::vector<StatusEntry> diff_head_index(const std::map<std::string, IndexEntry>& head,
                                         const std::map<std::string, IndexEntry>& index) {
  std::vector<StatusEntry> changes;
  for (const auto& [path, entry] : index) {
    const auto it = head.find(path);
    if (it == head.end()) {
      changes.push_back({path, StatusKind::Added});
    } else if (is_special_mode(it->second.mode) != is_special_mode(entry.mode)) {
      changes.push_back({path, StatusKind::TypeChanged});
    } else if (it->second != entry) {
      changes.push_back({path, StatusKind::Modified});
    }
  }
  for (const auto& [path, entry] : head) {
    if (index.count(path) == 0) changes.push_back({path, StatusKind::Deleted});
  }
  std::sort(changes.begin(), changes.end(),
            [](const StatusEntry& a, const StatusEntry& b) { return a.path < b.path; });
  return changes;
}

// True if some directory between the worktree and `path` is missing, is not
// a directory, or is a symbolic link. Files "behind" a symbolic link are not
// part of the worktree, so the tracked path counts as deleted, as in Git.
bool parent_is_not_a_directory(const fs::path& worktree, const std::string& path) {
  for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(worktree / fs::path(path.substr(0, slash)), ec);
    if (!fs::is_directory(st)) return true;  // also false for symbolic links
  }
  return false;
}

std::optional<StatusKind> worktree_change(const fs::path& worktree, const std::string& path,
                                          const IndexEntry& staged) {
  if (parent_is_not_a_directory(worktree, path)) return StatusKind::Deleted;
  const fs::path abs = worktree / fs::path(path);
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(abs, ec);
  if (!fs::exists(st) || fs::is_directory(st)) return StatusKind::Deleted;
  if (!fs::is_regular_file(st)) return StatusKind::TypeChanged;  // symlink, fifo, ...
  if (is_special_mode(staged.mode)) return StatusKind::TypeChanged;
  if (hash_object(ObjectType::Blob, read_file(abs)) != staged.oid) return StatusKind::Modified;
#ifndef _WIN32
  // Windows cannot represent the executable bit, so a mode recorded
  // elsewhere is not reported as a change there.
  if (worktree_file_mode(abs) != staged.mode) return StatusKind::Modified;
#endif
  return std::nullopt;
}

class UntrackedScanner {
 public:
  explicit UntrackedScanner(const StagingIndex& index) : index_(index) {}

  void scan(const std::string& rel, const fs::path& abs) {
    for (const auto& item : fs::directory_iterator(abs)) {
      const std::string name = item.path().filename().generic_string();
      if (name == ".git") continue;  // repository metadata, or a nested repository
      const std::string child = rel.empty() ? name : rel + '/' + name;
      const fs::file_status st = item.symlink_status();
      if (fs::is_directory(st)) {
        if (has_tracked_below(child)) {
          scan(child, item.path());
        } else if (contains_files(item.path())) {
          untracked_.push_back(child + '/');
        }
      } else if ((fs::is_regular_file(st) || fs::is_symlink(st)) && !is_tracked(child)) {
        untracked_.push_back(child);
      }
    }
  }

  std::vector<std::string> take() {
    std::sort(untracked_.begin(), untracked_.end());
    return std::move(untracked_);
  }

 private:
  bool is_tracked(const std::string& path) const { return index_.find(path) != nullptr; }

  bool has_tracked_below(const std::string& dir) const {
    const std::string prefix = dir + '/';
    const auto it = index_.entries().lower_bound(prefix);
    return it != index_.entries().end() && it->first.compare(0, prefix.size(), prefix) == 0;
  }

  // True if `dir` holds a file or symbolic link anywhere below it. Git does
  // not list directories that are empty or contain only empty directories.
  static bool contains_files(const fs::path& dir) {
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator();
         ++it) {
      const fs::file_status st = it->symlink_status();
      if (it->path().filename() == ".git") {
        it.disable_recursion_pending();
      } else if (fs::is_regular_file(st) || fs::is_symlink(st)) {
        return true;
      }
    }
    return false;
  }

  const StagingIndex& index_;
  std::vector<std::string> untracked_;
};

std::string_view long_label(StatusKind kind) {
  switch (kind) {
    case StatusKind::Added: return "new file:   ";
    case StatusKind::Modified: return "modified:   ";
    case StatusKind::Deleted: return "deleted:    ";
    case StatusKind::TypeChanged: return "typechange: ";
  }
  return "";
}

char short_code(StatusKind kind) {
  switch (kind) {
    case StatusKind::Added: return 'A';
    case StatusKind::Modified: return 'M';
    case StatusKind::Deleted: return 'D';
    case StatusKind::TypeChanged: return 'T';
  }
  return '?';
}

}  // namespace

Status compute_status(const Repository& repo) {
  const ObjectStore store(repo.objects_dir());
  const HeadState head = resolve_head(repo);
  const StagingIndex index = StagingIndex::load(repo.index_path());

  Status status;
  status.branch_ref = head.branch_ref;
  status.has_commits = head.commit.has_value();

  std::map<std::string, IndexEntry> head_files;
  if (head.commit) head_files = read_commit_snapshot(store, *head.commit);

  status.staged = diff_head_index(head_files, index.entries());
  for (const auto& [path, entry] : index.entries()) {
    if (auto kind = worktree_change(repo.worktree(), path, entry)) {
      status.unstaged.push_back({path, *kind});
    }
  }
  // A path deleted from the index but still on disk is untracked again, as
  // in Git; it also appears as a staged deletion.
  UntrackedScanner scanner(index);
  scanner.scan("", repo.worktree());
  status.untracked = scanner.take();
  return status;
}

void print_status_long(std::ostream& out, const Status& status) {
  out << "On branch " << short_branch_name(status.branch_ref) << '\n';
  if (!status.has_commits) out << "\nNo commits yet\n";

  if (!status.staged.empty()) {
    out << "\nChanges to be committed:\n";
    for (const auto& e : status.staged) out << '\t' << long_label(e.kind) << e.path << '\n';
  }
  if (!status.unstaged.empty()) {
    out << "\nChanges not staged for commit:\n"
           "  (use \"minigit add <file>...\" to update what will be committed)\n";
    for (const auto& e : status.unstaged) out << '\t' << long_label(e.kind) << e.path << '\n';
  }
  if (!status.untracked.empty()) {
    out << "\nUntracked files:\n"
           "  (use \"minigit add <file>...\" to include in what will be committed)\n";
    for (const auto& path : status.untracked) out << '\t' << path << '\n';
  }

  out << '\n';
  if (!status.staged.empty()) return;
  if (!status.unstaged.empty()) {
    out << "no changes added to commit (use \"minigit add\")\n";
  } else if (!status.untracked.empty()) {
    out << "nothing added to commit but untracked files present (use \"minigit add\" to track)\n";
  } else if (!status.has_commits) {
    out << "nothing to commit (create/copy files and use \"minigit add\" to track)\n";
  } else {
    out << "nothing to commit, working tree clean\n";
  }
}

void print_status_short(std::ostream& out, const Status& status) {
  std::map<std::string, std::pair<char, char>> codes;
  for (const auto& e : status.staged) codes[e.path] = {short_code(e.kind), ' '};
  for (const auto& e : status.unstaged) {
    auto [it, inserted] = codes.try_emplace(e.path, ' ', ' ');
    it->second.second = short_code(e.kind);
  }
  for (const auto& [path, xy] : codes) out << xy.first << xy.second << ' ' << path << '\n';
  for (const auto& path : status.untracked) out << "?? " << path << '\n';
}

}  // namespace minigit
