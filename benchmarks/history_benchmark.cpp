// Indexed versus unindexed file-history queries (milestone 10).
//
// Generates a repository with the MiniGit++ library itself (no `git`
// involved), then measures:
//
//   * repository size and commit count
//   * initial indexing time and index size
//   * file-history latency by traversal and through the index, for a file
//     changed often, one changed rarely, and one that never existed
//   * the same after new commits are added (before and after an incremental
//     `index` run)
//   * a delete-and-rebuild of the index, checked against traversal
//
// Results are printed as Markdown. Every query is repeated and the median is
// reported. Timings depend on the machine and its file cache; the generated
// repository is deterministic for given options.
//
// usage: history_benchmark [--commits N] [--files N] [--repeats N] [--keep DIR]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "minigit/create_commit.hpp"
#include "minigit/history.hpp"
#include "minigit/history_index.hpp"
#include "minigit/object_store.hpp"
#include "minigit/refs.hpp"
#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
  int commits = 2000;
  int files = 200;
  int repeats = 5;
  std::optional<fs::path> keep;  // generate here and do not delete
};

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Median wall time of `repeats` runs of `fn`, in milliseconds.
double median_ms(int repeats, const std::function<void()>& fn) {
  std::vector<double> times;
  for (int i = 0; i < repeats; ++i) {
    const auto start = Clock::now();
    fn();
    times.push_back(ms_since(start));
  }
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}

std::uintmax_t tree_size(const fs::path& dir, std::size_t* files = nullptr) {
  std::uintmax_t total = 0;
  std::size_t count = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.is_regular_file()) {
      total += e.file_size();
      ++count;
    }
  }
  if (files) *files = count;
  return total;
}

std::string kib(std::uintmax_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
  return buf;
}

std::string fmt_ms(double ms) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), ms < 10 ? "%.3f ms" : "%.1f ms", ms);
  return buf;
}

// Paths dNN/fileMMM.txt. The first 5% are "hot" and receive most edits; the
// rest are touched rarely, which is the common shape of real repositories.
class Generator {
 public:
  Generator(minigit::Repository repo, int files)
      : repo_(std::move(repo)), store_(repo_.objects_dir()), rng_(20261023) {
    for (int i = 0; i < files; ++i) {
      char name[64];
      std::snprintf(name, sizeof(name), "d%02d/file%03d.txt", i % 20, i);
      paths_.emplace_back(name);
    }
    hot_ = std::max<std::size_t>(1, paths_.size() / 20);
  }

  const std::vector<std::string>& paths() const { return paths_; }
  const std::string& hot_path() const { return paths_.front(); }
  // Added by the first commit and never changed again.
  const std::string& cold_path() const { return paths_.back(); }

  void commit_all_files() {
    auto index = minigit::StagingIndex::load(repo_.index_path());
    for (const auto& path : paths_) stage(index, path);
    finish(index);
  }

  void commit_random_edit() {
    auto index = minigit::StagingIndex::load(repo_.index_path());
    const int n = 1 + static_cast<int>(rng_() % 3);
    for (int k = 0; k < n; ++k) {
      // 80% of edits go to hot files; the cold path is never edited.
      const std::size_t i = rng_() % 5 != 0 ? rng_() % hot_
                                            : hot_ + rng_() % (paths_.size() - hot_ - 1);
      stage(index, paths_[i]);
    }
    finish(index);
  }

 private:
  void stage(minigit::StagingIndex& index, const std::string& path) {
    const std::string content = path + " revision " + std::to_string(++counter_) + "\n";
    index.stage(path, {"100644", store_.write(minigit::ObjectType::Blob, content)});
  }

  void finish(minigit::StagingIndex& index) {
    index.save();
    author_.timestamp += 60;
    minigit::create_commit(repo_, "commit " + std::to_string(counter_), author_);
  }

