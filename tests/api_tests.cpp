#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <csignal>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>

#include "minigit/cli.hpp"
#include "minigit/create_commit.hpp"
#include "minigit/history_api.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "test_support.hpp"

namespace {

using minigit::HistoryServer;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::ServerError;
using minigit::Signature;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::quote;
using minigit::test::read_text;
using minigit::test::run_command;
using minigit::test::ScopedCwd;
using minigit::test::TempDir;
using minigit::test::write_text;
using nlohmann::json;
namespace fs = std::filesystem;

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
  ObjectStore store{repo.objects_dir()};
  Signature author{"Ada Lovelace", "ada@example.com", 1700000000, "+0530"};

  void SetUp() override { start(); }

  // Stages `files` (path -> content; an empty optional deletes) and commits.
  std::string commit(const std::map<std::string, std::optional<std::string>>& files,
                     const std::string& message) {
    auto index = minigit::StagingIndex::load(repo.index_path());
    for (const auto& [path, text] : files) {
      if (text) {
        index.stage(path, {"100644", store.write(ObjectType::Blob, *text)});
      } else {
        index.remove(path);
      }
    }
    index.save();
    author.timestamp += 60;
    return minigit::create_commit(repo, message, author).oid;
  }
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

TEST_F(ApiTest, LogOnUnbornBranchIsEmpty) {
  const ApiResponse r = get("/api/log");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body["branch"], "main");
  EXPECT_TRUE(r.body["head"].is_null());
  EXPECT_EQ(r.body["commits"], json::array());
}

TEST_F(ApiTest, LogListsFirstParentHistoryNewestFirst) {
  const std::string c1 = commit({{"a.txt", "1\n"}}, "first");
  const std::string c2 = commit({{"a.txt", "2\n"}}, "second\n\nwith a body");
  const std::string c3 = commit({{"b.txt", "b\n"}}, "third");

  ApiResponse r = get("/api/log");
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.body["head"], c3);
  ASSERT_EQ(r.body["commits"].size(), 3u);
  EXPECT_EQ(r.body["commits"][0]["id"], c3);
  EXPECT_EQ(r.body["commits"][1]["id"], c2);
  EXPECT_EQ(r.body["commits"][2]["id"], c1);
  EXPECT_EQ(r.body["commits"][1]["summary"], "second");
  EXPECT_EQ(r.body["commits"][1]["message"], "second\n\nwith a body\n");
  EXPECT_EQ(r.body["commits"][1]["parents"], json::array({c1}));
  EXPECT_EQ(r.body["commits"][2]["parents"], json::array());

  r = get("/api/log?limit=2");
  ASSERT_EQ(r.body["commits"].size(), 2u);
  EXPECT_EQ(r.body["commits"][1]["id"], c2);

  r = get("/api/log?limit=0");
  EXPECT_EQ(r.body["commits"], json::array());
}

TEST_F(ApiTest, LogSeesCommitsMadeWhileServing) {
  commit({{"a.txt", "1\n"}}, "first");
  EXPECT_EQ(get("/api/log").body["commits"].size(), 1u);
  const std::string c2 = commit({{"a.txt", "2\n"}}, "second");
  const ApiResponse r = get("/api/log");
  EXPECT_EQ(r.body["head"], c2);
  EXPECT_EQ(r.body["commits"].size(), 2u);
}

TEST_F(ApiTest, LogDefaultsToTwentyCommits) {
  for (std::size_t i = 0; i < minigit::kDefaultLimit + 2; ++i) {
    commit({{"n.txt", std::to_string(i)}}, "commit " + std::to_string(i));
  }
  EXPECT_EQ(get("/api/log").body["commits"].size(), minigit::kDefaultLimit);
}

TEST_F(ApiTest, LogRejectsInvalidLimits) {
  for (const std::string bad : {"-1", "abc", "", "5x", "1001", "99999999999999999999999"}) {
    const ApiResponse r = get("/api/log?limit=" + bad);
    EXPECT_EQ(r.status, 400) << bad;
    EXPECT_NE(r.body["error"].get<std::string>().find("limit must be a whole number"),
              std::string::npos)
        << bad;
  }
  EXPECT_EQ(get("/api/log?limit=1000").status, 200);
}

