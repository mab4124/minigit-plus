#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "minigit/repository.hpp"

namespace minigit {

// Raised when a branch cannot be created or does not exist.
class BranchError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// True if `name` is acceptable as a new branch name, i.e. refs/heads/<name>
// passes Repository::is_valid_ref_name and, like `git check-ref-format
// --branch`, the name does not start with '-', end with '/' or '.', and is
// not "HEAD".
bool is_valid_branch_name(const std::string& name);

// "main" -> "refs/heads/main". Throws BranchError if the name is invalid.
std::string branch_ref(const std::string& name);

// Short names of every branch under .git/refs/heads, sorted byte-wise (as
// `git branch` lists them). A branch with no commits yet has no ref file and
// is therefore not listed, as in Git. Files whose names are not valid branch
// names are skipped.
std::vector<std::string> list_branches(const Repository& repo);

// Creates branch `name` pointing at the commit `oid`. Throws BranchError if
// the name is invalid, the branch already exists, or it would clash with an
// existing branch hierarchy ("a/b" when "a" exists, or the reverse).
void create_branch(const Repository& repo, const std::string& name, const std::string& oid);

}  // namespace minigit