  minigit::Repository repo_;
  minigit::ObjectStore store_;
  std::mt19937 rng_;
  std::vector<std::string> paths_;
  std::size_t hot_ = 1;
  int counter_ = 0;
  minigit::Signature author_{"Bench Mark", "bench@example.com", 1700000000, "+0000"};
};

struct QueryCase {
  std::string label;
  std::string path;
  std::size_t limit;
};

bool same(const std::vector<minigit::FileHistoryEntry>& a,
          const std::vector<minigit::FileHistoryEntry>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].oid != b[i].oid || !(a[i].change == b[i].change) || !(a[i].commit == b[i].commit)) {
      return false;
    }
  }
  return true;
}

class Bench {
 public:
  Bench(const minigit::Repository& repo, const Generator& gen, int repeats)
      : repo_(repo), store_(repo.objects_dir()), db_(minigit::HistoryIndex::default_path(repo)),
        repeats_(repeats) {
    cases_ = {{"hot file, limit 20", gen.hot_path(), 20},
              {"hot file, limit 1000", gen.hot_path(), 1000},
              {"cold file, limit 20", gen.cold_path(), 20},
              {"missing file, limit 20", "no/such/file.txt", 20}};
  }

  // One table: traversal vs file_history_with_index (which opens the
  // database on every call, as the server does).
  bool print_queries(const std::string& title) {
    const std::string head = minigit::resolve_head(repo_).commit.value();
    std::cout << "\n#### " << title << "\n\n"
              << "| Query | Results | Traversal | With index | Source | Speedup |\n"
              << "|---|---:|---:|---:|---|---:|\n";
    bool all_same = true;
    for (const auto& q : cases_) {
      std::vector<minigit::FileHistoryEntry> traversed, indexed;
      bool used = false;
      const double t = median_ms(repeats_, [&] {
        traversed = minigit::file_history(store_, head, q.path, q.limit);
      });
      const double i = median_ms(repeats_, [&] {
        indexed = minigit::file_history_with_index(db_, store_, head, q.path, q.limit, &used);
      });
      all_same = all_same && same(traversed, indexed);
      char speedup[32];
      std::snprintf(speedup, sizeof(speedup), "%.1f×", t / i);
      std::cout << "| " << q.label << " | " << traversed.size() << " | " << fmt_ms(t) << " | "
                << fmt_ms(i) << " | " << (used ? "index" : "traversal") << " | " << speedup
                << " |\n";
    }
    std::cout << "\nResults identical to traversal: " << (all_same ? "yes" : "**NO**") << "\n";
    return all_same;
  }

  // Every path's full history from the index equals traversal's.
  bool verify_all(const std::vector<std::string>& paths) {
    const std::string head = minigit::resolve_head(repo_).commit.value();
    const auto index = minigit::HistoryIndex::open_existing(db_);
    if (!index) return false;
    for (const auto& path : paths) {
      const auto indexed = index->file_history(store_, head, path, 1000000);
      if (!indexed || !same(*indexed, minigit::file_history(store_, head, path, 1000000))) {
        std::cout << "mismatch for " << path << "\n";
        return false;
      }
    }
    return true;
  }

  const fs::path& db() const { return db_; }

 private:
  const minigit::Repository& repo_;
  minigit::ObjectStore store_;
  fs::path db_;
  int repeats_;
  std::vector<QueryCase> cases_;
};

std::optional<int> parse_int(const std::string& text) {
  int value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size() || value <= 0) return std::nullopt;
  return value;
}

