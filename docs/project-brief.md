# Project Master Prompt: MiniGit++ — Build a Git-Inspired Version Control System in C++ with Scion-Inspired Extensions

## 1. Your role

Act as my senior C++ engineer, implementation partner, and technical mentor.

I want you to build an actual, working software project based on this educational repository:

https://github.com/RivaanRanawat/git-clone-python

Reimplement its core concepts and functionality in C++20, then gradually introduce a small number of improvements inspired by Anthropic's article:

https://www.anthropic.com/engineering/redesigning-source-control

I am a fourth-year engineering student with approximately one year of development experience.

**Do the implementation, not just the planning.** Create source files, compile the program, run tests, fix issues, write documentation, and establish GitHub Actions automation.

Keep the project understandable and achievable. Explain important implementation decisions without overwhelming me with advanced systems theory.

Do not generate an enormous codebase in one response. Implement the project incrementally through ten meaningful development commits.

## 2. Project objective

Build a lightweight, educational version control system in C++ called MiniGit++.

The first eight commits will implement a practical subset of Git from scratch. I want to understand how version control works internally, including object storage, staging, commit history, branches and working-tree changes.

The final two commits will explore a real software engineering question inspired by Scion:

**Can repository-history queries be served efficiently through an API, without requiring every consumer to clone or scan the repository?**

We will retain the basic Git-inspired architecture rather than attempting to recreate Scion or build a production-grade distributed version control service.

Use Scion as architectural inspiration, not as a specification to copy.

## 3. Technology stack

Use:

- C++20.
- CMake and Ninja.
- OpenSSL for SHA-1 hashing.
- Zlib for Git-style loose-object compression.
- SQLite for the later file-history index.
- A lightweight HTTP library such as cpp-httplib for the later API.
- nlohmann/json for JSON responses.
- GoogleTest for unit and integration testing.
- GitHub Actions for automated testing.

Check that dependency versions and installation instructions work in the actual environment. Prefer standard C++ and straightforward data structures over complex abstractions.

Do not use libgit2 to implement the core version control functionality. The purpose is to learn by implementing the object model and commands ourselves. Use the real Git executable as a reference and testing oracle where appropriate.

## 4. Scope of the Git implementation

Implement a useful educational subset of Git, inspired by the reference repository.

### Repository initialization

`minigit init`

Create the repository metadata directory, initialize HEAD and the initial branch, and create the directories required for object storage.

### Object storage

Implement three fundamental object types:

- **Blob:** Stores file contents.
- **Tree:** Represents directories and references blobs or subtrees.
- **Commit:** Stores commit metadata, a tree reference, and parent commit references.

Use the standard Git object framing convention for SHA-1 repositories:

`<type> <size>\0<content>`

Hash the framed content and store the compressed object under the corresponding `.git/objects/` path.

Implement the relevant serialization and deserialization logic yourself. Validate it by comparing object hashes and contents against the real Git executable wherever the formats are compatible.

Do not claim support for every Git repository format or Git feature.

### Staging

`minigit add <path>`

Support staging individual files and directory contents.

Maintain a simple internal staging index under a MiniGit-specific metadata directory rather than pretending that a simplified staging format is the full Git index format.

Support regular files first. Handle deletions and nested directories when practical. Do not recurse into `.git` or follow unsafe paths.

### Commit creation

`minigit commit -m "message"`

Construct tree objects from the staged files, create a commit containing author information and a timestamp, record the parent commit when one exists, and advance the current branch reference.

Read the author name and email from configuration or provide sensible local configuration commands.

Ensure that a failed commit does not advance the branch pointer to an invalid commit.

### History

`minigit log`

Display commit identifiers, messages, authors and timestamps by traversing the commit-parent graph.

Support a simple limit option, such as `minigit log -n 10`.

### Working-tree status

`minigit status`

Report at least:

- Staged changes.
- Modified tracked files.
- Deleted tracked files.
- Untracked files.

Keep the initial implementation simple. Document unsupported edge cases instead of silently producing misleading results.

### Branches and checkout

Implement:

- `minigit branch` to list branches.
- `minigit branch <name>` to create a branch.
- `minigit checkout <branch>` to switch branches.

Only implement deletion of branches if the core is already stable.

Checkout must refuse to overwrite local modifications or staged work that would be lost. Restrict the first version to regular tracked files and supported branch states.

### Error handling

Provide useful errors for invalid commands, missing files, malformed objects, invalid references and unsafe checkout operations.

