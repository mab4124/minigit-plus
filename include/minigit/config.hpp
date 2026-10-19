#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace minigit {

namespace fs = std::filesystem;

// Raised for malformed config files and invalid keys or values.
class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A small reader/writer for the repository's .git/config, in the same INI-like
// syntax Git uses, so `git config` and MiniGit++ can read each other's values:
//
//   [user]
//   	name = Ada Lovelace
//   	email = "ada@example.com"
//
// Supported: "section.key" names (both compared case-insensitively), "#" and
// ";" comments, double-quoted values and the escapes \\ \" \n \t \b. The last
// occurrence of a key wins, as in Git. Not supported: subsections
// ([branch "main"] is kept on save but never matched), include directives,
// line continuations, and the global/system config files.
//
// The file is kept as a list of lines so that saving preserves everything
// MiniGit++ did not change, including comments and unknown sections.
class Config {
 public:
  // Loads `file`, or returns an empty config if it does not exist.
  static Config load(const fs::path& file);

  // The value of `key` ("section.name"), or nullopt if it is not set.
  // Throws ConfigError for an invalid key or a malformed line.
  std::optional<std::string> get(std::string_view key) const;

  // Sets `key`, replacing its last occurrence or adding it to the end of the
  // section (creating the section if needed). Throws ConfigError for invalid
  // keys and for values containing a NUL byte.
  void set(std::string_view key, std::string_view value);

  // Writes the file atomically (temp file + rename).
  void save() const;

  // Throws ConfigError unless `key` is "<section>.<name>", where the section
  // is letters, digits and '-', and the name is a letter followed by letters,
  // digits and '-'.
  static void validate_key(std::string_view key);

 private:
  explicit Config(fs::path file) : file_(std::move(file)) {}

  fs::path file_;
  std::vector<std::string> lines_;  // without trailing '\n'
};

}  // namespace minigit
