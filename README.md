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

Development follows the roadmap in [`docs/roadmap.md`](docs/roadmap.md).
Currently implemented commands:

| Command                                  | Description                                  |
|------------------------------------------|----------------------------------------------|
| `minigit add [--] <path>...`             | Stage files or directories for the next commit |
| `minigit cat-file (-t\|-s\|-p) <object>`  | Show an object's type, size or contents      |
| `minigit commit -m <message>`            | Record the staged files as a new commit      |
| `minigit config <key> [<value>]`         | Get or set `user.name` / `user.email` in `.git/config` |
| `minigit hash-object [-w] <file>`        | Compute a blob id; `-w` also stores the blob |
| `minigit help`                           | Show the list of commands                    |
| `minigit init [dir]`                     | Create an empty repository                   |
| `minigit log [-n <count>]`               | Show the current branch's history, newest first |
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
- Annotated tag objects are not supported. Unknown commit headers (such as
  `gpgsig`) are skipped when parsing commits written by Git.

## Building

Requirements:

- A C++20 compiler (GCC 11+, Clang 14+)
- CMake 3.20+ and Ninja
- OpenSSL (SHA-1) and zlib (object compression) development packages
- SQLite development package (needed by a later milestone)

GoogleTest is downloaded automatically by CMake.

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

## Exit codes

| Code | Meaning                                 |
|------|-----------------------------------------|
| 0    | Success                                 |
| 1    | The command ran but failed              |
| 2    | Invalid command line (usage error)      |
