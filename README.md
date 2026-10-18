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
| `minigit cat-file (-t\|-s\|-p) <oid>`     | Show an object's type, size or contents      |
| `minigit hash-object [-w] <file>`        | Compute a blob id; `-w` also stores the blob |
| `minigit help`                           | Show the list of commands                    |
| `minigit init [dir]`                     | Create an empty repository                   |
| `minigit version`                        | Print the MiniGit++ version                  |

### Current limitations

- Only loose objects are read and written; pack files are not supported.
- Object ids must be given in full (40 hex characters); abbreviations and
  ref names are not accepted by `cat-file` yet.
- `hash-object` hashes file bytes exactly; Git's line-ending and filter
  conversions are not applied.
- `add` stages regular files only. Symbolic links and special files found
  while walking a directory are skipped with a warning, directories named
  `.git` are never entered, and paths outside the worktree or through a
  symbolic link are refused. File names containing a newline cannot be staged.
- The staging index is MiniGit++'s own text file (`.git/minigit/index`), not
  Git's binary `.git/index`, so `git status` does not see what MiniGit++ staged.
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
