#include "minigit/tree.hpp"

#include <algorithm>
#include <map>

#include "minigit/hash.hpp"
#include "minigit/object_store.hpp"
#include "minigit/staging_index.hpp"

namespace minigit {
namespace {

bool is_known_mode(std::string_view mode) {
  return mode == kModeFile || mode == kModeExecutable || mode == kModeTree ||
         mode == kModeSymlink || mode == kModeGitlink;
}

void validate_entry(const TreeEntry& entry) {
  if (!is_known_mode(entry.mode)) {
    throw ObjectError("tree entry '" + entry.name + "' has unsupported mode '" + entry.mode + "'");
  }
  if (entry.name.empty() || entry.name == "." || entry.name == ".." ||
      entry.name.find_first_of(std::string_view("/\0", 2)) != std::string::npos) {
    throw ObjectError("invalid tree entry name '" + entry.name + "'");
  }
  if (!is_valid_oid(entry.oid)) {
    throw ObjectError("tree entry '" + entry.name + "' has invalid id '" + entry.oid + "'");
  }
}

std::string sort_key(const TreeEntry& entry) {
  return entry.is_tree() ? entry.name + '/' : entry.name;
}

using IndexIterator = std::map<std::string, IndexEntry>::const_iterator;

// Writes the tree for the index entries in [first, last), which all start
// with the same `prefix_len`-byte prefix (empty for the root, otherwise
// "dir/"). The index is sorted, so all keys sharing a "dir/sub/" prefix form
// one contiguous run that can be handed to the recursive call.
std::string write_subtree(const ObjectStore& store, IndexIterator first, IndexIterator last,
                          std::size_t prefix_len) {
  std::vector<TreeEntry> entries;
  while (first != last) {
    const std::string_view rest = std::string_view(first->first).substr(prefix_len);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) {
      entries.push_back({first->second.mode, std::string(rest), first->second.oid});
      ++first;
      continue;
    }
    const std::string dir_name(rest.substr(0, slash));
    const std::string dir_prefix = first->first.substr(0, prefix_len + slash + 1);
    auto end = first;
    while (end != last && end->first.compare(0, dir_prefix.size(), dir_prefix) == 0) ++end;
    entries.push_back(
        {std::string(kModeTree), dir_name, write_subtree(store, first, end, dir_prefix.size())});
    first = end;
  }
  // serialize_tree sorts into Git's order (directories compare as "name/"),
  // so correctness does not depend on the order entries were collected in.
  return store.write(ObjectType::Tree, serialize_tree(std::move(entries)));
}

}  // namespace

bool tree_entry_less(const TreeEntry& a, const TreeEntry& b) {
  // std::string compares through char_traits<char>, which orders bytes as
  // unsigned char, matching Git's memcmp-based ordering.
  return sort_key(a) < sort_key(b);
}

std::string serialize_tree(std::vector<TreeEntry> entries) {
  for (const auto& entry : entries) validate_entry(entry);
  std::sort(entries.begin(), entries.end(), tree_entry_less);
  for (std::size_t i = 1; i < entries.size(); ++i) {
    if (entries[i].name == entries[i - 1].name) {
      throw ObjectError("duplicate tree entry '" + entries[i].name + "'");
    }
  }

  std::string out;
  for (const auto& entry : entries) {
    out += entry.mode;
    out += ' ';
    out += entry.name;
    out += '\0';
    out += hex_decode(entry.oid);
  }
  return out;
}

std::vector<TreeEntry> parse_tree(std::string_view content) {
  std::vector<TreeEntry> entries;
  std::size_t pos = 0;
  while (pos < content.size()) {
    const auto space = content.find(' ', pos);
    if (space == std::string_view::npos) throw ObjectError("tree entry has no mode separator");
    const auto nul = content.find('\0', space + 1);
    if (nul == std::string_view::npos) throw ObjectError("tree entry name is not terminated");
    if (content.size() - (nul + 1) < kSha1RawSize) throw ObjectError("tree entry id is truncated");

    TreeEntry entry;
    entry.mode = std::string(content.substr(pos, space - pos));
    entry.name = std::string(content.substr(space + 1, nul - space - 1));
    entry.oid = hex_encode(content.substr(nul + 1, kSha1RawSize));
    validate_entry(entry);
    entries.push_back(std::move(entry));
    pos = nul + 1 + kSha1RawSize;
  }
  return entries;
}

std::string write_tree_from_index(const ObjectStore& store, const StagingIndex& index) {
  return write_subtree(store, index.entries().begin(), index.entries().end(), 0);
}

}  // namespace minigit
