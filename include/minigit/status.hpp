#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "minigit/repository.hpp"

namespace minigit {

enum class ChangeKind { Added, Modified, Deleted, TypeChanged };

// One changed path, relative to the worktree root and '/'-separated.
struct StatusEntry {
  std::string path;
  ChangeKind kind;

  bool operator==(const StatusEntry&) const = default;
};

// The three-way comparison behind `minigit status`.
struct Status {
  std::string branch_ref;  // e.g. "refs/heads/main"
  bool has_commits = false;

  // HEAD's tree vs the staging index: Added, Modified, Deleted or TypeChanged.
  std::vector<StatusEntry> staged;
  // Staging index vs the worktree, for tracked paths only: Modified, Deleted
  // or TypeChanged (the file was replaced by a symbolic link or special file).
  std::vector<StatusEntry> unstaged;
  // Files and symbolic links that are not in the staging index (so a file
  // whose deletion is staged but which is still on disk is listed here too,
  // as in Git). A directory with no staged files below it is reported once,
  // as "dir/", like Git's default.
  std::vector<std::string> untracked;

  bool clean() const { return staged.empty() && unstaged.empty() && untracked.empty(); }
};

// Compares HEAD, the staging index and the worktree. Every list is sorted by
// path. Tracked files are re-hashed (the index caches no stat data), so this
// reads every tracked file. Directories named ".git" are never entered, and
// there is no .gitignore support: every untracked file is listed.
Status compute_status(const Repository& repo);

// `git status`-like long output.
void print_status_long(std::ostream& out, const Status& status);

// `git status --porcelain`-like output: "XY path" per changed path, where X
// is the staged change and Y the unstaged one (A, M, D, T or space), then
// "?? path" for untracked paths. Paths are never quoted.
void print_status_short(std::ostream& out, const Status& status);

}  // namespace minigit
