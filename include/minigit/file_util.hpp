#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minigit {

namespace fs = std::filesystem;

// Raised when a file cannot be read or written.
class FileError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Reads a whole file in binary mode.
std::string read_file(const fs::path& path);

// Writes `data` to a temporary file next to `path`, then renames it over
// `path`. Readers therefore see either the old or the new contents, never a
// half-written file. The temporary file is removed if anything fails.
void write_file_atomic(const fs::path& path, std::string_view data);

}  // namespace minigit
