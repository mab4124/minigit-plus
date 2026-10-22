#include "minigit/history_api.hpp"

#include <httplib.h>

#include <exception>
#include <nlohmann/json.hpp>

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

HistoryServer::HistoryServer(Repository repo) : impl_(std::make_unique<Impl>(std::move(repo))) {
  httplib::Server& server = impl_->server;
  server.set_socket_options(exclusive_port_socket_options);

  server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    send_json(res, 200, {{"status", "ok"}});
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

  // An exception escaping a handler (a corrupt object, an unreadable ref)
  // becomes a 500 with the message instead of a dropped connection.
  server.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
          std::rethrow_exception(ep);
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
