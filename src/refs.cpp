#include "minigit/refs.hpp"

#include "minigit/hash.hpp"

namespace minigit {
namespace {

constexpr std::string_view kHeadsPrefix = "refs/heads/";

}  // namespace

HeadState resolve_head(const Repository& repo) {
  HeadState head;
  head.branch_ref = repo.head_ref();
  head.commit = repo.read_ref(head.branch_ref);
  return head;
}

std::string short_branch_name(std::string_view ref) {
  if (ref.substr(0, kHeadsPrefix.size()) == kHeadsPrefix) ref.remove_prefix(kHeadsPrefix.size());
  return std::string(ref);
}

std::string resolve_revision(const Repository& repo, const std::string& name) {
  if (is_valid_oid(name)) return name;

  if (name == "HEAD") {
    const HeadState head = resolve_head(repo);
    if (!head.commit) {
      throw RevisionError("HEAD does not point at a commit yet (branch '" +
                          short_branch_name(head.branch_ref) + "' has no commits)");
    }
    return *head.commit;
  }

  for (const std::string& candidate : {name, "refs/tags/" + name, "refs/heads/" + name}) {
    // Skipping invalid names (rather than letting read_ref throw) keeps a
    // typo such as "a..b" from being reported as a corrupt repository.
    if (!Repository::is_valid_ref_name(candidate)) continue;
    if (auto oid = repo.read_ref(candidate)) return *oid;
  }
  throw RevisionError("unknown revision '" + name + "'");
}

Commit read_commit(const ObjectStore& store, std::string_view oid) {
  const RawObject obj = store.read(oid);
  if (obj.type != ObjectType::Commit) {
    throw ObjectError("object " + std::string(oid) + " is a " + std::string(to_string(obj.type)) +
                      ", not a commit");
  }
  return parse_commit(obj.content);
}

}  // namespace minigit
