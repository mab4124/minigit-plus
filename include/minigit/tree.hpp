#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace minigit {

class ObjectStore;
class StagingIndex;

// File modes as Git writes them in tree objects (note: no leading zero on
// directories). MiniGit++ creates only the first three; the others are
// accepted when parsing trees written by real Git.
inline constexpr std::string_view kModeFile = "100644";
inline constexpr std::string_view kModeExecutable = "100755";
inline constexpr std::string_view kModeTree = "40000";
inline constexpr std::string_view kModeSymlink = "120000";
inline constexpr std::string_view kModeGitlink = "160000";

// One entry of a tree object: a file (blob) or a subdirectory (tree).
struct TreeEntry {
  std::string mode;  // one of the kMode* constants
  std::string name;  // a single path component, never containing '/'
  std::string oid;   // 40-char hex id of the blob or subtree

  bool is_tree() const { return mode == kModeTree; }
  bool operator==(const TreeEntry&) const = default;
};

// Git's tree ordering: byte-wise by name, except that a directory compares
// as if its name ended in '/'. So "a.txt" < "a/" (a directory) < "a0".
bool tree_entry_less(const TreeEntry& a, const TreeEntry& b);

// Serializes entries as Git does: for each entry, in tree_entry_less order,
// "<mode> <name>\0<20 raw id bytes>". Throws ObjectError on an invalid mode,
// name or id, or on duplicate names.
std::string serialize_tree(std::vector<TreeEntry> entries);

// Parses tree content (without the object header). Throws ObjectError if the
// data is truncated or an entry is invalid.
std::vector<TreeEntry> parse_tree(std::string_view content);

// Writes the staged snapshot as tree objects: one tree per directory, each
// subtree written before its parent, and returns the root tree's id. An
// empty index yields Git's empty tree (4b825dc6...). Only directories that
// contain staged files appear, since Git trees cannot record empty
// directories. Blobs are assumed to be in `store` already (`add` writes them).
std::string write_tree_from_index(const ObjectStore& store, const StagingIndex& index);

}  // namespace minigit
