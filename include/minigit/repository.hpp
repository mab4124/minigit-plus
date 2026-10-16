#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace minigit {

namespace fs = std::filesystem;

// Raised for repository-level problems such as "not a repository".
class RepositoryError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A MiniGit++ repository rooted at a working tree.
//
// On-disk layout (Git compatible where it matters):
//   <worktree>/.git/HEAD             "ref: refs/heads/main"
//   <worktree>/.git/config           minimal core settings
//   <worktree>/.git/objects/         loose objects (milestone 3)
//   <worktree>/.git/refs/heads/      branch references
//   <worktree>/.git/refs/tags/
//   <worktree>/.git/minigit/         MiniGit++-only data (staging index, ...)
//
// The staging index lives under .git/minigit/ because its format is our own
// simplified one, not Git's binary index format.
class Repository {
 public:
  static constexpr const char* kDefaultBranch = "main";

  // Creates a new repository in `worktree` (created if missing).
  // If one already exists it is left untouched and `reinitialized` is set.
  static Repository init(const fs::path& worktree, bool* reinitialized = nullptr);

  // Finds the repository containing `start` by walking up parent directories.
  // Throws RepositoryError if none is found.
  static Repository discover(const fs::path& start);

  // True if `dir` itself is the root of a MiniGit++ repository.
  static bool is_repository_root(const fs::path& dir);

  const fs::path& worktree() const { return worktree_; }
  fs::path git_dir() const { return worktree_ / ".git"; }
  fs::path objects_dir() const { return git_dir() / "objects"; }
  fs::path refs_dir() const { return git_dir() / "refs"; }
  fs::path minigit_dir() const { return git_dir() / "minigit"; }

  // Contents of HEAD, e.g. "refs/heads/main" for a symbolic HEAD.
  // Throws RepositoryError if HEAD is missing or malformed.
  std::string head_ref() const;

 private:
  explicit Repository(fs::path worktree) : worktree_(std::move(worktree)) {}

  fs::path worktree_;
};

}  // namespace minigit
