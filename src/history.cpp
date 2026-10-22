#include "minigit/history.hpp"

#include <algorithm>
#include <map>

#include "minigit/refs.hpp"
#include "minigit/tree.hpp"

namespace minigit {
namespace {

// A tree's entries by name; empty for a missing tree.
std::map<std::string, TreeEntry> read_entries(const ObjectStore& store,
                                              const std::optional<std::string>& tree_oid) {
  std::map<std::string, TreeEntry> entries;
  if (!tree_oid) return entries;
  const RawObject obj = store.read(*tree_oid);
  if (obj.type != ObjectType::Tree) throw ObjectError("object " + *tree_oid + " is not a tree");
  for (auto& entry : parse_tree(obj.content)) {
    std::string name = entry.name;
    entries.emplace(std::move(name), std::move(entry));
  }
  return entries;
}

std::string join(const std::string& prefix, const std::string& name) {
  return prefix.empty() ? name : prefix + '/' + name;
}

void diff_recursive(const ObjectStore& store, const std::string& prefix,
                    const std::optional<std::string>& from, const std::optional<std::string>& to,
                    std::vector<FileChange>& out) {
  const auto old_entries = read_entries(store, from);
  const auto new_entries = read_entries(store, to);

  auto visit = [&](const std::string& name, const TreeEntry* a, const TreeEntry* b) {
    if (a && b && *a == *b) return;  // identical file or subtree: nothing below changed
    const std::string path = join(prefix, name);
    const std::optional<std::string> old_tree =
        a && a->is_tree() ? std::optional(a->oid) : std::nullopt;
    const std::optional<std::string> new_tree =
        b && b->is_tree() ? std::optional(b->oid) : std::nullopt;
    if (old_tree || new_tree) diff_recursive(store, path, old_tree, new_tree, out);

    const TreeEntry* old_file = a && !a->is_tree() ? a : nullptr;
    const TreeEntry* new_file = b && !b->is_tree() ? b : nullptr;
    if (!old_file && !new_file) return;
    FileChange change{path, ChangeKind::Modified, std::nullopt, std::nullopt};
    if (old_file) change.old_entry = IndexEntry{old_file->mode, old_file->oid};
    if (new_file) change.new_entry = IndexEntry{new_file->mode, new_file->oid};
    if (!old_file) change.kind = ChangeKind::Added;
    if (!new_file) change.kind = ChangeKind::Deleted;
    out.push_back(std::move(change));
  };

  for (const auto& [name, a] : old_entries) {
    const auto it = new_entries.find(name);
    visit(name, &a, it == new_entries.end() ? nullptr : &it->second);
  }
  for (const auto& [name, b] : new_entries) {
    if (!old_entries.contains(name)) visit(name, nullptr, &b);
  }
}

}  // namespace

std::string_view to_string(ChangeKind kind) {
  switch (kind) {
    case ChangeKind::Added:
      return "added";
    case ChangeKind::Modified:
      return "modified";
    case ChangeKind::Deleted:
      return "deleted";
  }
  return "unknown";
}

std::vector<FileChange> diff_trees(const ObjectStore& store,
                                   const std::optional<std::string>& from,
                                   const std::string& to) {
  std::vector<FileChange> changes;
  if (from && *from == to) return changes;
  diff_recursive(store, "", from, to, changes);
  // Tree order ("a.txt" < "a/") differs from plain path order; sort so the
  // result does not depend on it.
  std::sort(changes.begin(), changes.end(),
            [](const FileChange& x, const FileChange& y) { return x.path < y.path; });
  return changes;
}

std::optional<IndexEntry> lookup_path(const ObjectStore& store, const std::string& tree_oid,
                                      std::string_view path) {
  std::string tree = tree_oid;
  while (true) {
    const auto slash = path.find('/');
    const std::string_view name = path.substr(0, slash);
    const auto entries = read_entries(store, tree);
    const auto it = entries.find(std::string(name));
    if (it == entries.end()) return std::nullopt;
    const TreeEntry& entry = it->second;
    if (slash == std::string_view::npos) {
      if (entry.is_tree()) return std::nullopt;
      return IndexEntry{entry.mode, entry.oid};
    }
    if (!entry.is_tree()) return std::nullopt;
    tree = entry.oid;
    path.remove_prefix(slash + 1);
  }
}

std::vector<FileHistoryEntry> file_history(const ObjectStore& store, const std::string& start,
                                           std::string_view path, std::size_t limit) {
  std::vector<FileHistoryEntry> history;
  std::optional<std::string> next = start;
  // The file as the commit being examined has it; carried over from the
  // previous step so every tree is searched once.
  Commit commit;
  std::optional<IndexEntry> current;
  bool have_current = false;
  while (next && history.size() < limit) {
    if (!have_current) {
      commit = read_commit(store, *next);
      current = lookup_path(store, commit.tree, path);
    }
    std::optional<std::string> parent_oid;
    Commit parent;
    std::optional<IndexEntry> before;
    if (!commit.parents.empty()) {
      parent_oid = commit.parents.front();
      parent = read_commit(store, *parent_oid);
      // Same tree, same file: skip the lookup.
      before = parent.tree == commit.tree ? current : lookup_path(store, parent.tree, path);
    }

    if (current != before) {
      FileChange change{std::string(path), ChangeKind::Modified, before, current};
      if (!before) change.kind = ChangeKind::Added;
      if (!current) change.kind = ChangeKind::Deleted;
      history.push_back({*next, commit, std::move(change)});
    }

    next = std::move(parent_oid);
    commit = std::move(parent);
    current = std::move(before);
    have_current = true;
  }
  return history;
}

}  // namespace minigit
