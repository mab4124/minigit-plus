#include "minigit/file_util.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace minigit {
namespace {

// Deletes the temporary file on scope exit unless release() was called.
class TempFileGuard {
 public:
  explicit TempFileGuard(fs::path path) : path_(std::move(path)) {}
  ~TempFileGuard() {
    if (active_) {
      std::error_code ec;
      fs::remove(path_, ec);
    }
  }
  TempFileGuard(const TempFileGuard&) = delete;
  TempFileGuard& operator=(const TempFileGuard&) = delete;

  void release() { active_ = false; }

 private:
  fs::path path_;
  bool active_ = true;
};

std::string random_suffix() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  return std::to_string(rng());
}

}  // namespace

std::string read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw FileError("cannot read " + path.string());
  std::ostringstream ss;
  ss << file.rdbuf();
  if (file.bad()) throw FileError("failed while reading " + path.string());
  return ss.str();
}

void write_file_atomic(const fs::path& path, std::string_view data) {
  const fs::path temp = path.parent_path() / (path.filename().string() + ".tmp-" + random_suffix());
  TempFileGuard guard(temp);
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) throw FileError("cannot write " + temp.string());
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    if (!file) throw FileError("failed while writing " + temp.string());
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) throw FileError("cannot rename " + temp.string() + " to " + path.string() + ": " +
                          ec.message());
  guard.release();
}

}  // namespace minigit
