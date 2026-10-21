#include "minigit/branch.hpp"

#include <algorithm>
#include <system_error>

namespace minigit {

bool is_valid_branch_name(const std::string& name) {
  if (name.empty() || name.front() == '-' || name.back() == '/' || name.back() == '.' ||
      name == "HEAD") {
    return false;
  }
  return Repository::is_valid_ref_name("refs/heads/" + name);
}

std::string branch_ref(const std::string& name) {
  if (!is_valid_branch_name(name)) throw BranchError("'" + name + "' is not a valid branch name");
  return "refs/heads/" + name;
}

std::vector<std::string> list_branches(const Repository& repo) {
  const fs::path heads = repo.refs_dir() / "heads";
  std::vector<std::string> names;
  std::error_code ec;
  if (!fs::is_directory(heads, ec)) return names;
  for (const auto& item : fs::recursive_directory_iterator(heads)) {
    if (!item.is_regular_file()) continue;
    const std::string name = item.path().lexically_relative(heads).generic_string();
    if (is_valid_branch_name(name)) names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

void create_branch(const Repository& repo, const std::string& name, const std::string& oid) {
  const std::string ref = branch_ref(name);
  const fs::path path = repo.git_dir() / ref;
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) throw BranchError("a branch named '" + name + "' already exists");
  if (fs::exists(path, ec)) {
    throw BranchError("cannot create branch '" + name + "': branches named '" + name +
                      "/...' already exist");
  }
  // A file at any parent ("a" when creating "a/b") is an existing branch.
  for (fs::path parent = path.parent_path(); parent != repo.refs_dir() / "heads";
       parent = parent.parent_path()) {
    if (fs::exists(parent, ec) && !fs::is_directory(parent, ec)) {
      throw BranchError("cannot create branch '" + name + "': branch '" +
                        parent.lexically_relative(repo.refs_dir() / "heads").generic_string() +
                        "' already exists");
    }
  }
  repo.write_ref(ref, oid);
}

}  // namespace minigit
