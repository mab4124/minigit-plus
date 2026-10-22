#include "minigit/history_api.hpp"

#include <httplib.h>

#include <cctype>
#include <charconv>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>

#include "minigit/hash.hpp"
#include "minigit/log.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"

namespace minigit {
namespace {

using nlohmann::json;

void send_json(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(2) + "\n", "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& message) {
  send_json(res, status, {{"error", message}});
}

// Thrown by handlers to answer with a 4xx status and {"error": message}.
struct ClientError {
  int status;
  std::string message;
};

// The `limit` query parameter: a whole number from 0 to kMaxLimit, or
// `fallback` when absent. Anything else is a 400, so a typo is not silently
// treated as "no limit".
std::size_t limit_param(const httplib::Request& req, std::size_t fallback) {
  if (!req.has_param("limit")) return fallback;
  const std::string text = req.get_param_value("limit");
  std::size_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc() || end != text.data() + text.size() ||
      value > kMaxLimit) {
    throw ClientError{400, "limit must be a whole number from 0 to " +
                               std::to_string(kMaxLimit) + ", got '" + text + "'"};
  }
  return value;
}

json signature_json(const Signature& sig) {
  return {{"name", sig.name},
          {"email", sig.email},
          {"timestamp", sig.timestamp},
          {"timezone", sig.timezone},
          {"date", format_git_date(sig.timestamp, sig.timezone)}};
}

json commit_json(const std::string& oid, const Commit& commit) {
  const std::string_view message = commit.message;
  return {{"id", oid},
          {"tree", commit.tree},
          {"parents", commit.parents},
          {"author", signature_json(commit.author)},
          {"committer", signature_json(commit.committer)},
          {"summary", std::string(message.substr(0, message.find('\n')))},
          {"message", commit.message}};
}

// cpp-httplib's default sets SO_REUSEPORT on Linux, which lets a second
// server bind the same port and silently receive half of the requests.
// SO_REUSEADDR alone still allows a quick restart after Ctrl+C; on Windows,
// where SO_REUSEADDR would allow port hijacking, exclusive use is requested.
void exclusive_port_socket_options(socket_t sock) {
  int on = 1;
#ifdef _WIN32
  setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on),
             sizeof(on));
#else
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
}

}  // namespace

struct HistoryServer::Impl {
  explicit Impl(Repository r) : repo(std::move(r)) {}

  Repository repo;
  httplib::Server server;
  bool bound = false;
};

HistoryServer::HistoryServer(Repository repository)
    : impl_(std::make_unique<Impl>(std::move(repository))) {
  httplib::Server& server = impl_->server;
  server.set_socket_options(exclusive_port_socket_options);

  const Repository* repo = &impl_->repo;

  server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, {{"status", "ok"}});
  });

  // The current branch's first-parent history, newest first.
  server.Get("/api/log", [repo](const httplib::Request& req, httplib::Response& res) {
    const std::size_t limit = limit_param(req, kDefaultLimit);
    const HeadState head = resolve_head(*repo);
    json commits = json::array();
    if (head.commit) {
      for (const auto& [oid, commit] :
           first_parent_log(ObjectStore(repo->objects_dir()), *head.commit, limit)) {
        commits.push_back(commit_json(oid, commit));
      }
    }
    send_json(res, 200,
              {{"branch", short_branch_name(head.branch_ref)},
               {"head", head.commit ? json(*head.commit) : json(nullptr)},
               {"commits", std::move(commits)}});
  });

  // One commit by its full id (upper case is accepted).
  server.Get(R"(/api/commit/([^/]*))", [repo](const httplib::Request& req,
                                               httplib::Response& res) {
    std::string oid = req.matches[1];
    for (char& c : oid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!is_valid_oid(oid)) {
      throw ClientError{400, "'" + std::string(req.matches[1]) +
                                 "' is not a full 40-character commit id"};
    }
    const ObjectStore store(repo->objects_dir());
    if (!store.contains(oid)) throw ClientError{404, "commit " + oid + " not found"};
    const RawObject obj = store.read(oid);
    if (obj.type != ObjectType::Commit) {
      throw ClientError{404, "object " + oid + " is a " + std::string(to_string(obj.type)) +
                                 ", not a commit"};
    }
    send_json(res, 200, commit_json(oid, parse_commit(obj.content)));
  });

  // Unknown paths and unsupported methods get a JSON body too. Handlers that
  // already produced an error body are left alone.
  server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    if (!res.body.empty()) return;
    if (res.status == 404) {
      send_error(res, 404, "no such endpoint: " + req.method + " " + req.path);
    } else {
      send_error(res, res.status, httplib::status_message(res.status));
    }
  });

  // A ClientError from a handler becomes its 4xx response. Any other
  // exception (a corrupt object, an unreadable ref) becomes a 500 with the
  // message instead of a dropped connection.
  server.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
          std::rethrow_exception(ep);
        } catch (const ClientError& e) {
          send_error(res, e.status, e.message);
        } catch (const std::exception& e) {
          send_error(res, 500, e.what());
        } catch (...) {
          send_error(res, 500, "unknown error");
        }
      });
}

HistoryServer::~HistoryServer() = default;

int HistoryServer::bind(const std::string& host, int port) {
  int bound_port = -1;
  if (port == 0) {
    bound_port = impl_->server.bind_to_any_port(host);
  } else if (impl_->server.bind_to_port(host, port)) {
    bound_port = port;
  }
  if (bound_port <= 0) {
    throw ServerError("cannot listen on " + host + ":" + std::to_string(port) +
                      " (address in use or not available)");
  }
  impl_->bound = true;
  return bound_port;
}

void HistoryServer::listen() {
  if (!impl_->bound) throw ServerError("the server is not bound to a port");
  if (!impl_->server.listen_after_bind()) throw ServerError("the server stopped unexpectedly");
}

void HistoryServer::wait_until_ready() const { impl_->server.wait_until_ready(); }

void HistoryServer::stop() { impl_->server.stop(); }

}  // namespace minigit
