#pragma once

#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>

#include "minigit/commit.hpp"
#include "minigit/repository.hpp"

namespace minigit {

// Raised when a commit is refused: nothing staged, no identity, empty message,
// or the branch moved while the commit was being created.
class CommitError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct CommitResult {
  std::string oid;     // the new commit
  std::string branch;  // the ref that was advanced, e.g. "refs/heads/main"
  bool root = false;   // true if the commit has no parent
};

// Turns the message into the form stored in the commit: trailing whitespace
// is removed from every line, leading and trailing blank lines are dropped,
// and the result ends with exactly one '\n'. Throws CommitError if nothing is
// left. Unlike Git, '#' lines are kept (there is no editor template to strip).
std::string clean_commit_message(std::string_view message);

// Formats a UTC offset in seconds as Git's "+hhmm" / "-hhmm".
std::string format_utc_offset(std::int64_t offset_seconds);

// The local timezone's offset from UTC, in seconds, at time `t`.
std::int64_t local_utc_offset(std::time_t t);

// A signature for `name` <`email`> at time `now` in the local timezone.
Signature make_signature(const std::string& name, const std::string& email, std::time_t now);

// Records the staging index as a new commit on the branch HEAD points to.
//
// The tree and commit objects are written first and the branch ref last, by
// atomic rename, so a failure at any point leaves the branch on its previous
// commit (at worst with unreferenced objects in the store). The commit's
// parent is the branch's current commit, if any. `author` is used as the
// committer too.
//
// Throws CommitError if the index records the same tree as the parent commit
// (or is empty on a branch with no commits), or if the branch was changed by
// someone else in the meantime.
CommitResult create_commit(const Repository& repo, const std::string& message,
                           const Signature& author);

}  // namespace minigit