Never silently discard user files to make a command succeed.

## 5. Ten-commit implementation plan

Treat these as ten logical implementation commits, not a requirement to cram all the work into ten giant patches. Each commit must represent one coherent, working increment.

### Commit 1 — Project foundation

Create the repository structure, CMake configuration, dependency setup, CLI argument parser and initial tests.

Add a README with build and run instructions.

Set up the initial GitHub Actions workflow so every push and pull request triggers compilation and tests.

**Deliverable:** A program that compiles, accepts commands and reports invalid arguments correctly.

### Commit 2 — Repository initialization

Implement `minigit init`, repository discovery, HEAD, the initial branch reference and the metadata directory structure.

Test initialization, repeated initialization, and attempts to execute commands outside a repository.

**Deliverable:** A valid initialized repository structure.

### Commit 3 — Git objects

Implement blob, tree and commit object serialization utilities, SHA-1 object identifiers, Zlib compression, object writing and object reading.

Begin with blobs and add the remaining object types as their dependent code becomes ready.

Verify the object framing and hashes against Git-generated fixtures.

**Deliverable:** Immutable objects can be written, read back and validated.

### Commit 4 — File staging

Implement `minigit add` and a persistent, MiniGit-specific staging index.

Support single files, multiple files and nested directories. Skip the repository's internal metadata.

Test repeated staging, empty files, binary files, unusual filenames that the implementation claims to support, and missing paths.

**Deliverable:** Staged file contents are stored as blob objects and survive program restarts.

### Commit 5 — Tree construction

Build tree objects from the staged index, preserving directory hierarchy and file modes for supported regular files.

Ensure deterministic tree serialization and correct ordering according to Git's tree-entry rules.

Test nested trees, empty directories according to the documented scope, and repeatable object identifiers.

**Deliverable:** The staged project can be represented as a tree of immutable objects.

### Commit 6 — Commit creation

Implement `minigit commit -m`, commit metadata, parent relationships, initial commits, subsequent commits and branch-reference updates.

Reject commits with no staged changes unless the behavior is explicitly documented otherwise.

Validate the generated commit data with the real Git executable where compatible.

**Deliverable:** A sequence of commits forms a persistent, traversable history.

### Commit 7 — Log and status

Implement `minigit log` and `minigit status`.

Compare the working tree, staging index and latest commit to determine staged, modified, deleted and untracked files.

Add tests for no changes, staged-only changes, unstaged changes, and mixed changes.

**Deliverable:** The core system can explain the current repository state and show its history.

### Commit 8 — Branches and checkout

Implement branch listing, branch creation and safe switching between branches.

Reconstruct supported working-tree contents from tree and blob objects. Refuse unsafe checkout operations that could destroy local changes.

Test switching branches, returning to previous commits where supported, missing references and interrupted or failed checkouts.

**Deliverable:** A usable educational Git implementation with basic branching.

### Commit 9 — Read-only history API

Now introduce the first Scion-inspired feature.

Implement a lightweight HTTP service that reads an existing MiniGit++ repository and exposes repository metadata over an API.

Provide the following endpoints:

- `GET /health`
- `GET /api/log?limit=20`
- `GET /api/commit/<hash>`
- `GET /api/file-history?path=src/main.cpp&limit=20`
- `GET /api/changed-files?from=<hash>&to=<hash>`

Return JSON responses with useful errors. Bind to localhost by default.

Initially, these operations may traverse commit history and reconstruct tree information on demand. Do not optimize before the API works correctly.

The important idea is that a remote client or CI job can ask about commit history and changed files without maintaining its own checkout.

The server still reads a repository stored on the server. Do not claim that this eliminates repository storage or implements Scion's architecture.

Test these endpoints with integration tests and simple `curl` commands.

**Deliverable:** A working, read-only history service built on top of MiniGit++.

### Commit 10 — File-history index and performance experiment

Add the second Scion-inspired feature: a small, persistent index that accelerates file-history queries.

Use SQLite to store derived metadata associating commit identifiers with changed paths and, where useful, previous and new blob identifiers.

Build the index from the existing commit history. Subsequent indexing should process only previously unindexed commits where possible.

Index data is derived data, not the source of truth. It must be rebuildable from the underlying objects and commit history.

Add a command such as:

`minigit index`

Modify the file-history API to use the index instead of traversing all relevant history on every request.

Benchmark indexed versus unindexed file-history queries on the same repositories.

