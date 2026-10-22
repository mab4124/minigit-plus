#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "minigit/repository.hpp"

namespace minigit {

// Raised when the history server cannot bind its socket or fails to serve.
class ServerError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// `minigit serve` listens here unless told otherwise. Only the loopback
// address is used: the API has no authentication.
inline constexpr const char* kServeHost = "127.0.0.1";
inline constexpr int kDefaultServePort = 8080;

// The `limit` query parameter's default and largest accepted value.
inline constexpr std::size_t kDefaultLimit = 20;
inline constexpr std::size_t kMaxLimit = 1000;

// A read-only HTTP/JSON view of a repository's history (milestone 9).
//
// Every request reads the repository's objects and refs afresh, so commits
// made while the server runs are visible to the next request. Nothing is
// ever written. Responses are JSON; failures are {"error": "<message>"}
// with a 4xx/5xx status.
//
// Endpoints:
//   GET /health                {"status": "ok"}
//   GET /api/log?limit=N       {"branch", "head", "commits": [commit...]}: the
//                              current branch's first-parent history, newest
//                              first (default 20, at most kMaxLimit); an empty
//                              list on a branch with no commits
//   GET /api/commit/<id>       one commit; 400 unless <id> is a full id, 404
//                              if it is missing or not a commit
//
// A commit is {"id", "tree", "parents", "author", "committer", "summary",
// "message"}; author and committer are {"name", "email", "timestamp",
// "timezone", "date"} with `date` in `git log`'s format.
//
// The HTTP library is hidden behind a pointer so that only history_api.cpp
// has to compile it.
class HistoryServer {
 public:
  explicit HistoryServer(Repository repo);
  ~HistoryServer();
  HistoryServer(const HistoryServer&) = delete;
  HistoryServer& operator=(const HistoryServer&) = delete;

  // Binds to `host`:`port` and returns the bound port (port 0 picks a free
  // one). Throws ServerError if the address is in use or invalid.
  int bind(const std::string& host, int port);

  // Serves requests on the bound socket until stop() is called from another
  // thread. Throws ServerError if bind() has not succeeded.
  void listen();

  // Blocks until listen() is accepting connections.
  void wait_until_ready() const;

  // Makes listen() return. Safe to call from any thread.
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace minigit
