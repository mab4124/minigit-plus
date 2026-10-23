# MiniGit++

An educational, Git-inspired version control system written in C++20.

MiniGit++ reimplements a practical subset of Git from scratch (object storage,
staging, commits, history, branches) to show how version control works
internally. Later milestones add a small read-only history API and a
file-history index, inspired by Anthropic's
[Redesigning source control](https://www.anthropic.com/engineering/redesigning-source-control)
article. It is a learning project, **not** a complete Git implementation.

The original concepts come from
[RivaanRanawat/git-clone-python](https://github.com/RivaanRanawat/git-clone-python).

## Status

Development follows the roadmap in [`docs/roadmap.md`](docs/roadmap.md); the
design is described in [`docs/architecture.md`](docs/architecture.md).
Currently implemented commands:

| Command                                  | Description                                  |
|------------------------------------------|----------------------------------------------|
| `minigit add [--] <path>...`             | Stage files or directories for the next commit |
| `minigit branch [<name> [<start>]]`      | List branches (current marked `*`) or create one |
| `minigit cat-file (-t\|-s\|-p) <object>`  | Show an object's type, size or contents      |
| `minigit checkout <branch>`              | Switch branches, refusing to lose local or staged work |
| `minigit commit -m <message>`            | Record the staged files as a new commit      |
| `minigit config <key> [<value>]`         | Get or set `user.name` / `user.email` in `.git/config` |
| `minigit hash-object [-w] <file>`        | Compute a blob id; `-w` also stores the blob |
| `minigit help`                           | Show the list of commands                    |
| `minigit index [--rebuild]`              | Build or incrementally update the file-history index in `.git/minigit/history.db` |
| `minigit init [dir]`                     | Create an empty repository                   |
| `minigit log [-n <count>]`               | Show the current branch's history, newest first |
| `minigit serve [--port <port>]`          | Serve a read-only JSON history API on `127.0.0.1` (default port 8080; 0 picks a free one) |
| `minigit status [-s\|--short]`           | Show staged, modified, deleted and untracked files |
| `minigit version`                        | Print the MiniGit++ version                  |

### Current limitations

- Only loose objects are read and written; pack files are not supported.
- Objects are named by a full 40-character id, `HEAD`, a full ref name
  (`refs/heads/main`) or a short branch/tag name (tags win, as in Git).
  Abbreviated ids and revision syntax such as `HEAD~1`, `main^` or `@` are not
  supported.
- `hash-object` hashes file bytes exactly; Git's line-ending and filter
  conversions are not applied.
- `add` stages regular files only. Symbolic links and special files found
  while walking a directory are skipped with a warning, directories named
  `.git` are never entered, and paths outside the worktree or through a
  symbolic link are refused. File names containing a newline cannot be staged.
- `add` stages deletions: naming a removed tracked file or directory, or adding
  a directory whose tracked files have disappeared, unstages them. A tracked
  file that was replaced by a symbolic link keeps its old staged entry.
- Non-ASCII file names are stored as their raw UTF-8 bytes on Linux/macOS; on
  Windows they currently go through the narrow code page and are not tested.
- The staging index is MiniGit++'s own text file (`.git/minigit/index`), not
  Git's binary `.git/index`, so `git status` does not see what MiniGit++ staged.
- `config` only reads and writes the repository's `.git/config` and only
  accepts `user.name` and `user.email`. The global `~/.gitconfig` is not read.
  Values are stored in Git's syntax (quoted and escaped where needed) so real
  Git can read them; subsections, `include` directives and line continuations
  are not supported, though existing lines MiniGit++ does not understand are
  preserved.
- `commit` needs `user.name` and `user.email` set with `minigit config`; Git's
  `GIT_AUTHOR_*` / `GIT_COMMITTER_*` environment variables are ignored, and the
  author is also recorded as the committer. Times use the local timezone.
- `commit` refuses to record a tree identical to the parent's (there is no
  `--allow-empty`). Messages are cleaned like Git's default (trailing
  whitespace and surrounding blank lines removed) but `#` lines are kept.
- The branch ref is written last, by atomic rename, so a failed commit never
  moves the branch. There is no lock file: two simultaneous commits are
  detected on a best-effort basis only. HEAD must point at a branch (detached
  HEAD is not supported) and packed refs are not read.
- `log` follows first parents only (like `git log --first-parent`) from the
  current branch; it takes no revision or path arguments and has no
  `--oneline` or other formats. Its output matches Git's default format,
  except that merge parents on the `Merge:` line are always abbreviated to 7
  characters (Git lengthens them if 7 is ambiguous).