int run(const Options& opt) {
  fs::path dir;
  if (opt.keep) {
    dir = *opt.keep;
    if (fs::exists(dir)) {
      std::cerr << "history_benchmark: " << dir << " already exists\n";
      return 2;
    }
  } else {
    dir = fs::temp_directory_path() /
          ("minigit-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
  }
  struct Cleanup {
    fs::path dir;
    bool keep;
    ~Cleanup() {
      std::error_code ec;
      if (!keep) fs::remove_all(dir, ec);
    }
  } cleanup{dir, opt.keep.has_value()};

  const minigit::Repository repo = minigit::Repository::init(dir);
  Generator gen(repo, opt.files);
  const int added_later = std::max(1, opt.commits / 10);

  auto start = Clock::now();
  gen.commit_all_files();
  for (int i = 1; i < opt.commits; ++i) gen.commit_random_edit();
  const double generate_ms = ms_since(start);

  std::size_t object_files = 0;
  const auto objects_bytes = tree_size(repo.objects_dir(), &object_files);
  std::cout << "### " << opt.commits << " commits, " << opt.files << " files\n\n"
            << "| Measure | Value |\n|---|---:|\n"
            << "| Commits | " << opt.commits << " |\n"
            << "| Files in the tree | " << opt.files << " |\n"
            << "| Loose objects | " << object_files << " |\n"
            << "| Object store size | " << kib(objects_bytes) << " |\n"
            << "| Generation time | " << fmt_ms(generate_ms) << " |\n";

  Bench bench(repo, gen, opt.repeats);
  start = Clock::now();
  auto initial = minigit::HistoryIndex::open_for_update(bench.db()).update(repo);
  const double index_ms = ms_since(start);
  std::cout << "| Initial `index` | " << fmt_ms(index_ms) << " (" << initial.changes_added
            << " changed-path rows) |\n"
            << "| Index size | " << kib(fs::file_size(bench.db())) << " (" << std::fixed;
  std::cout.precision(1);
  std::cout << 100.0 * static_cast<double>(fs::file_size(bench.db())) /
                   static_cast<double>(objects_bytes)
            << "% of the object store) |\n";
  std::cout.unsetf(std::ios::fixed);

  bool ok = bench.print_queries("Queries on a fresh index");

  for (int i = 0; i < added_later; ++i) gen.commit_random_edit();
  ok = bench.print_queries("After " + std::to_string(added_later) +
                           " new commits, before re-indexing (index lags behind HEAD)") &&
       ok;

  start = Clock::now();
  const auto incremental = minigit::HistoryIndex::open_for_update(bench.db()).update(repo);
  const double incremental_ms = ms_since(start);
  std::cout << "\nIncremental `index`: " << incremental.commits_added << " commits in "
            << fmt_ms(incremental_ms) << "; index size now " << kib(fs::file_size(bench.db()))
            << ".\n";
  ok = bench.print_queries("After the incremental update") && ok;

  fs::remove(bench.db());
  start = Clock::now();
  const auto rebuilt = minigit::HistoryIndex::open_for_update(bench.db(), true).update(repo);
  const double rebuild_ms = ms_since(start);
  const bool verified = bench.verify_all(gen.paths());
  std::cout << "\nDeleted and rebuilt the index: " << rebuilt.total_commits << " commits in "
            << fmt_ms(rebuild_ms) << "; full history of all " << gen.paths().size()
            << " paths identical to traversal: " << (verified ? "yes" : "**NO**") << ".\n";
  if (opt.keep) std::cout << "\nRepository kept at " << dir.generic_string() << "\n";
  return ok && verified ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  const std::string usage =
      "usage: history_benchmark [--commits N] [--files N] [--repeats N] [--keep DIR]\n";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (i + 1 >= argc) {
      std::cerr << usage;
      return 2;
    }
    const std::string value = argv[++i];
    std::optional<int> n;
    if (arg == "--keep") {
      opt.keep = value;
      continue;
    }
    n = parse_int(value);
    if (!n) {
      std::cerr << "history_benchmark: '" << value << "' is not a positive number\n" << usage;
      return 2;
    }
    if (arg == "--commits") {
      opt.commits = *n;
    } else if (arg == "--files" && *n >= 3) {
      opt.files = *n;
    } else if (arg == "--repeats") {
      opt.repeats = *n;
    } else {
      std::cerr << usage;
      return 2;
    }
  }
  try {
    return run(opt);
  } catch (const std::exception& e) {
    std::cerr << "history_benchmark: " << e.what() << "\n";
    return 1;
  }
}
