#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "minigit/repository.hpp"

namespace minigit {

// Raised when a checkout is refused. Nothing in the worktree, index or HEAD
// has been changed when this is thrown.
class CheckoutError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct CheckoutResult {
  std::string branch_ref;            // the branch HEAD now points at
  bool already_on = false;           // it was already the current branch
  std::vector<std::string> written;  // files created or overwritten
  std::vector<std::string> removed;  // tracked files deleted from the worktree
};

// Switches to branch `name`: updates the worktree and staging index to the
// branch's commit, then points HEAD at it (HEAD is written last).
//
// Only paths whose content differs between the current commit and the
// target are touched. Every one of them must be safe to replace, otherwise
// the checkout is refused before anything is written:
//
//   * its staged entry must equal the current commit's (no staged work),
//   * the worktree file must match the index (no local modifications), and
//   * nothing untracked may be in the way: no untracked file at a path the
//     target writes, at one of its parent directories, or inside a directory
//     the target replaces with a file.
//
// Staged and local changes to paths that are the same in both commits are
// kept, as in Git. Only regular files are supported: a symbolic link or
// submodule among the changed paths makes the checkout fail. A missing
// tracked file is not a local change (no data can be lost), so it is simply
// restored or left deleted. Directories left empty by removed files are
// deleted.
//
// Throws CheckoutError if the branch does not exist (a commit id or tag
// would need a detached HEAD, which is not supported) or the checkout is
// unsafe, BranchError if `name` is not a valid branch name, and ObjectError
// if an object is missing or corrupt (also before anything is written).
CheckoutResult checkout_branch(const Repository& repo, const std::string& name);

}  // namespace minigit