- `status` compares HEAD, MiniGit++'s staging index and the worktree. What it
  does not do (yet):
  - **No `.gitignore` support**: every untracked file is listed. A directory
    with no staged files below it is shown once as `dir/`, like Git's default
    (`-unormal`); there is no `-uall`/`-uno`.
  - **No rename detection**: a moved file shows as a deletion plus an
    addition (Git's `--no-renames`).
  - Paths are always relative to the repository root (even when run from a
    subdirectory) and are never quoted, so `--short` output for names with
    spaces, quotes or non-ASCII characters differs from `git status
    --porcelain`, which quotes them.
  - No pathspec arguments, branch/ahead-behind information, `-b`, `-z` or
    `--porcelain=v2`; `--porcelain` is accepted as a synonym for `--short`.
  - Every tracked file is re-read and hashed on each run (no stat cache), so
    it is slower than Git on large trees but never fooled by timestamps.
  - Executable-bit changes are reported (even though `init` writes
    `core.filemode = false`, which makes real Git ignore them); on Windows the
    bit is never compared.
  - Symbolic links are listed as untracked but cannot be staged; a tracked
    file replaced by a symbolic link is a `typechange`. A tracked file
    replaced by a directory, or reached through a symbolic link, is reported
    as deleted. Submodules and special files are not supported.
  - A nested repository that contains only `.git` metadata is not listed.
- `branch` lists and creates branches only: there is no deletion, renaming,
  `-v`, `-a` or remote branches. Names follow `git check-ref-format --branch`
  (a subset of its rules, see `is_valid_branch_name`). A branch is created at
  `HEAD` or at a start point given as a full id or branch/tag name; it does not
  switch to the new branch.
- `checkout` switches between existing branches only: there is no `-b`, no
  checking out of commits or tags (that needs a detached HEAD) and no
  `checkout -- <path>`. Only paths that differ between the two commits are
  touched. It refuses, before changing anything, when one of those paths has
  staged changes, local modifications, or an untracked file or directory in
  the way; changes to paths that are the same on both branches are carried
  over, as in Git. A tracked file that was deleted locally is simply restored
  (Git does the same). Symbolic links and submodules among the changed paths
  are refused, since only regular files are supported. Updates are applied
  file by file (each file atomically) and HEAD is written last, but the
  checkout as a whole is not transactional: an I/O error halfway leaves the
  worktree partly updated while HEAD still names the old branch. All blobs
  to be written are read into memory first, so a missing object fails
  before anything is touched.
- `serve` always binds to `127.0.0.1` and speaks plain HTTP with no
  authentication, so it is meant for local use only; there is no option to
  listen on other interfaces. It never writes to the repository. See
  [History API](#history-api) for the endpoints.
- `index` stores derived data in a SQLite database, `.git/minigit/history.db`:
  for every commit on the first-parent line of some branch, its first parent,
  its depth and the files it changed relative to that parent (old/new mode and
  blob id). A later `index` walks back from each branch tip only until it
  reaches an indexed commit, and writes everything in one transaction, so an
  interrupted run leaves the previous index intact. `--rebuild` builds a
  fresh index next to the old one and swaps it in when complete. The index is
  never the source of truth: it can be deleted at any time, and a file that
  is not a MiniGit++ index (or has another schema version) is rebuilt. It is
  not updated automatically by `commit`; tags and commits reachable only as
  second parents of merges are not indexed. On Windows the swap after
  `--rebuild` fails while another process (such as `serve`) has the database
  open.
- Annotated tag objects are not supported. Unknown commit headers (such as
  `gpgsig`) are skipped when parsing commits written by Git.

## History API

`minigit serve` starts a small read-only HTTP service for the repository it is
run in, so that a script or CI job can ask about history without its own
checkout. Every request re-reads refs and objects, so new commits show up
immediately. Responses are JSON; errors are `{"error": "..."}` with a 4xx or
5xx status.

| Endpoint                     | Response |
|------------------------------|----------|
| `GET /health`                | `{"status": "ok"}` |
| `GET /api/log?limit=20`      | `{"branch", "head", "commits": [...]}`: the current branch's first-parent history, newest first. `limit` defaults to 20 and must be 0–1000 |
| `GET /api/commit/<id>`       | One commit. `<id>` must be a full 40-character id (400 otherwise); 404 if missing or not a commit |
| `GET /api/file-history?path=src/main.cpp&limit=20` | `{"path", "branch", "source", "commits": [...]}`: commits on the current branch that changed the file, newest first, each with a `change`. `source` is `index` or `traversal` (see below) |
| `GET /api/changed-files?from=<rev>&to=<rev>` | `{"from", "to", "changes": [...]}`: files that differ between two commits, sorted by path. `from`/`to` take a full id, `HEAD` or a branch/tag name |

A commit is returned as `{"id", "tree", "parents", "author", "committer",
"summary", "message"}`, where `author` and `committer` are `{"name", "email",
"timestamp", "timezone", "date"}` and `date` uses `git log`'s format. A corrupt
object or unreadable ref gives a 500 with the reason. A change is `{"path",
"status", "old_mode", "new_mode", "old_blob", "new_blob"}` with `status` one of
`added`, `modified` or `deleted`, and `null` on the side without the file.

Limitations of the history endpoints:

- Without an index they traverse objects on every request: `file-history`
  walks the branch's first-parent line and looks the path up in each commit's
  tree (reading only the trees along the path), and `changed-files` compares
  the two root trees, skipping subtrees whose ids match.
- After `minigit index`, `file-history` is answered from
  `.git/minigit/history.db` whenever the index contains `HEAD` (`"source":
  "index"`): the rows for the path are looked up by key, `HEAD`'s first-parent
  line is followed in the index to discard other branches' rows, and only the
  returned commits are read from the object store. The answer is the same as
  traversal's (tests compare the two on generated histories). If `HEAD` has
  moved past the indexed commits, the index is missing or it cannot be read,
  the server traverses instead (`"source": "traversal"`); it never writes the
  index itself, so run `minigit index` again after committing. A lagging
  index is not partly used. `changed-files` always traverses.
- `file-history` follows first parents only and compares each commit with its
  first parent, so a change merged from another branch is reported at the
  merge commit (like `git log --first-parent -- <path>`). It always starts at
  `HEAD`, takes one file path (not a directory or pattern) and does not follow
  renames.
- `changed-files` does not detect renames or copies (Git's `--no-renames`); a
  file replaced by a directory is a deletion plus additions. A mode-only change
  is reported as `modified`.

Example session (start the server in one terminal, query it from another):

```sh
cd my-project
minigit serve --port 8080
# Serving the history of /home/me/my-project at http://127.0.0.1:8080/

curl -s http://127.0.0.1:8080/health
curl -s 'http://127.0.0.1:8080/api/log?limit=5'
curl -s http://127.0.0.1:8080/api/commit/<full-commit-id>
curl -s 'http://127.0.0.1:8080/api/file-history?path=src/main.cpp&limit=20'
curl -s 'http://127.0.0.1:8080/api/changed-files?from=<old-id>&to=main'

# Errors carry a status code and a message (bodies are pretty-printed JSON):
curl -s -w '%{http_code}\n' http://127.0.0.1:8080/api/commit/abc123
# {
#   "error": "'abc123' is not a full 40-character commit id"
# }
# 400
```

Paths with spaces or other special characters must be URL-encoded
(`path=my%20file.txt`). The test `ServeCommand.CurlExamplesWorkEndToEnd` runs
these requests with `curl` against the real executable.

The server still reads a repository stored on the server's disk; it is an
experiment inspired by Scion, not an implementation of it.

## Building

Requirements:

- A C++20 compiler (GCC 11+, Clang 14+)
- CMake 3.20+ and Ninja
- OpenSSL (SHA-1) and zlib (object compression) development packages
- SQLite 3 development package (the file-history index)

GoogleTest, [cpp-httplib](https://github.com/yhirose/cpp-httplib) and
[nlohmann/json](https://github.com/nlohmann/json) are downloaded automatically
by CMake (cpp-httplib with `git`, so `git` must be installed when configuring).

### Linux (Ubuntu)

```sh
sudo apt-get install ninja-build libssl-dev zlib1g-dev libsqlite3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows (MSYS2 UCRT64)

```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,openssl,zlib,sqlite3}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

If you build from Git Bash instead of the MSYS2 shell, put
`C:\msys64\ucrt64\bin` first on your `PATH`, otherwise the compiler can pick up
incompatible DLLs and fail without an error message.

### Sanitizers

```sh
cmake -S . -B build-asan -G Ninja -DMINIGIT_SANITIZE=ON
```

## Benchmark

`benchmarks/history_benchmark.cpp` generates a repository with the MiniGit++
library, then compares file-history queries by traversal with queries through
the index, before and after new commits, and checks a deleted-and-rebuilt
index against traversal. It prints Markdown tables; use a Release build:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/history_benchmark --commits 5000 --files 200 --repeats 5
```

Measured results and what they mean are in
[`docs/experiment-results.md`](docs/experiment-results.md). A tiny run
(`history_benchmark_smoke`) is part of `ctest`.

## Continuous integration

- `ci.yml` builds and tests every push and pull request (Debug, Release and
  ASan/UBSan).
- `nightly.yml` runs at 18:30 UTC (midnight IST) and on manual dispatch: the
  test suite with `MINIGIT_LONG_TESTS=1` (larger generated histories), the
  sanitizer suite three times, the history benchmark on generated
  repositories of 1,000 and 5,000 commits and `minigit index` on a generated
  repository. Logs and benchmark tables are uploaded as workflow artifacts;
  the workflow never commits anything. Timings are recorded, not gated,
  because shared runners are noisy.

## Exit codes

| Code | Meaning                                 |
|------|-----------------------------------------|
| 0    | Success                                 |
| 1    | The command ran but failed              |
| 2    | Invalid command line (usage error)      |
