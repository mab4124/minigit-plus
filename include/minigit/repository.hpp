#pragma once

#include <filesystem>
#include <optional>
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
  fs::path config_path() const { return git_dir() / "config"; }
  fs::path objects_dir() const { return git_dir() / "objects"; }
  fs::path refs_dir() const { return git_dir() / "refs"; }
  fs::path minigit_dir() const { return git_dir() / "minigit"; }
  fs::path index_path() const { return minigit_dir() / "index"; }

  // Contents of HEAD, e.g. "refs/heads/main" for a symbolic HEAD.
  // Throws RepositoryError if HEAD is missing or malformed.
  std::string head_ref() const;

  // Points HEAD at the branch `ref` ("ref: <ref>"), replacing the file
  // atomically. Throws RepositoryError if `ref` is not a valid ref name.
  void set_head(const std::string& ref) const;

  // The commit id stored in `ref` (e.g. "refs/heads/main"), or nullopt if the
  // ref file does not exist yet, as on a branch with no commits. Throws
  // RepositoryError if the ref name is invalid or the file does not hold a
  // valid id. Packed refs are not supported.
  std::optional<std::string> read_ref(const std::string& ref) const;

  // Points `ref` at `oid`, creating the ref file if needed. The file is
  // replaced atomically, so readers see either the old or the new id.
  void write_ref(const std::string& ref, const std::string& oid) const;

  // True if `ref` is a name read_ref/write_ref accept: "refs/" followed by
  // components that are not empty, do not start with '.', do not end in
  // ".lock" and contain no spaces, control characters or any of ~^:?*[\.
  // ".." and "@{" are rejected anywhere. A subset of `git check-ref-format`.
  static bool is_valid_ref_name(const std::string& ref);

 private:
  explicit Repository(fs::path worktree) : worktree_(std::move(worktree)) {}

  fs::path worktree_;
};

}  // namespace minigit
