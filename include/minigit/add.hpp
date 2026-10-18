#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"

namespace minigit {

// Raised for pathspecs `add` refuses: outside the worktree, inside .git,
// through a symbolic link, or matching nothing.
class AddError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct AddResult {
  std::vector<std::string> staged;    // paths whose blob was (re)staged
  std::vector<std::string> warnings;  // e.g. skipped symbolic links
};

// Stages the files named by `pathspecs` (interpreted relative to `cwd`) into
// `index`. Directories are walked recursively; any directory named ".git" is
// skipped, and symbolic links and special files are skipped with a warning.
// Only regular files are supported.
//
// Every pathspec is validated before anything is staged, so a bad pathspec
// leaves `index` unchanged. Blobs are written to the object store as files
// are staged; the caller saves the index.
AddResult add_paths(const Repository& repo, StagingIndex& index,
                    const std::vector<std::string>& pathspecs, const fs::path& cwd);

}  // namespace minigit
