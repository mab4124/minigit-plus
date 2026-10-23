#include "minigit/history_index.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "minigit/branch.hpp"
#include "minigit/refs.hpp"

namespace minigit {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
-- One row per indexed commit. `parent` is the first parent (NULL for a root
-- commit) and `depth` the number of first-parent steps to the root.
CREATE TABLE commits (
  oid    TEXT PRIMARY KEY,
  parent TEXT,
  depth  INTEGER NOT NULL
) WITHOUT ROWID;
-- One row per file a commit changed relative to its first parent. The
-- primary key makes "every commit that touched <path>" a range scan.
CREATE TABLE changes (
  path       TEXT NOT NULL,
  commit_oid TEXT NOT NULL,
  status     TEXT NOT NULL,
  old_mode   TEXT,
  new_mode   TEXT,
  old_blob   TEXT,
  new_blob   TEXT,
  PRIMARY KEY (path, commit_oid)
) WITHOUT ROWID;
CREATE INDEX changes_by_commit ON changes (commit_oid);
)sql";

[[noreturn]] void fail(sqlite3* db, const std::string& what) {
  throw HistoryIndexError(what + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
}

// A prepared statement, finalized on destruction.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      fail(db, "cannot prepare history index query");
    }
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  // Parameters are numbered from 1, as in SQLite.
  Statement& bind(int i, std::string_view text) {
    check(sqlite3_bind_text(stmt_, i, text.data(), static_cast<int>(text.size()),
                            SQLITE_TRANSIENT));
    return *this;
  }
  Statement& bind(int i, const std::string& text) { return bind(i, std::string_view(text)); }
  Statement& bind(int i, const std::optional<std::string>& text) {
    if (text) return bind(i, std::string_view(*text));
    check(sqlite3_bind_null(stmt_, i));
    return *this;
  }
  Statement& bind(int i, std::int64_t value) {
    check(sqlite3_bind_int64(stmt_, i, value));
    return *this;
  }

  // Advances to the next row; false when there are no more.
  bool step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    fail(db_, "history index query failed");
  }

  // Runs a statement that returns no rows, then makes it reusable.
  void run() {
    step();
    reset();
  }

  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

  // Columns are numbered from 0.
  std::optional<std::string> text(int i) const {
    const auto* p = sqlite3_column_text(stmt_, i);
    if (!p) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(p),
                       static_cast<std::size_t>(sqlite3_column_bytes(stmt_, i)));
  }
  std::int64_t integer(int i) const { return sqlite3_column_int64(stmt_, i); }

 private:
  void check(int rc) const {
    if (rc != SQLITE_OK) fail(db_, "cannot bind history index query parameter");
  }

  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

void exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    const std::string text = message ? message : "unknown error";
    sqlite3_free(message);
    throw HistoryIndexError("history index statement failed: " + text);
  }
}

// Opens `path` with `flags`; the handle is closed again on failure.
sqlite3* open_db(const fs::path& path, int flags) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(path.string().c_str(), &db, flags, nullptr);
  if (rc != SQLITE_OK) {
    const std::string message = db ? sqlite3_errmsg(db) : "out of memory";
    sqlite3_close(db);
    throw HistoryIndexError("cannot open history index " + path.generic_string() + ": " +
                            message);
  }
  // The server reads while `minigit index` writes: wait for the other side's
  // lock instead of failing at once.
  sqlite3_busy_timeout(db, 5000);
  return db;
}

