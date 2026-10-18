#include <gtest/gtest.h>

#include "minigit/repository.hpp"
#include "minigit/staging_index.hpp"
#include "test_support.hpp"

namespace {

using minigit::IndexEntry;
using minigit::IndexError;
using minigit::Repository;
using minigit::StagingIndex;
using minigit::test::read_text;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

constexpr const char* kHelloBlob = "ce013625030ba8dba906f756967f9e9ca394464a";
constexpr const char* kEmptyBlob = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";

class StagingIndexTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
};

TEST_F(StagingIndexTest, MissingFileLoadsEmpty) {
  const StagingIndex index = StagingIndex::load(repo.index_path());
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.file(), tmp.path() / ".git" / "minigit" / "index");
}

TEST_F(StagingIndexTest, SaveWritesSortedTextFormat) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  index.stage("src/main.cpp", {"100644", kHelloBlob});
  index.stage("README", {"100755", kEmptyBlob});
  index.save();
  EXPECT_EQ(read_text(repo.index_path()),
            "# minigit index v1\n"
            "100755 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 README\n"
            "100644 ce013625030ba8dba906f756967f9e9ca394464a src/main.cpp\n");
}

TEST_F(StagingIndexTest, SaveAndLoadRoundTrip) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  index.stage("a b/c d.txt", {"100644", kHelloBlob});
  index.stage("x", {"100755", kEmptyBlob});
  index.save();

  const StagingIndex loaded = StagingIndex::load(repo.index_path());
  EXPECT_EQ(loaded.entries(), index.entries());
  ASSERT_NE(loaded.find("a b/c d.txt"), nullptr);
  EXPECT_EQ(*loaded.find("a b/c d.txt"), (IndexEntry{"100644", kHelloBlob}));
  EXPECT_EQ(loaded.find("missing"), nullptr);
}

TEST_F(StagingIndexTest, SaveLeavesNoTempFiles) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  index.stage("f", {"100644", kHelloBlob});
  index.save();
  index.save();
  int files = 0;
  for (const auto& entry : fs::directory_iterator(repo.minigit_dir())) {
    EXPECT_EQ(entry.path().filename(), "index");
    ++files;
  }
  EXPECT_EQ(files, 1);
}

TEST_F(StagingIndexTest, StageReplacesAndRemoves) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  index.stage("f", {"100644", kHelloBlob});
  index.stage("f", {"100755", kEmptyBlob});
  EXPECT_EQ(index.entries().size(), 1u);
  EXPECT_EQ(index.find("f")->oid, kEmptyBlob);
  EXPECT_TRUE(index.remove("f"));
  EXPECT_FALSE(index.remove("f"));
  EXPECT_TRUE(index.empty());
}

TEST_F(StagingIndexTest, PathsUnderMatchesWholeComponents) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  for (const char* p : {"a", "a.txt", "ab/c", "b/c/d", "b/e", "bc"}) {
    index.stage(p, {"100644", kHelloBlob});
  }
  EXPECT_EQ(index.paths_under("a"), std::vector<std::string>{"a"});
  EXPECT_EQ(index.paths_under("b"), (std::vector<std::string>{"b/c/d", "b/e"}));
  EXPECT_EQ(index.paths_under("b/c"), std::vector<std::string>{"b/c/d"});
  EXPECT_TRUE(index.paths_under("zzz").empty());
  EXPECT_EQ(index.paths_under("").size(), 6u);
}

TEST_F(StagingIndexTest, FileAndDirectoryConflictsAreResolved) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  index.stage("a", {"100644", kHelloBlob});
  index.stage("ab", {"100644", kHelloBlob});
  index.stage("a/b/c", {"100644", kHelloBlob});  // "a" became a directory
  EXPECT_EQ(index.find("a"), nullptr);
  EXPECT_NE(index.find("ab"), nullptr);

  index.stage("a/b/d", {"100644", kHelloBlob});
  index.stage("a/b", {"100644", kEmptyBlob});  // "a/b" became a file again
  EXPECT_EQ(index.find("a/b/c"), nullptr);
  EXPECT_EQ(index.find("a/b/d"), nullptr);
  EXPECT_EQ(index.find("a/b")->oid, kEmptyBlob);
  EXPECT_EQ(index.entries().size(), 2u);
}

TEST_F(StagingIndexTest, RejectsInvalidPathsAndEntries) {
  StagingIndex index = StagingIndex::load(repo.index_path());
  const std::vector<std::string> bad_paths = {"",       "/abs", "a//b", "a/",          "./a",
                                              "a/../b", "..",   ".git", ".git/config", "new\nline",
                                              std::string("n\0l", 3)};
  for (const auto& bad : bad_paths) {
    EXPECT_THROW(index.stage(bad, {"100644", kHelloBlob}), IndexError) << bad;
  }
  EXPECT_NO_THROW(index.stage("sub/.git-notes", {"100644", kHelloBlob}));
  EXPECT_THROW(index.stage("f", {"040000", kHelloBlob}), IndexError);
  EXPECT_THROW(index.stage("f", {"100644", "abc"}), IndexError);
}

TEST_F(StagingIndexTest, MalformedFilesAreReported) {
  const auto load_text = [&](const std::string& text) {
    write_text(repo.index_path(), text);
    return StagingIndex::load(repo.index_path());
  };
  EXPECT_THROW(load_text(""), IndexError);
  EXPECT_THROW(load_text("DIRC binary git index\n"), IndexError);
  EXPECT_THROW(load_text("# minigit index v1\n100644 abc f\n"), IndexError);
  EXPECT_THROW(load_text("# minigit index v1\n100644 " + std::string(kHelloBlob) + "\n"),
               IndexError);
  EXPECT_THROW(load_text("# minigit index v1\n100644 " + std::string(kHelloBlob) + " f"),
               IndexError);  // truncated
  const std::string line = "100644 " + std::string(kHelloBlob) + " f\n";
  EXPECT_THROW(load_text("# minigit index v1\n" + line + line), IndexError);  // duplicate
  try {
    load_text("# minigit index v1\n" + line + "100644 " + kHelloBlob + " ../escape\n");
    FAIL() << "expected IndexError";
  } catch (const IndexError& e) {
    EXPECT_NE(std::string(e.what()).find("line 3"), std::string::npos) << e.what();
  }
  EXPECT_NO_THROW(load_text("# minigit index v1\n"));
}

}  // namespace
