#include "minigit/create_commit.hpp"

#include <cstdlib>

#include "minigit/object_store.hpp"
#include "minigit/staging_index.hpp"
#include "minigit/tree.hpp"

namespace minigit {
namespace {

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Lets us turn a broken-down local time back into seconds
// without timegm(), which is not portable.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

}  // namespace

std::string clean_commit_message(std::string_view message) {
  std::string out;
  std::size_t pos = 0;
  while (pos <= message.size()) {
    auto eol = message.find('\n', pos);
    if (eol == std::string_view::npos) eol = message.size();
    std::string_view line = message.substr(pos, eol - pos);
    while (!line.empty() && is_blank(line.back())) line.remove_suffix(1);
    if (!line.empty() || !out.empty()) {  // drop leading blank lines
      out += line;
      out += '\n';
    }
    pos = eol + 1;
  }
  while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
    out.pop_back();  // collapse trailing blank lines
  }
  if (out.empty()) throw CommitError("aborting commit due to empty commit message");
  return out;
}

std::string format_utc_offset(std::int64_t offset_seconds) {
  const char sign = offset_seconds < 0 ? '-' : '+';
  const std::int64_t minutes = std::llabs(offset_seconds) / 60;
  const std::int64_t hh = minutes / 60;
  const std::int64_t mm = minutes % 60;
  std::string out(1, sign);
  out += static_cast<char>('0' + hh / 10 % 10);
  out += static_cast<char>('0' + hh % 10);
  out += static_cast<char>('0' + mm / 10);
  out += static_cast<char>('0' + mm % 10);
  return out;
}

std::int64_t local_utc_offset(std::time_t t) {
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  const std::int64_t local_seconds =
      days_from_civil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                      static_cast<unsigned>(local.tm_mday)) *
          86400 +
      local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  return local_seconds - static_cast<std::int64_t>(t);
}

Signature make_signature(const std::string& name, const std::string& email, std::time_t now) {
  return {name, email, static_cast<std::int64_t>(now), format_utc_offset(local_utc_offset(now))};
}

CommitResult create_commit(const Repository& repo, const std::string& message,
                           const Signature& author) {
  const ObjectStore store(repo.objects_dir());
  const StagingIndex index = StagingIndex::load(repo.index_path());
  const std::string branch = repo.head_ref();
  const std::optional<std::string> parent = repo.read_ref(branch);

  Commit commit;
  commit.message = clean_commit_message(message);
  commit.author = author;
  commit.committer = author;
  commit.tree = write_tree_from_index(store, index);

  if (parent) {
    const RawObject obj = store.read(*parent);
    if (obj.type != ObjectType::Commit) {
      throw CommitError(branch + " points at a " + std::string(to_string(obj.type)) +
                        ", not a commit");
    }
    if (parse_commit(obj.content).tree == commit.tree) {
      throw CommitError("nothing to commit (the staged files match the last commit)");
    }
    commit.parents.push_back(*parent);
  } else if (index.empty()) {
    throw CommitError("nothing to commit (use \"minigit add\" to stage files)");
  }

  const std::string oid = store.write(ObjectType::Commit, serialize_commit(commit));

  // Refuse to overwrite a branch that moved since we read it (for example a
  // second minigit process committing at the same time). This narrows the
  // race rather than closing it: there is no lock file.
  if (repo.read_ref(branch) != parent) {
    throw CommitError(branch + " changed while committing; nothing was updated");
  }
  repo.write_ref(branch, oid);  // the only step that makes the commit visible
  return {oid, branch, !parent};
}

}  // namespace minigit