Record:
- Repository size and commit count.
- Initial indexing time.
- Query latency before and after indexing.
- Database/index size.
- Results after new commits are added.
- Correctness after deleting and rebuilding the derived index.

Do not promise a speedup in advance. Explain what the measurements show and under which workloads indexing is worthwhile.

**Deliverable:** A measured improvement to history-query performance, with a rebuildable index and reproducible test instructions.

## 6. GitHub Actions automation

Automate the engineering workflow, not artificial contribution activity.

### CI on every push and pull request

Create `.github/workflows/ci.yml`.

Run it on pushes, pull requests and manual dispatch. It should configure CMake, compile the project, execute unit and integration tests, and fail when required checks fail.

Add sanitizers where practical. Keep permissions minimal.

### Nightly reliability testing

Create `.github/workflows/nightly.yml` with:

- A schedule of `30 18 * * *`.
- A `workflow_dispatch` trigger.
- Longer indexing and history-query tests.
- Generated test repositories.
- A small performance smoke test.
- Artifact uploads for logs and benchmark output.

The schedule is 18:30 UTC, or midnight IST on the following calendar day. Scheduled GitHub Actions runs can be delayed, so the workflow must not assume it starts at an exact second.

**Do not create fake daily commits. Do not make GitHub Actions commit or push generated log lines to the repository.** CI should build and test the code on every push; the nightly workflow should run additional tests and publish results.

Official documentation: https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows

## 7. Testing strategy

Add tests continuously rather than postponing testing until the end.

Prioritize:

- Object serialization and deserialization.
- Hash and compression correctness.
- Staging and tree construction.
- Commit parent relationships and branch updates.
- Working-tree status.
- Safe checkout.
- API request validation.
- File-history correctness.
- Index rebuild and incremental update behavior.

Use temporary test repositories and compare results against the real Git executable wherever compatible.

Include malformed inputs and filesystem edge cases. Do not test against the same implementation logic exclusively and assume that passing tests prove correctness.

Never claim a test passed unless it was actually executed successfully.

## 8. Required repository structure

Use a straightforward structure similar to:

```text
minigit-plus/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── .github/
│   └── workflows/
│       ├── ci.yml
│       └── nightly.yml
├── include/
│   └── minigit/
│       ├── repository.hpp
│       ├── object_store.hpp
│       ├── staging_index.hpp
│       ├── tree.hpp
│       ├── commit.hpp
│       └── history_api.hpp
├── src/
│   ├── main.cpp
│   ├── repository.cpp
│   ├── object_store.cpp
│   ├── staging_index.cpp
│   ├── tree.cpp
│   ├── commit.cpp
│   └── history_api.cpp
├── tests/
│   ├── object_tests.cpp
│   ├── repository_tests.cpp
│   ├── command_tests.cpp
│   └── api_tests.cpp
├── benchmarks/
│   └── history_benchmark.cpp
└── docs/
    ├── architecture.md
    └── experiment-results.md
```

Adapt the structure as needed. Do not create empty files just to match the template.

## 9. Engineering rules

- Implement incrementally and keep the code understandable.
- Use RAII and appropriate standard-library data structures.
- Separate object storage, repository logic, CLI parsing, and API handling.
- Do not use Git's executable to secretly implement the core commands.
- Use real Git only for reference comparisons, test fixtures, and validation.
- Avoid advanced distributed-systems designs, custom merge algorithms, cloud deployment and unnecessary multithreading.
- Document limitations honestly.
- Benchmark before optimizing.
- Do not claim that MiniGit++ is a complete Git implementation or an equivalent replacement for Scion.

Where a feature is too complicated for the project's scope, simplify it explicitly and explain the trade-off rather than introducing a large framework.

## 10. How to execute the project

Start by inspecting the working directory, available compiler, CMake version, dependency availability and Git state.

Then:

1. Implement Commit 1.
2. Compile and test it.
3. Make a meaningful local Git commit when the milestone is working, provided it is safe to do so.
4. Continue through the roadmap in the same manner.
5. Run the full test suite after meaningful changes.
6. Never overwrite existing user work, reset branches unexpectedly, force-push, or push changes without authorization.

Keep the implementation history meaningful. A commit should represent a tested change, not just an arbitrary day of activity.

If a genuine environment blocker occurs, explain it and implement the parts that can still be completed.

**Begin now: inspect the environment, create the actual project foundation for Commit 1, build it, run its tests, and report the results. Do not stop after presenting another design document.**