TEST_F(ApiTest, CommitReturnsOneCommit) {
  const std::string c1 = commit({{"a.txt", "1\n"}}, "first");
  const std::string c2 = commit({{"a.txt", "2\n"}}, "second");

  ApiResponse r = get("/api/commit/" + c2);
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.body["id"], c2);
  EXPECT_EQ(r.body["parents"], json::array({c1}));
  EXPECT_EQ(r.body["summary"], "second");
  EXPECT_EQ(r.body["author"]["name"], "Ada Lovelace");
  EXPECT_EQ(r.body["author"]["email"], "ada@example.com");
  EXPECT_EQ(r.body["author"]["timestamp"], author.timestamp);
  EXPECT_EQ(r.body["author"]["timezone"], "+0530");
  EXPECT_EQ(r.body["committer"], r.body["author"]);

  // Upper-case ids are accepted and answered with the canonical id.
  std::string upper = c1;
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  r = get("/api/commit/" + upper);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body["id"], c1);
}

TEST_F(ApiTest, CommitErrors) {
  const std::string c1 = commit({{"a.txt", "1\n"}}, "first");

  ApiResponse r = get("/api/commit/" + c1.substr(0, 7));
  EXPECT_EQ(r.status, 400);
  EXPECT_NE(r.body["error"].get<std::string>().find("not a full 40-character commit id"),
            std::string::npos);

  r = get("/api/commit/main");  // ref names are not accepted here
  EXPECT_EQ(r.status, 400);

  r = get("/api/commit/");
  EXPECT_EQ(r.status, 400);

  r = get("/api/commit/" + std::string(40, 'a'));
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body["error"], "commit " + std::string(40, 'a') + " not found");

  const std::string blob = store.write(ObjectType::Blob, "1\n");
  r = get("/api/commit/" + blob);
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body["error"], "object " + blob + " is a blob, not a commit");
}

TEST_F(ApiTest, CorruptObjectIsServerError) {
  const std::string c1 = commit({{"a.txt", "1\n"}}, "first");
  const fs::path file = store.object_path(c1);
  fs::permissions(file, fs::perms::owner_write, fs::perm_options::add);
  write_text(file, "not zlib data");

  ApiResponse r = get("/api/commit/" + c1);
  EXPECT_EQ(r.status, 500);
  EXPECT_NE(r.body["error"].get<std::string>().find("is corrupt"), std::string::npos);

  r = get("/api/log");
  EXPECT_EQ(r.status, 500);
}

TEST_F(ApiTest, FileHistoryListsCommitsThatChangedThePath) {
  const std::string c1 = commit({{"src/main.cpp", "1"}, {"README", "r"}}, "add main");
  commit({{"README", "r2"}}, "docs only");
  const std::string c3 = commit({{"src/main.cpp", "2"}}, "edit main");
  const std::string c4 = commit({{"src/main.cpp", std::nullopt}}, "drop main");

  ApiResponse r = get("/api/file-history?path=src/main.cpp");
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.body["path"], "src/main.cpp");
  EXPECT_EQ(r.body["branch"], "main");
  const json& commits = r.body["commits"];
  ASSERT_EQ(commits.size(), 3u);
  EXPECT_EQ(commits[0]["id"], c4);
  EXPECT_EQ(commits[0]["summary"], "drop main");
  EXPECT_EQ(commits[0]["change"]["status"], "deleted");
  EXPECT_EQ(commits[0]["change"]["old_blob"], store.write(ObjectType::Blob, "2"));
  EXPECT_TRUE(commits[0]["change"]["new_blob"].is_null());
  EXPECT_TRUE(commits[0]["change"]["new_mode"].is_null());
  EXPECT_EQ(commits[1]["id"], c3);
  EXPECT_EQ(commits[1]["change"]["status"], "modified");
  EXPECT_EQ(commits[2]["id"], c1);
  EXPECT_EQ(commits[2]["change"],
            json({{"path", "src/main.cpp"},
                  {"status", "added"},
                  {"old_mode", nullptr},
                  {"new_mode", "100644"},
                  {"old_blob", nullptr},
                  {"new_blob", store.write(ObjectType::Blob, "1")}}));

  r = get("/api/file-history?path=src/main.cpp&limit=1");
  ASSERT_EQ(r.body["commits"].size(), 1u);
  EXPECT_EQ(r.body["commits"][0]["id"], c4);

  // Query strings are URL-decoded, so names with spaces work.
  const std::string c5 = commit({{"my file.txt", "x"}}, "spaces");
  r = get("/api/file-history?path=my%20file.txt");
  ASSERT_EQ(r.body["commits"].size(), 1u);
  EXPECT_EQ(r.body["commits"][0]["id"], c5);

  r = get("/api/file-history?path=never/there.txt");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body["commits"], json::array());
}

TEST_F(ApiTest, FileHistoryOnUnbornBranchIsEmpty) {
  const ApiResponse r = get("/api/file-history?path=a.txt");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body["commits"], json::array());
}

