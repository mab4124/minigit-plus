#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minigit {

namespace fs = std::filesystem;

// Raised when the index file is malformed or an entry is invalid.
class IndexError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// What the next commit will record for one path.
struct IndexEntry {
  std::string mode;  // kModeFile or kModeExecutable
  std::string oid;   // id of the staged blob

  bool operator==(const IndexEntry&) const = default;
};

// MiniGit++'s staging area: a sorted map from worktree-relative path (always
// '/'-separated, e.g. "src/main.cpp") to the staged blob and mode.
//
// This is deliberately NOT Git's binary .git/index format. It is a small text
// file under .git/minigit/index:
//
//   # minigit index v1
//   100644 ce013625030ba8dba906f756967f9e9ca394464a hello.txt
//   100755 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 dir/with space.sh
//
// The path is everything after the second space, so spaces in names are fine.
// Paths containing a newline or NUL cannot be represented and are rejected.
// No stat data (mtime, size) is cached, so later commands re-hash files to
// detect changes.
class StagingIndex {
 public:
  // Loads `file`, or returns an empty index if it does not exist yet.
  // Throws IndexError if the file is malformed.
  static StagingIndex load(const fs::path& file);

  // Writes the index atomically (temp file + rename), so an interrupted
  // save leaves the previous index intact.
  void save() const;

  // Adds or replaces the entry for `path`. Entries that would conflict with
  // it in a tree (a file "a" when staging "a/b", or "a/b" when staging a
  // file "a") are removed, mirroring `git add`. Throws IndexError for paths
  // that are not valid worktree-relative paths, and for invalid modes/ids.
  void stage(const std::string& path, IndexEntry entry);

  // Removes `path`; returns false if it was not staged.
  bool remove(const std::string& path);

  // The entry for `path`, or nullptr.
  const IndexEntry* find(const std::string& path) const;

  const std::map<std::string, IndexEntry>& entries() const { return entries_; }
  bool empty() const { return entries_.empty(); }
  const fs::path& file() const { return file_; }

  // Throws IndexError unless `path` is a normalized relative path with no
  // empty, "." or ".." components, no NUL/newline, and not inside ".git".
  static void validate_path(std::string_view path);

 private:
  explicit StagingIndex(fs::path file) : file_(std::move(file)) {}

  fs::path file_;
  std::map<std::string, IndexEntry> entries_;
};

}  // namespace minigit
