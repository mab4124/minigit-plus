#include "minigit/repository.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace minigit {
namespace {

void write_file(const fs::path& path, const std::string& contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) throw RepositoryError("cannot write " + path.string());
  file << contents;
  if (!file) throw RepositoryError("failed while writing " + path.string());
}

std::string read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw RepositoryError("cannot read " + path.string());
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

}  // namespace

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
  std::string head = read_file(git_dir() / "HEAD");
  while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) head.pop_back();

  const std::string prefix = "ref: ";
  if (head.rfind(prefix, 0) != 0 || head.size() == prefix.size()) {
    throw RepositoryError("HEAD is malformed or detached, which is not supported yet: '" +
                          head + "'");
  }
  return head.substr(prefix.size());
}

}  // namespace minigit