TEST_F(ApiTest, FileHistoryErrors) {
  commit({{"a.txt", "1"}}, "first");

  ApiResponse r = get("/api/file-history");
  EXPECT_EQ(r.status, 400);
  EXPECT_EQ(r.body["error"], "missing required parameter 'path'");

  r = get("/api/file-history?path=");
  EXPECT_EQ(r.status, 400);

  for (const std::string bad : {"../etc/passwd", "/abs", "a//b", "./a.txt", ".git/HEAD"}) {
    r = get("/api/file-history?path=" + bad);
    EXPECT_EQ(r.status, 400) << bad;
    EXPECT_NE(r.body["error"].get<std::string>().find("invalid path"), std::string::npos) << bad;
  }

  r = get("/api/file-history?path=a.txt&limit=-5");
  EXPECT_EQ(r.status, 400);
}

TEST_F(ApiTest, ChangedFilesBetweenCommits) {
  const std::string c1 = commit({{"a.txt", "a"}, {"dir/b.txt", "b"}, {"gone", "g"}}, "one");
  const std::string c2 =
      commit({{"a.txt", "A"}, {"gone", std::nullopt}, {"dir/new.txt", "n"}}, "two");

  ApiResponse r = get("/api/changed-files?from=" + c1 + "&to=" + c2);
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.body["from"], c1);
  EXPECT_EQ(r.body["to"], c2);
  const json& changes = r.body["changes"];
  ASSERT_EQ(changes.size(), 3u);
  EXPECT_EQ(changes[0]["path"], "a.txt");
  EXPECT_EQ(changes[0]["status"], "modified");
  EXPECT_EQ(changes[0]["old_blob"], store.write(ObjectType::Blob, "a"));
  EXPECT_EQ(changes[0]["new_blob"], store.write(ObjectType::Blob, "A"));
  EXPECT_EQ(changes[1]["path"], "dir/new.txt");
  EXPECT_EQ(changes[1]["status"], "added");
  EXPECT_EQ(changes[2]["path"], "gone");
  EXPECT_EQ(changes[2]["status"], "deleted");

  // Branch names and HEAD resolve too; a commit against itself is empty.
  r = get("/api/changed-files?from=" + c1 + "&to=main");
  EXPECT_EQ(r.body["to"], c2);
  EXPECT_EQ(r.body["changes"].size(), 3u);
  r = get("/api/changed-files?from=HEAD&to=" + c2);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body["changes"], json::array());

  // Reversed, additions and deletions swap.
  r = get("/api/changed-files?from=" + c2 + "&to=" + c1);
  EXPECT_EQ(r.body["changes"][1]["status"], "deleted");
  EXPECT_EQ(r.body["changes"][2]["status"], "added");
}

TEST_F(ApiTest, ChangedFilesErrors) {
  const std::string c1 = commit({{"a.txt", "a"}}, "one");

  ApiResponse r = get("/api/changed-files?to=" + c1);
  EXPECT_EQ(r.status, 400);
  EXPECT_EQ(r.body["error"], "missing required parameter 'from'");

  r = get("/api/changed-files?from=" + c1);
  EXPECT_EQ(r.status, 400);
  EXPECT_EQ(r.body["error"], "missing required parameter 'to'");

  r = get("/api/changed-files?from=" + c1 + "&to=no-such-branch");
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body["error"], "unknown revision 'no-such-branch'");

  r = get("/api/changed-files?from=" + std::string(40, 'b') + "&to=" + c1);
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body["error"], "commit " + std::string(40, 'b') + " not found");

  const std::string blob = store.write(ObjectType::Blob, "a");
  r = get("/api/changed-files?from=" + c1 + "&to=" + blob);
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body["error"], "'" + blob + "' is a blob, not a commit");
}

// Real Git must agree with the API about the history and its metadata.
TEST_F(ApiTest, LogMatchesGitOracle) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  commit({{"a.txt", "1\n"}}, "first");
  commit({{"dir/b.txt", "b\n"}}, "second");
  commit({{"a.txt", std::nullopt}}, "third");

  const auto git = git_in(tmp.path(), "log --format=%H%x09%an%x09%ae%x09%at%x09%ad%x09%s");
  ASSERT_EQ(git.status, 0);
  const ApiResponse r = get("/api/log");
  std::string expected;
  for (const auto& c : r.body["commits"]) {
    if (!expected.empty()) expected += '\n';
    expected += c["id"].get<std::string>() + '\t' + c["author"]["name"].get<std::string>() +
                '\t' + c["author"]["email"].get<std::string>() + '\t' +
                std::to_string(c["author"]["timestamp"].get<std::int64_t>()) + '\t' +
                c["author"]["date"].get<std::string>() + '\t' + c["summary"].get<std::string>();
  }
  EXPECT_EQ(git.output, expected);
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
