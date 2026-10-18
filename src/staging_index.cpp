#include "minigit/staging_index.hpp"

#include <system_error>

#include "minigit/file_util.hpp"
#include "minigit/hash.hpp"
#include "minigit/tree.hpp"

namespace minigit {
namespace {

constexpr std::string_view kHeader = "# minigit index v1";

void validate_entry(const IndexEntry& entry) {
  if (entry.mode != kModeFile && entry.mode != kModeExecutable) {
    throw IndexError("unsupported index mode '" + entry.mode + "'");
  }
  if (!is_valid_oid(entry.oid)) throw IndexError("invalid blob id '" + entry.oid + "'");
}

}  // namespace

void StagingIndex::validate_path(std::string_view path) {
  if (path.empty()) throw IndexError("empty path");
  if (path.find_first_of(std::string_view("\n\0", 2)) != std::string_view::npos) {
    throw IndexError("paths containing a newline or NUL byte are not supported");
  }
  if (path.front() == '/') throw IndexError("path '" + std::string(path) + "' is not relative");

  std::size_t start = 0;
  bool first = true;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto end = slash == std::string_view::npos ? path.size() : slash;
    const std::string_view part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") {
      throw IndexError("path '" + std::string(path) + "' is not normalized");
    }
    if (first && part == ".git") {
      throw IndexError("path '" + std::string(path) + "' is inside the repository metadata");
    }
    first = false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
}

StagingIndex StagingIndex::load(const fs::path& file) {
  StagingIndex index(file);
  std::error_code ec;
  if (!fs::exists(file, ec)) return index;

  const std::string text = read_file(file);
  std::size_t pos = 0;
  int line_no = 0;
  while (pos < text.size()) {
    const auto eol = text.find('\n', pos);
    if (eol == std::string::npos) throw IndexError("index is truncated (last line has no newline)");
    const std::string_view line(text.data() + pos, eol - pos);
    pos = eol + 1;
    ++line_no;

    if (line_no == 1) {
      if (line != kHeader) throw IndexError("index has an unknown header: '" + std::string(line) + "'");
      continue;
    }
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
      throw IndexError("index line " + std::to_string(line_no) + " is malformed");
    }
    IndexEntry entry{std::string(line.substr(0, sp1)), std::string(line.substr(sp1 + 1, sp2 - sp1 - 1))};
    const std::string path(line.substr(sp2 + 1));
    try {
      validate_entry(entry);
      validate_path(path);
    } catch (const IndexError& e) {
      throw IndexError("index line " + std::to_string(line_no) + ": " + e.what());
    }
    if (!index.entries_.emplace(path, std::move(entry)).second) {
      throw IndexError("index line " + std::to_string(line_no) + ": duplicate path '" + path + "'");
    }
  }
  if (line_no == 0) throw IndexError("index is empty (missing header)");
  return index;
}

void StagingIndex::save() const {
  std::string text(kHeader);
  text += '\n';
  for (const auto& [path, entry] : entries_) {
    text += entry.mode + ' ' + entry.oid + ' ' + path + '\n';
  }
  write_file_atomic(file_, text);
}

void StagingIndex::stage(const std::string& path, IndexEntry entry) {
  validate_path(path);
  validate_entry(entry);

  // A parent directory of `path` must not be staged as a file ...
  for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    entries_.erase(path.substr(0, slash));
  }
  // ... and `path` itself must not be a directory of staged files. Every
  // "path/..." key sorts directly after "path/" in the map.
  const std::string prefix = path + '/';
  auto it = entries_.lower_bound(prefix);
  while (it != entries_.end() && it->first.compare(0, prefix.size(), prefix) == 0) {
    it = entries_.erase(it);
  }
  entries_[path] = std::move(entry);
}

bool StagingIndex::remove(const std::string& path) { return entries_.erase(path) > 0; }

const IndexEntry* StagingIndex::find(const std::string& path) const {
  const auto it = entries_.find(path);
  return it == entries_.end() ? nullptr : &it->second;
}

}  // namespace minigit
