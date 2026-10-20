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
  std::vector<std::string> removed;   // tracked paths staged for deletion
  std::vector<std::string> warnings;  // e.g. skipped symbolic links
};

// Stages the files named by `pathspecs` (interpreted relative to `cwd`) into
// `index`. Directories are walked recursively; any directory named ".git" is
// skipped, and symbolic links and special files are skipped with a warning.
// Only regular files are supported.
//
// Deletions are staged too: a pathspec naming a tracked file (or directory of
// tracked files) that no longer exists removes it from the index, and walking
// a directory removes tracked files below it that have disappeared. A
// pathspec that matches neither a file on disk nor a tracked path is an
// error.
//
// Every pathspec is validated before anything is staged, so a bad pathspec
// leaves `index` unchanged. Blobs are written to the object store as files
// are staged; the caller saves the index.
AddResult add_paths(const Repository& repo, StagingIndex& index,
                    const std::vector<std::string>& pathspecs, const fs::path& cwd);

// The mode `add` records for the regular file at `abs`: kModeExecutable if
// its owner-execute bit is set, otherwise kModeFile. Always kModeFile on
// Windows, which has no executable bit.
std::string_view worktree_file_mode(const fs::path& abs);

}  // namespace minigit
