#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace minigit::test {

// A unique temporary directory that is removed when the object goes out of scope.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("minigit-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
    path_ = std::filesystem::canonical(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

inline void write_text(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

inline std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// True if a real `git` executable is available to use as a test oracle.
inline bool git_available() {
#ifdef _WIN32
  return std::system("git --version > NUL 2>&1") == 0;
#else
  return std::system("git --version > /dev/null 2>&1") == 0;
#endif
}

// Wraps a path in double quotes for use in a shell command line.
inline std::string quote(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }

struct CommandResult {
  int status;          // 0 on success
  std::string output;  // captured stdout
};

// Runs a shell command (used only to drive the real `git` oracle) and
// captures its standard output.
inline CommandResult run_command(const std::string& command) {
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "rb");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (pipe == nullptr) return {-1, {}};
  std::string output;
  char buffer[4096];
  std::size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
#ifdef _WIN32
  const int status = _pclose(pipe);
#else
  const int status = pclose(pipe);
#endif
  return {status, output};
}

// Runs `git -C <dir> <args>` and returns its stdout without trailing newlines.
// Callers should check `status` before trusting the output.
inline CommandResult git_in(const std::filesystem::path& dir, const std::string& args) {
  CommandResult r = run_command("git -C " + quote(dir) + " " + args);
  while (!r.output.empty() && (r.output.back() == '\n' || r.output.back() == '\r')) {
    r.output.pop_back();
  }
  return r;
}

// Changes the working directory for the lifetime of the object. Safe in tests
// because ctest runs each test case in its own process.
class ScopedCwd {
 public:
  explicit ScopedCwd(const std::filesystem::path& dir) : old_(std::filesystem::current_path()) {
    std::filesystem::current_path(dir);
  }
  ~ScopedCwd() {
    std::error_code ec;
    std::filesystem::current_path(old_, ec);
  }
  ScopedCwd(const ScopedCwd&) = delete;
  ScopedCwd& operator=(const ScopedCwd&) = delete;

 private:
  std::filesystem::path old_;
};

// Sets an environment variable for the lifetime of the object (inherited by
// git processes started with run_command), restoring the old value afterwards.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
    if (const char* old = std::getenv(name_.c_str())) old_ = old;
    set(value.c_str());
  }
  ~ScopedEnv() { set(old_ ? old_->c_str() : nullptr); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  void set(const char* value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value ? value : "");
#else
    if (value) {
      setenv(name_.c_str(), value, 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }

  std::string name_;
  std::optional<std::string> old_;
};

}  // namespace minigit::test