// The schema version recorded in `db`, or nullopt if it is not a history
// index at all (an empty file, another database, or not SQLite).
std::optional<int> schema_version(sqlite3* db) {
  try {
    Statement q(db, "SELECT value FROM meta WHERE key = 'schema_version'");
    if (!q.step()) return std::nullopt;
    return std::stoi(q.text(0).value_or(""));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void create_schema(sqlite3* db) {
  exec(db, "BEGIN");
  exec(db, kSchema);
  Statement(db, "INSERT INTO meta (key, value) VALUES ('schema_version', ?1)")
      .bind(1, std::to_string(HistoryIndex::kSchemaVersion))
      .run();
  exec(db, "COMMIT");
}

void remove_quietly(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
  fs::remove(fs::path(path.string() + "-journal"), ec);
}

std::optional<IndexEntry> entry_from(const Statement& q, int mode_col, int blob_col) {
  const auto mode = q.text(mode_col);
  const auto blob = q.text(blob_col);
  if (!mode || !blob) return std::nullopt;
  return IndexEntry{*mode, *blob};
}

ChangeKind parse_kind(const std::optional<std::string>& status) {
  if (status == "added") return ChangeKind::Added;
  if (status == "deleted") return ChangeKind::Deleted;
  if (status == "modified") return ChangeKind::Modified;
  throw HistoryIndexError("history index holds an unknown change status '" +
                          status.value_or("") + "'");
}

}  // namespace

void HistoryIndex::Closer::operator()(sqlite3* db) const { sqlite3_close(db); }

fs::path HistoryIndex::default_path(const Repository& repo) {
  return repo.minigit_dir() / "history.db";
}

HistoryIndex::HistoryIndex(Handle db, fs::path final_path, fs::path build_path)
    : db_(std::move(db)), final_path_(std::move(final_path)), build_path_(std::move(build_path)) {}

HistoryIndex::HistoryIndex(HistoryIndex&&) noexcept = default;
HistoryIndex& HistoryIndex::operator=(HistoryIndex&&) noexcept = default;

HistoryIndex::~HistoryIndex() {
  // A rebuild that never completed leaves the old index in place.
  if (!build_path_.empty()) {
    db_.reset();
    remove_quietly(build_path_);
  }
}

HistoryIndex HistoryIndex::open_for_update(const fs::path& db_path, bool rebuild) {
  constexpr int kFlags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  std::error_code ec;
  fs::create_directories(db_path.parent_path(), ec);
  if (!rebuild && fs::exists(db_path)) {
    Handle db(open_db(db_path, kFlags));
    if (schema_version(db.get()) == kSchemaVersion) return HistoryIndex(std::move(db), {}, {});
    rebuild = true;  // another version, or not an index: start over
  }
  if (!rebuild) {
    Handle db(open_db(db_path, kFlags));
    create_schema(db.get());
    return HistoryIndex(std::move(db), {}, {});
  }
  const fs::path build_path = db_path.string() + ".rebuild";
  remove_quietly(build_path);  // left over from an interrupted rebuild
  Handle db(open_db(build_path, kFlags));
  try {
    create_schema(db.get());
  } catch (...) {
    db.reset();
    remove_quietly(build_path);
    throw;
  }
  return HistoryIndex(std::move(db), db_path, build_path);
}

std::optional<HistoryIndex> HistoryIndex::open_existing(const fs::path& db_path) {
  if (!fs::is_regular_file(db_path)) return std::nullopt;
  try {
    Handle db(open_db(db_path, SQLITE_OPEN_READONLY));
    if (schema_version(db.get()) != kSchemaVersion) return std::nullopt;
    return HistoryIndex(std::move(db), {}, {});
  } catch (const HistoryIndexError&) {
    return std::nullopt;
  }
}

bool HistoryIndex::contains(std::string_view oid) const {
  Statement q(db_.get(), "SELECT 1 FROM commits WHERE oid = ?1");
  return q.bind(1, oid).step();
}

std::size_t HistoryIndex::commit_count() const {
  Statement q(db_.get(), "SELECT COUNT(*) FROM commits");
  q.step();
  return static_cast<std::size_t>(q.integer(0));
}

std::vector<FileChange> HistoryIndex::changes(std::string_view oid) const {
  Statement q(db_.get(),
              "SELECT path, status, old_mode, old_blob, new_mode, new_blob FROM changes "
              "WHERE commit_oid = ?1 ORDER BY path");
  q.bind(1, oid);
  std::vector<FileChange> out;
  while (q.step()) {
    out.push_back({q.text(0).value_or(""), parse_kind(q.text(1)), entry_from(q, 2, 3),
                   entry_from(q, 4, 5)});
  }
  return out;
}

std::optional<std::vector<FileHistoryEntry>> HistoryIndex::file_history(
    const ObjectStore& store, const std::string& start, std::string_view path,
    std::size_t limit) const {
  sqlite3* db = db_.get();
  Statement lookup(db, "SELECT parent, depth FROM commits WHERE oid = ?1");
  lookup.bind(1, start);
  if (!lookup.step()) return std::nullopt;
  std::optional<std::string> parent = lookup.text(0);
  std::int64_t depth = lookup.integer(1);
  lookup.reset();

  std::vector<FileHistoryEntry> history;
  if (limit == 0) return history;

  // Every indexed commit that changed `path` and is not newer than `start`,
  // deepest first. Other branches' commits are among them and are skipped
  // by following `start`'s line down to each row's depth.
  Statement rows(db,
                 "SELECT ch.commit_oid, c.depth, ch.status, ch.old_mode, ch.old_blob, "
                 "ch.new_mode, ch.new_blob FROM changes ch JOIN commits c ON c.oid = "
                 "ch.commit_oid WHERE ch.path = ?1 AND c.depth <= ?2 ORDER BY c.depth DESC");
  rows.bind(1, path).bind(2, depth);
  std::string current = start;  // the commit on start's line at `depth`
  while (history.size() < limit && rows.step()) {
    const std::string oid = rows.text(0).value_or("");
    const std::int64_t row_depth = rows.integer(1);
    while (depth > row_depth) {
      if (!parent) throw HistoryIndexError("history index is inconsistent at " + current);
      current = *parent;
      lookup.bind(1, current);
      if (!lookup.step()) throw HistoryIndexError("history index is missing commit " + current);
      parent = lookup.text(0);
      depth = lookup.integer(1);
      lookup.reset();
    }
    if (oid != current) continue;
    FileChange change{std::string(path), parse_kind(rows.text(2)), entry_from(rows, 3, 4),
                      entry_from(rows, 5, 6)};
    history.push_back({oid, read_commit(store, oid), std::move(change)});
  }
  return history;
}

IndexUpdate HistoryIndex::update(const Repository& repo) {
  const ObjectStore store(repo.objects_dir());
  sqlite3* db = db_.get();
  Statement find(db, "SELECT depth FROM commits WHERE oid = ?1");
  auto indexed_depth = [&](const std::string& oid) -> std::optional<std::int64_t> {
    find.bind(1, oid);
    std::optional<std::int64_t> depth;
    if (find.step()) depth = find.integer(0);
    find.reset();
    return depth;
  };

  // Walk back from every branch tip to the first indexed commit. Segments
  // are collected newest first and written oldest first, so a commit's
  // parent always has its depth by the time the commit is written.
  struct Pending {
    std::string oid;
    Commit commit;
  };
  std::vector<std::vector<Pending>> segments;
  std::unordered_set<std::string> seen;
  for (const auto& name : list_branches(repo)) {
    std::optional<std::string> next = repo.read_ref(branch_ref(name));
    std::vector<Pending> segment;
    while (next && !seen.contains(*next) && !indexed_depth(*next)) {
      Commit commit = read_commit(store, *next);
      seen.insert(*next);
      std::optional<std::string> parent;
      if (!commit.parents.empty()) parent = commit.parents.front();
      segment.push_back({std::move(*next), std::move(commit)});
      next = std::move(parent);
    }
    if (!segment.empty()) segments.push_back(std::move(segment));
  }

  // Everything is computed before the transaction starts, so a missing
  // object throws without leaving a write lock behind.
  struct Row {
    std::string oid;
    std::optional<std::string> parent;
    std::int64_t depth;
    std::vector<FileChange> changes;
  };
  std::vector<Row> rows;
  std::unordered_map<std::string, std::pair<std::string, std::int64_t>> batch;  // oid -> tree, depth
  for (auto& segment : segments) {
    for (auto it = segment.rbegin(); it != segment.rend(); ++it) {
      Row row{it->oid, std::nullopt, 0, {}};
      std::optional<std::string> parent_tree;
      if (!it->commit.parents.empty()) {
        row.parent = it->commit.parents.front();
        if (const auto b = batch.find(*row.parent); b != batch.end()) {
          parent_tree = b->second.first;
          row.depth = b->second.second + 1;
        } else {
          parent_tree = read_commit(store, *row.parent).tree;
          row.depth = indexed_depth(*row.parent).value_or(-1) + 1;
        }
      }
      row.changes = diff_trees(store, parent_tree, it->commit.tree);
      batch.emplace(row.oid, std::pair{it->commit.tree, row.depth});
      rows.push_back(std::move(row));
    }
  }

  IndexUpdate result;
  exec(db, "BEGIN IMMEDIATE");
  try {
    Statement add_commit(db, "INSERT INTO commits (oid, parent, depth) VALUES (?1, ?2, ?3)");
    Statement add_change(db,
                         "INSERT INTO changes (path, commit_oid, status, old_mode, new_mode, "
                         "old_blob, new_blob) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    for (const Row& row : rows) {
      add_commit.bind(1, row.oid).bind(2, row.parent).bind(3, row.depth).run();
      for (const FileChange& change : row.changes) {
        auto field = [](const std::optional<IndexEntry>& e, std::string IndexEntry::*member) {
          return e ? std::optional((*e).*member) : std::nullopt;
        };
        add_change.bind(1, change.path)
            .bind(2, row.oid)
            .bind(3, to_string(change.kind))
            .bind(4, field(change.old_entry, &IndexEntry::mode))
            .bind(5, field(change.new_entry, &IndexEntry::mode))
            .bind(6, field(change.old_entry, &IndexEntry::oid))
            .bind(7, field(change.new_entry, &IndexEntry::oid))
            .run();
        ++result.changes_added;
      }
      ++result.commits_added;
    }
    exec(db, "COMMIT");
  } catch (...) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }

  if (!build_path_.empty()) {
    // The rebuilt index is complete: move it into place and keep using it.
    db_.reset();
    std::error_code ec;
    fs::rename(build_path_, final_path_, ec);
    if (ec) {
      remove_quietly(build_path_);
      build_path_.clear();
      throw HistoryIndexError("cannot replace " + final_path_.generic_string() + ": " +
                              ec.message());
    }
    build_path_.clear();
    db_.reset(open_db(final_path_, SQLITE_OPEN_READWRITE));
  }
  result.total_commits = commit_count();
  return result;
}

std::vector<FileHistoryEntry> file_history_with_index(const fs::path& db_path,
                                                      const ObjectStore& store,
                                                      const std::string& start,
                                                      std::string_view path, std::size_t limit,
                                                      bool* used_index) {
  if (used_index) *used_index = false;
  std::optional<std::vector<FileHistoryEntry>> indexed;
  try {
    if (const auto index = HistoryIndex::open_existing(db_path)) {
      indexed = index->file_history(store, start, path, limit);
    }
  } catch (const HistoryIndexError&) {
    indexed.reset();  // a broken index is ignored, not reported
  }
  if (!indexed) return minigit::file_history(store, start, path, limit);
  if (used_index) *used_index = true;
  return std::move(*indexed);
}

}  // namespace minigit
