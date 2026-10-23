#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "minigit/history.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"

struct sqlite3;

namespace minigit {

namespace fs = std::filesystem;

// Raised when the history index cannot be opened, read or written.
class HistoryIndexError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// What one call to HistoryIndex::update did.
struct IndexUpdate {
  std::size_t commits_added = 0;  // commits that were not indexed before
  std::size_t changes_added = 0;  // changed-path rows written for them
  std::size_t total_commits = 0;  // commits in the index afterwards
};

// A SQLite database of derived history metadata (milestone 10), stored at
// .git/minigit/history.db.
//
// For every commit on the first-parent line of some branch it records the
// commit's first parent, its first-parent depth (a root commit has depth 0)
// and every file that differs from that parent, with the old and new mode and
// blob id: the same per-commit facts file_history() recomputes from trees on
// each call.
//
// The index is never the source of truth. It can be deleted at any time and
// rebuilt from the objects and refs, and readers fall back to traversing
// objects when it is missing, unreadable or does not cover the commit they
// start from. Commits are immutable, so an indexed commit's rows can never
// become wrong; the only way the index goes stale is by missing new commits.
//
// Invariant: if a commit is indexed, so is its whole first-parent line. Each
// update() writes its commits in one transaction, so an interrupted update
// leaves the previous, still consistent index behind.
class HistoryIndex {
 public:
  // Bumped whenever the schema changes; a database with another version is
  // rebuilt by open_for_update() and ignored by open_existing().
  static constexpr int kSchemaVersion = 1;

  // .git/minigit/history.db
  static fs::path default_path(const Repository& repo);

  // Opens the index at `db_path` for writing, creating it if needed. With
  // `rebuild`, or when the file holds another schema version, the index is
  // built from scratch: into a temporary file that replaces `db_path` only
  // when update() completes, so readers never see a half-built index. Throws
  // HistoryIndexError if the database cannot be opened or created.
  static HistoryIndex open_for_update(const fs::path& db_path, bool rebuild = false);

  // Opens an existing index read-only. Returns nullopt, without creating
  // anything, if the file does not exist, is not a MiniGit++ history index or
  // has another schema version.
  static std::optional<HistoryIndex> open_existing(const fs::path& db_path);

  HistoryIndex(HistoryIndex&&) noexcept;
  HistoryIndex& operator=(HistoryIndex&&) noexcept;
  ~HistoryIndex();

  // Indexes every commit on the first-parent lines of the repository's
  // branches that is not indexed yet, walking back from each branch tip only
  // until it meets an indexed commit. Returns what was added. Throws
  // ObjectError for missing or corrupt objects and HistoryIndexError for
  // database failures; in both cases nothing is written.
  IndexUpdate update(const Repository& repo);

  // True if commit `oid` (and therefore its first-parent line) is indexed.
  bool contains(std::string_view oid) const;

  std::size_t commit_count() const;

  // The files commit `oid` changed relative to its first parent, as stored
  // (the same list diff_trees() gives), sorted by path. Empty if `oid` is not
  // indexed or changed nothing.
  std::vector<FileChange> changes(std::string_view oid) const;

  // The same result as minigit::file_history(store, start, path, limit),
  // answered from the index, or nullopt if `start` is not indexed (the
  // caller should then traverse). The rows for `path` are found by key and
  // sorted by depth; the first-parent line of `start` is then followed in
  // the index (not in the objects) only as far as needed to tell which rows
  // lie on it, and only the commits returned are read from `store`. Throws
  // HistoryIndexError for database failures and ObjectError if a returned
  // commit is missing or corrupt.
  std::optional<std::vector<FileHistoryEntry>> file_history(const ObjectStore& store,
                                                            const std::string& start,
                                                            std::string_view path,
                                                            std::size_t limit) const;

 private:
  struct Closer {
    void operator()(sqlite3* db) const;
  };
  using Handle = std::unique_ptr<sqlite3, Closer>;

  HistoryIndex(Handle db, fs::path final_path, fs::path build_path);

  Handle db_;
  // For a rebuild, update() builds the database at build_path_ and then
  // renames it to final_path_. Both are empty otherwise.
  fs::path final_path_;
  fs::path build_path_;
};

// file_history() answered from the index at `db_path` when it exists and
// covers `start`, otherwise by traversing objects. `used_index` (if given)
// reports which one happened. An index that cannot be opened or read is
// treated as absent, since it is only derived data; there is no partial use
// of an index that lags behind `start`.
std::vector<FileHistoryEntry> file_history_with_index(const fs::path& db_path,
                                                      const ObjectStore& store,
                                                      const std::string& start,
                                                      std::string_view path, std::size_t limit,
                                                      bool* used_index = nullptr);

}  // namespace minigit
