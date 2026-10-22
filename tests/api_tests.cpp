#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <csignal>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "minigit/cli.hpp"
#include "minigit/history_api.hpp"
#include "minigit/repository.hpp"
#include "test_support.hpp"

namespace {

using minigit::HistoryServer;
using minigit::Repository;
using minigit::ServerError;
using minigit::test::quote;
using minigit::test::read_text;
using minigit::test::run_command;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using nlohmann::json;

struct CliResult {
  int code;
  std::string out;
  std::string err;
};

CliResult run(const std::vector<std::string>& args) {
  std::ostringstream out, err;
  const int code = minigit::run_cli(args, out, err);
  return {code, out.str(), err.str()};
}

// A status code and parsed JSON body.
struct ApiResponse {
  int status = 0;
  std::string content_type;
  json body;
};

ApiResponse get_json(httplib::Client& client, const std::string& target) {
  const auto res = client.Get(target);
  if (!res) {
    ADD_FAILURE() << "request failed: " << httplib::to_string(res.error());
    return {};
  }
  ApiResponse out{res->status, res->get_header_value("Content-Type"), json()};
  out.body = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
  EXPECT_FALSE(out.body.is_discarded()) << "not JSON: " << res->body;
  return out;
}

// Runs a HistoryServer on a free loopback port in a background thread for
// the duration of each test, with a client connected to it.
class ApiTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());

  void SetUp() override { start(); }
  void TearDown() override { stop(); }

  void start() {
    server_ = std::make_unique<HistoryServer>(repo);
    port_ = server_->bind(minigit::kServeHost, 0);
    thread_ = std::thread([this] { server_->listen(); });
    server_->wait_until_ready();
    client_ = std::make_unique<httplib::Client>(minigit::kServeHost, port_);
  }

  void stop() {
    if (!server_) return;
    server_->stop();
    thread_.join();
    server_.reset();
  }

  ApiResponse get(const std::string& target) { return get_json(*client_, target); }

  int port_ = 0;
  std::unique_ptr<HistoryServer> server_;
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

TEST_F(ApiTest, HealthReportsOk) {
  const ApiResponse r = get("/health");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.content_type, "application/json");
  EXPECT_EQ(r.body, json({{"status", "ok"}}));
}

TEST_F(ApiTest, UnknownPathIsJson404) {
  const ApiResponse r = get("/api/nope");
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body["error"], "no such endpoint: GET /api/nope");
}

TEST_F(ApiTest, ServerIsReadOnly) {
  const auto res = client_->Post("/health", "{}", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);  // no write routes exist at all
}

TEST_F(ApiTest, SecondServerOnSamePortFails) {
  HistoryServer other(repo);
  EXPECT_THROW(other.bind(minigit::kServeHost, port_), ServerError);
}

TEST(HistoryServerTest, ListenWithoutBindThrows) {
  TempDir tmp;
  HistoryServer server(Repository::init(tmp.path()));
  EXPECT_THROW(server.listen(), ServerError);
}

TEST(ServeCommand, RejectsBadArguments) {
  TempDir tmp;
  Repository::init(tmp.path());
  ScopedCwd cwd(tmp.path());

  CliResult r = run({"serve", "--port"});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("usage: minigit serve [--port <port>]"), std::string::npos);

  r = run({"serve", "--port", "70000"});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("'70000' is not a valid port"), std::string::npos);

  r = run({"serve", "--port=abc"});
  EXPECT_EQ(r.code, minigit::kExitUsage);

  r = run({"serve", "--host", "0.0.0.0"});
  EXPECT_EQ(r.code, minigit::kExitUsage);
  EXPECT_NE(r.err.find("unexpected argument '--host'"), std::string::npos);
}

TEST(ServeCommand, FailsOutsideRepository) {
  TempDir tmp;
  ScopedCwd cwd(tmp.path());
  const CliResult r = run({"serve", "--port", "0"});
  EXPECT_EQ(r.code, minigit::kExitError);
  EXPECT_NE(r.err.find("fatal:"), std::string::npos);
}

#ifndef _WIN32
// Starts the real executable, reads the port it prints and talks to it, so
// the whole `minigit serve` path (argument parsing, bind, listen) is covered.
TEST(ServeCommand, RealExecutableServesOnLoopback) {
  TempDir tmp;
  Repository::init(tmp.path());
  const auto log = tmp.path() / "serve.out";
  const auto started = run_command("cd " + quote(tmp.path()) + " && " + quote(MINIGIT_EXE) +
                                   " serve --port 0 > " + quote(log) + " 2>&1 & echo $!");
  ASSERT_EQ(started.status, 0);
  const pid_t pid = std::stoi(started.output);

  // The first line is "Serving the history of <dir> at http://127.0.0.1:<port>/".
  int port = 0;
  for (int i = 0; i < 200 && port == 0; ++i) {
    const std::string text = read_text(log);
    const auto at = text.find("http://127.0.0.1:");
    if (at != std::string::npos && text.find('/', at + 17) != std::string::npos) {
      port = std::stoi(text.substr(at + 17));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  if (port != 0) {
    httplib::Client client(minigit::kServeHost, port);
    const ApiResponse r = get_json(client, "/health");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["status"], "ok");
  }
  kill(pid, SIGTERM);
  ASSERT_NE(port, 0) << "server did not start; output:\n" << read_text(log);
}
#endif

}  // namespace
