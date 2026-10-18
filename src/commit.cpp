#include "minigit/commit.hpp"

#include <charconv>

#include "minigit/hash.hpp"
#include "minigit/object_store.hpp"

namespace minigit {
namespace {

bool is_valid_timezone(std::string_view tz) {
  if (tz.size() != 5 || (tz[0] != '+' && tz[0] != '-')) return false;
  for (std::size_t i = 1; i < tz.size(); ++i) {
    if (tz[i] < '0' || tz[i] > '9') return false;
  }
  return true;
}

void validate_signature(const Signature& sig) {
  // These characters would make the line ambiguous to parse.
  if (sig.name.find_first_of("<>\n") != std::string::npos ||
      sig.email.find_first_of("<>\n") != std::string::npos) {
    throw ObjectError("name or email contains '<', '>' or a newline");
  }
  if (!is_valid_timezone(sig.timezone)) {
    throw ObjectError("invalid timezone '" + sig.timezone + "' (expected +hhmm or -hhmm)");
  }
}

void require_oid(std::string_view oid, std::string_view what) {
  if (!is_valid_oid(oid)) {
    throw ObjectError("commit has invalid " + std::string(what) + " id '" + std::string(oid) + "'");
  }
}

}  // namespace

std::string format_signature(const Signature& sig) {
  validate_signature(sig);
  return sig.name + " <" + sig.email + "> " + std::to_string(sig.timestamp) + " " + sig.timezone;
}

Signature parse_signature(std::string_view text) {
  const auto lt = text.find('<');
  const auto gt = text.find('>', lt == std::string_view::npos ? 0 : lt);
  if (lt == std::string_view::npos || gt == std::string_view::npos) {
    throw ObjectError("malformed signature '" + std::string(text) + "'");
  }
  Signature sig;
  std::string_view name = text.substr(0, lt);
  while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
  sig.name = std::string(name);
  sig.email = std::string(text.substr(lt + 1, gt - lt - 1));

  // After '>' comes " <timestamp> <timezone>".
  std::string_view rest = text.substr(gt + 1);
  if (rest.empty() || rest.front() != ' ') throw ObjectError("signature has no timestamp");
  rest.remove_prefix(1);
  const auto space = rest.find(' ');
  if (space == std::string_view::npos) throw ObjectError("signature has no timezone");
  const std::string_view ts = rest.substr(0, space);
  const auto [end, ec] = std::from_chars(ts.data(), ts.data() + ts.size(), sig.timestamp);
  if (ts.empty() || ec != std::errc() || end != ts.data() + ts.size()) {
    throw ObjectError("signature has invalid timestamp '" + std::string(ts) + "'");
  }
  sig.timezone = std::string(rest.substr(space + 1));
  validate_signature(sig);
  return sig;
}

std::string serialize_commit(const Commit& commit) {
  require_oid(commit.tree, "tree");
  for (const auto& parent : commit.parents) require_oid(parent, "parent");

  std::string out = "tree " + commit.tree + "\n";
  for (const auto& parent : commit.parents) out += "parent " + parent + "\n";
  out += "author " + format_signature(commit.author) + "\n";
  out += "committer " + format_signature(commit.committer) + "\n";
  out += "\n";
  out += commit.message;
  return out;
}

Commit parse_commit(std::string_view content) {
  Commit commit;
  bool have_tree = false, have_author = false, have_committer = false;
  std::size_t pos = 0;
  while (true) {
    const auto eol = content.find('\n', pos);
    if (eol == std::string_view::npos) throw ObjectError("commit has no blank line before message");
    const std::string_view line = content.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.empty()) break;  // end of headers
    if (line.front() == ' ') continue;  // continuation of a multi-line header (e.g. gpgsig)

    const auto space = line.find(' ');
    const std::string_view key = line.substr(0, space);
    const std::string_view value =
        space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
    if (key == "tree") {
      if (have_tree) throw ObjectError("commit has more than one tree");
      require_oid(value, "tree");
      commit.tree = std::string(value);
      have_tree = true;
    } else if (key == "parent") {
      require_oid(value, "parent");
      commit.parents.emplace_back(value);
    } else if (key == "author") {
      commit.author = parse_signature(value);
      have_author = true;
    } else if (key == "committer") {
      commit.committer = parse_signature(value);
      have_committer = true;
    }
    // Other headers are ignored; see the note in commit.hpp.
  }
  if (!have_tree) throw ObjectError("commit has no tree");
  if (!have_author) throw ObjectError("commit has no author");
  if (!have_committer) throw ObjectError("commit has no committer");
  commit.message = std::string(content.substr(pos));
  return commit;
}

}  // namespace minigit
