#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace minigit {

// Author or committer line: "Name <email> <unix seconds> <+hhmm|-hhmm>".
struct Signature {
  std::string name;
  std::string email;
  std::int64_t timestamp = 0;
  std::string timezone = "+0000";

  bool operator==(const Signature&) const = default;
};

std::string format_signature(const Signature& sig);
Signature parse_signature(std::string_view text);

// The commit fields MiniGit++ understands. Headers it does not know (for
// example "gpgsig" or "encoding" in commits written by real Git) are skipped
// when parsing and therefore not preserved.
struct Commit {
  std::string tree;
  std::vector<std::string> parents;
  Signature author;
  Signature committer;
  std::string message;  // stored verbatim, normally ending in '\n'

  bool operator==(const Commit&) const = default;
};

// Serializes in Git's header order: tree, parents, author, committer, a
// blank line, then the message. Throws ObjectError on invalid ids or
// signatures that cannot be represented.
std::string serialize_commit(const Commit& commit);

// Parses commit content (without the object header). Throws ObjectError if a
// required header is missing or malformed.
Commit parse_commit(std::string_view content);

}  // namespace minigit
