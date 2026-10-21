#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "minigit/commit.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"

namespace minigit {

// Raised when a revision name does not resolve to an object.
class RevisionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Where HEAD points: the branch it names and that branch's commit, if any.
struct HeadState {
  std::string branch_ref;             // e.g. "refs/heads/main"
  std::optional<std::string> commit;  // nullopt on a branch with no commits yet
};

// Follows HEAD -> branch -> commit id. Throws RepositoryError if HEAD is
// detached or malformed, or the branch ref holds an invalid id.
HeadState resolve_head(const Repository& repo);

// "refs/heads/main" -> "main". Other refs are returned unchanged.
std::string short_branch_name(std::string_view ref);

// Resolves a revision given on the command line to an object id:
//
//   * a full 40-character lowercase hex id (returned as is, not checked to exist)
//   * "HEAD"
//   * a full ref name such as "refs/heads/main"
//   * a short name, tried as refs/tags/<name>, then refs/heads/<name> (Git's order)
//
// Abbreviated ids, "@", "HEAD~1", "main^" and other revision syntax are not
// supported. Throws RevisionError if nothing matches, including when HEAD's
// branch has no commits yet.
std::string resolve_revision(const Repository& repo, const std::string& name);

// Reads `oid` and parses it as a commit. Throws ObjectError if the object is
// missing, corrupt, or not a commit.
Commit read_commit(const ObjectStore& store, std::string_view oid);

// The files recorded by commit `oid`: its tree, flattened into a map from
// '/'-separated path to mode and blob id (see flatten_tree), which is the
// shape of the staging index. Throws ObjectError if the commit or any tree
// below it is missing, corrupt or of the wrong type.
std::map<std::string, IndexEntry> read_commit_snapshot(const ObjectStore& store,
                                                       std::string_view oid);

}  // namespace minigit
