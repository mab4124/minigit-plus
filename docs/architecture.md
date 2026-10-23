# MiniGit++ architecture

MiniGit++ is a small, layered C++20 program. Each layer only uses the ones
below it, and everything except `main()` lives in the `minigit_core` library
so that tests and the benchmark can call it directly.

```text
                 main.cpp ── cli.cpp  (argument parsing, output, exit codes)
                                │
     ┌──────────────┬───────────┼─────────────┬───────────────┐
  add.cpp      create_commit  status.cpp  checkout.cpp   history_api.cpp ── cpp-httplib, nlohmann/json
  branch.cpp   log.cpp                                         │
     │              │             │             │          history.cpp ◄── history_index.cpp ── SQLite
     └──────────────┴──────┬──────┴─────────────┴──────────────┘
                staging_index.cpp   tree.cpp   commit.cpp   refs.cpp   config.cpp
                           │            │           │          │
                     object_store.cpp ── hash.cpp (OpenSSL SHA-1), compression.cpp (zlib)
                           │
                     repository.cpp, file_util.cpp (paths, refs, atomic writes)
```

## On-disk layout

```text
<worktree>/.git/
├── HEAD                  "ref: refs/heads/main" (Git format)
├── config                Git syntax; MiniGit++ reads user.name / user.email
├── objects/xx/yyyy…      zlib-compressed loose objects (Git format)
├── refs/heads/<branch>   one commit id per file (Git format)
└── minigit/              MiniGit++-only data, not understood by Git
    ├── index             the staging index (text, see staging_index.hpp)
    └── history.db        the file-history index (SQLite, derived data)
```

Everything outside `.git/minigit/` uses Git's formats, so real Git can read a
MiniGit++ repository (`git log`, `git cat-file` and `git fsck` are used as
test oracles). The staging index is deliberately *not* Git's binary
`.git/index`: a simple text file is easier to read and reason about, and
pretending to implement Git's format would be misleading.

## Layers

**Storage.** `Repository` finds and initializes `.git`, reads and writes refs
(always by write-to-temp-and-rename, so readers see the old or the new id).
`ObjectStore` frames objects as `<type> <size>\0<content>`, names them by the
SHA-1 of those bytes and stores them compressed. Reads re-hash the content, so
a corrupt object is reported instead of being used.

**Object model.** `tree.cpp` and `commit.cpp` serialize and parse trees
(entries sorted the way Git sorts them, with directories compared as
`name/`) and commits. They are pure functions over bytes; the tests compare
their output with `git hash-object` and `git cat-file`.

**Working state.** `StagingIndex` maps paths to a mode and blob id.
`add.cpp` walks the worktree into it, `tree.cpp` turns it into nested tree
objects, and `create_commit.cpp` writes the commit and moves the branch ref
*last*, so a failure never leaves a branch pointing at a broken commit.
`status.cpp` compares HEAD, the index and the worktree; `checkout.cpp`
refuses to switch branches when a path that differs between the two commits
has local or staged changes, and writes HEAD last.

**Commands.** `cli.cpp` maps each command name to a function that parses its
arguments, calls the layers above and prints results. Errors are C++
exceptions (`RepositoryError`, `ObjectError`, `IndexError`, ...) caught in
one place and printed as `fatal: <message>` with exit code 1; usage errors
exit with 2.

## History API (milestone 9)

`minigit serve` runs `HistoryServer` (`history_api.cpp`) on `127.0.0.1`. It
is read-only and keeps no state between requests: each request opens the
object store and refs again, so new commits are visible immediately. The
history queries themselves live in `history.cpp`, independent of HTTP:

- `diff_trees(from, to)` compares two trees recursively and skips any
  subtree whose id is the same on both sides, so its cost follows the size of
  the change, not the size of the repository.
- `lookup_path(tree, path)` reads only the trees along one path.
- `file_history(start, path, limit)` walks first parents from `start` and
  reports each commit whose version of `path` differs from its parent's.
  Its cost grows with the number of commits walked: a file that has not
  changed for a long time makes it read the whole history.

The server still reads a repository on its own disk. It shows the idea of
answering history questions for clients that have no checkout; it is not a
distributed or remote storage system.

## File-history index (milestone 10)

`history_index.cpp` keeps derived metadata in SQLite so `file_history` does
not have to walk the objects on every request.

```sql
commits (oid PRIMARY KEY, parent, depth)           -- first parent, distance to the root
changes (path, commit_oid, status, old_mode, new_mode, old_blob, new_blob,
         PRIMARY KEY (path, commit_oid))           -- one row per file a commit changed
```

- **Building.** `minigit index` walks back from every branch tip along first
  parents until it meets an indexed commit, diffs each new commit against its
  first parent with `diff_trees`, and inserts everything in one transaction.
  This keeps the invariant *"an indexed commit's whole first-parent line is
  indexed"*, which is what makes the index safe to use: if `HEAD` is in it,
  every commit `file_history` would visit is in it too. Because commits are
  immutable, rows never become wrong; the index can only lag behind.
- **Querying.** The rows for a path come from the primary key, ordered by
  depth. Rows from other branches are discarded by following `HEAD`'s line
  through the `commits` table (parent pointers in SQLite, not objects) down to
  each row's depth. Only the commits that are returned are read from the
  object store, so commit metadata still comes from the source of truth.
- **Falling back.** `file_history_with_index` uses the index only when it
  exists, has the expected schema version and contains the start commit;
  otherwise, or if SQLite reports an error, it traverses. A response's
  `source` field says which happened. The server opens the database
  read-only and never creates or updates it.
- **Rebuilding.** The database can be deleted at any time. `--rebuild` (or a
  file that is not a MiniGit++ index) builds a new database next to the old
  one and renames it into place when complete.

Measurements are in [`experiment-results.md`](experiment-results.md).

## Testing

GoogleTest suites in `tests/` create temporary repositories (`TempDir` in
`test_support.hpp`) and check behaviour through the library, through
`run_cli`, through the real `minigit` executable and over HTTP. Where formats
are shared with Git, the real `git` executable is the oracle: object ids,
tree ids, `git log` output, `git diff --name-status` and
`git log -- <path>`. The index is checked against the traversal it replaces
on generated histories with branches and merges.

## Automation

- `.github/workflows/ci.yml` builds and runs the tests on every push and pull
  request in Debug, Release and ASan/UBSan configurations.
- `.github/workflows/nightly.yml` runs at 18:30 UTC (and on demand): the test
  suite with `MINIGIT_LONG_TESTS=1` (larger generated histories), the
  sanitizer suite repeated three times, the history benchmark on generated
  repositories of 1,000 and 5,000 commits, and `minigit index` through the
  CLI. Logs and benchmark tables are uploaded as artifacts; nothing is
  committed back to the repository.

## Deliberate simplifications

- Loose objects only (no pack files), no packed refs, no detached HEAD, no
  merges, no remotes, no `.gitignore`.
- First-parent history only, in `log`, the API and the index.
- One process at a time is assumed for writes; there are no lock files.
- The index is updated by an explicit `minigit index`, not by `commit`.
