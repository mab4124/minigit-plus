# MiniGit++ Roadmap

The full specification is in [`project-brief.md`](project-brief.md). This file
tracks progress. Each unchecked box is one small, tested commit. The ten
milestones from the brief are grouped into eight working days.

Rules for every commit:

- The project must build and **all** tests must pass before committing.
- Tests are added in the same commit as the code they cover.
- Tick the box (`[x]`) in this file as part of the commit that completes it.
- Never commit generated output, build directories or fake activity.

## Day 1 — 2026-10-16 · Milestones 1–2: foundation and `init`

- [x] CMake project skeleton and CLI command dispatcher
- [x] GitHub Actions CI (Debug, Release, ASan/UBSan)
- [x] README and project brief
- [x] `.gitattributes` for LF line endings
- [x] `Repository` with init and discovery, plus a real-git oracle test
- [x] `minigit init` command

## Day 2 — 2026-10-17 · Milestone 3: Git objects

- [x] SHA-1 (OpenSSL) and zlib wrappers with RAII, linked through CMake `find_package`
- [x] Object framing `<type> <size>\0<content>`, loose-object write/read in `.git/objects/xx/yyyy…`
- [ ] Blob, tree and commit serialization/parsing; validate hashes against `git hash-object` / `git cat-file`
- [ ] `minigit hash-object [-w] <file>` and `minigit cat-file (-t|-s|-p) <oid>`; malformed-object errors

## Day 3 — 2026-10-18 · Milestone 4: staging

- [ ] `StagingIndex` (path → blob id, mode) persisted under `.git/minigit/index` (text format, atomic write via temp file + rename)
- [ ] `minigit add <path>...` for files and directories; skip `.git`; reject paths outside the worktree
- [ ] Staging of deletions (`add` on a removed tracked path)
- [ ] Tests: repeated staging, empty files, binary files, spaces in names, missing paths, restart persistence

## Day 4 — 2026-10-19 · Milestones 5–6: trees and commits

- [ ] Build nested tree objects from the index with Git's entry ordering (directories sort as `name/`); `git` oracle check of tree ids
- [ ] Config: `minigit config user.name|user.email [value]` stored in `.git/config`
- [ ] `minigit commit -m <msg>`: commit object, parent, branch ref update written last (atomic); reject empty commits
- [ ] Tests: initial + subsequent commits, parent chain, `git cat-file -p` / `git log` oracle on our commits

## Day 5 — 2026-10-20 · Milestone 7: log and status

- [ ] Ref resolution helpers (HEAD → branch → commit id) shared by commands
- [ ] `minigit log [-n N]` traversing first parents
- [ ] `minigit status`: staged, modified, deleted, untracked
- [ ] Tests: no changes, staged-only, unstaged-only, mixed; document unsupported cases in README

## Day 6 — 2026-10-21 · Milestone 8: branches and checkout

- [ ] `minigit branch` (list, current marked with `*`) and `minigit branch <name>` with name validation
- [ ] Read a commit's tree into a flat path → blob map
- [ ] `minigit checkout <branch>`: refuse when local or staged changes would be lost; update worktree, index, HEAD
- [ ] Tests: switching back and forth, missing refs, refused unsafe checkout, file add/remove across branches

## Day 7 — 2026-10-22 · Milestone 9: read-only history API

- [ ] Add cpp-httplib and nlohmann/json via FetchContent; `minigit serve [--port N]` bound to 127.0.0.1
- [ ] `GET /health`, `GET /api/log?limit=`, `GET /api/commit/<hash>`
- [ ] `GET /api/file-history?path=&limit=` and `GET /api/changed-files?from=&to=` (on-demand traversal)
- [ ] Integration tests for every endpoint including error responses; `curl` examples in README

## Day 8 — 2026-10-23 · Milestone 10: file-history index and experiment

- [ ] SQLite index (`.git/minigit/history.db`): commits and changed paths with old/new blob ids; `minigit index [--rebuild]`, incremental
- [ ] File-history API uses the index when present and fresh, falls back to traversal otherwise; equivalence tests
- [ ] `benchmarks/history_benchmark.cpp` on generated repositories; results in `docs/experiment-results.md`
- [ ] `.github/workflows/nightly.yml` (`30 18 * * *` + `workflow_dispatch`): long tests, generated repos, perf smoke test, artifact upload; `docs/architecture.md`
