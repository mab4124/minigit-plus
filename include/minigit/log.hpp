#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minigit/commit.hpp"
#include "minigit/object_store.hpp"

namespace minigit {

// One commit in a history listing.
struct LogEntry {
  std::string oid;
  Commit commit;
};

// Walks history from `start` following only the first parent of each commit
// (the branch's own line of development; for a merge, the commit that was
// checked out when merging). Returns at most `limit` commits, newest first.
// Throws ObjectError if a commit on the way is missing, corrupt or not a commit.
std::vector<LogEntry> first_parent_log(const ObjectStore& store, const std::string& start,
                                       std::optional<std::size_t> limit = std::nullopt);

// Formats a timestamp the way `git log` does by default, in the given
// "+hhmm"/"-hhmm" timezone: "Wed Nov 15 03:43:20 2023 +0530".
std::string format_git_date(std::int64_t timestamp, std::string_view timezone);

// Writes `entries` in `git log`'s default ("medium") format: a "commit" line,
// "Merge:" for merges (parents abbreviated to 7 characters), "Author:",
// "Date:", then the message indented by four spaces, with a blank line
// between commits.
void print_log(std::ostream& out, const std::vector<LogEntry>& entries);

}  // namespace minigit
