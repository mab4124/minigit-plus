#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minigit/commit.hpp"
#include "minigit/object_store.hpp"
#include "minigit/staging_index.hpp"

namespace minigit {

// How a file differs between two snapshots.
enum class ChangeKind { Added, Modified, Deleted };

std::string_view to_string(ChangeKind kind);

// One changed file. `old_entry` is empty for an addition, `new_entry` for a
// deletion; a modification has both (with a different blob, mode, or both).
struct FileChange {
  std::string path;  // '/'-separated, relative to the repository root
  ChangeKind kind;
  std::optional<IndexEntry> old_entry;
  std::optional<IndexEntry> new_entry;

  bool operator==(const FileChange&) const = default;
};

// The files that differ between trees `from` and `to`, sorted by path. An
// empty `from` means "no files" (for a root commit). Subtrees with the same
// id on both sides are skipped without being read, so the cost grows with
// the size of the change rather than the size of the trees. A file replaced
// by a directory (or the reverse) is a deletion plus additions. Renames are
// not detected. Throws ObjectError if a tree is missing or corrupt.
std::vector<FileChange> diff_trees(const ObjectStore& store,
                                   const std::optional<std::string>& from,
                                   const std::string& to);

// The file at `path` in tree `tree_oid`, found by reading only the trees on
// the way to it, or nullopt if there is no such file. A directory at `path`
// is not a file and also gives nullopt.
std::optional<IndexEntry> lookup_path(const ObjectStore& store, const std::string& tree_oid,
                                      std::string_view path);

// A commit that changed a file, and how.
struct FileHistoryEntry {
  std::string oid;
  Commit commit;
  FileChange change;
};

// The commits that changed `path`, newest first, at most `limit` of them.
// Walks first parents from `start` and compares each commit's version of the
// file with its first parent's (a root commit is compared with nothing), so a
// change merged in from another branch is attributed to the merge commit.
// History is traversed on demand: every call reads the commits and the trees
// along the path again. Throws ObjectError if an object is missing or corrupt.
std::vector<FileHistoryEntry> file_history(const ObjectStore& store, const std::string& start,
                                           std::string_view path, std::size_t limit);

}  // namespace minigit
