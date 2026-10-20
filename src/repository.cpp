#include "minigit/repository.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "minigit/file_util.hpp"
#include "minigit/hash.hpp"

namespace minigit {
namespace {

void write_file(const fs::path& path, const std::string& contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) throw RepositoryError("cannot write " + path.string());
  file << contents;
  if (!file) throw RepositoryError("failed while writing " + path.string());
}

std::string read_repo_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw RepositoryError("cannot read " + path.string());
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

void strip_line_ending(std::string& text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
}

// See Repository::is_valid_ref_name. The rules are enough to keep ref files
// inside .git/refs.
bool check_ref_name(const std::string& ref) {
  if (ref.rfind("refs/", 0) != 0) return false;
  std::size_t start = 0;
  while (true) {
    const auto slash = ref.find('/', start);
    const std::string part = ref.substr(start, slash == std::string::npos ? slash : slash - start);
    if (part.empty() || part.front() == '.' ||
        (part.size() >= 5 && part.compare(part.size() - 5, 5, ".lock") == 0)) {
      return false;
    }
    for (const char c : part) {
      if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ' ' || c == '~' ||
          c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\') {
        return false;
      }
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return ref.find("..") == std::string::npos && ref.find("@{") == std::string::npos;
}

void validate_ref_name(const std::string& ref) {
  if (!check_ref_name(ref)) throw RepositoryError("invalid ref name '" + ref + "'");
}

}  // namespace

bool Repository::is_valid_ref_name(const std::string& ref) { return check_ref_name(ref); }

bool Repository::is_repository_root(const fs::path& dir) {
  std::error_code ec;
  const fs::path git = dir / ".git";
  return fs::is_regular_file(git / "HEAD", ec) && fs::is_directory(git / "objects", ec) &&
         fs::is_directory(git / "refs", ec);
}

Repository Repository::init(const fs::path& worktree, bool* reinitialized) {
  std::error_code ec;
  fs::create_directories(worktree, ec);
  if (ec) throw RepositoryError("cannot create " + worktree.string() + ": " + ec.message());

  Repository repo(fs::canonical(worktree));
  if (is_repository_root(repo.worktree())) {
    // Never rewrite an existing HEAD or config: that could lose user state.
    if (reinitialized) *reinitialized = true;
    return repo;
  }
  if (fs::exists(repo.git_dir())) {
    throw RepositoryError(repo.git_dir().string() + " exists but is not a valid repository");
  }

  for (const fs::path& dir : {repo.objects_dir(), repo.refs_dir() / "heads",
                              repo.refs_dir() / "tags", repo.minigit_dir()}) {
    fs::create_directories(dir, ec);
    if (ec) throw RepositoryError("cannot create " + dir.string() + ": " + ec.message());
  }
  write_file(repo.git_dir() / "HEAD", std::string("ref: refs/heads/") + kDefaultBranch + "\n");
  write_file(repo.git_dir() / "config",
             "[core]\n"
             "\trepositoryformatversion = 0\n"
             "\tfilemode = false\n"
             "\tbare = false\n");

  if (reinitialized) *reinitialized = false;
  return repo;
}

Repository Repository::discover(const fs::path& start) {
  std::error_code ec;
  fs::path dir = fs::weakly_canonical(fs::absolute(start), ec);
  if (ec) dir = fs::absolute(start);

  while (true) {
    if (is_repository_root(dir)) return Repository(dir);
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  throw RepositoryError("not a minigit repository (or any of the parent directories): " +
                        start.string());
}

std::string Repository::head_ref() const {
  std::string head = read_repo_file(git_dir() / "HEAD");
  strip_line_ending(head);

  const std::string prefix = "ref: ";
  if (head.rfind(prefix, 0) != 0 || head.size() == prefix.size()) {
    throw RepositoryError("HEAD is malformed or detached, which is not supported yet: '" +
                          head + "'");
  }
  return head.substr(prefix.size());
}

std::optional<std::string> Repository::read_ref(const std::string& ref) const {
  validate_ref_name(ref);
  const fs::path path = git_dir() / ref;
  std::error_code ec;
  if (!fs::exists(path, ec)) return std::nullopt;
  std::string oid = read_repo_file(path);
  strip_line_ending(oid);
  if (!is_valid_oid(oid)) throw RepositoryError("ref " + ref + " is malformed: '" + oid + "'");
  return oid;
}

void Repository::write_ref(const std::string& ref, const std::string& oid) const {
  validate_ref_name(ref);
  if (!is_valid_oid(oid)) throw RepositoryError("cannot point " + ref + " at invalid id '" + oid + "'");
  const fs::path path = git_dir() / ref;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) throw RepositoryError("cannot create " + path.parent_path().string() + ": " + ec.message());
  write_file_atomic(path, oid + "\n");
}

}  // namespace minigit
