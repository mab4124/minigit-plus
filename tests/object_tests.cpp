#include <gtest/gtest.h>

#include <string>

#include "minigit/commit.hpp"
#include "minigit/compression.hpp"
#include "minigit/hash.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "minigit/tree.hpp"
#include "test_support.hpp"

namespace {

using minigit::Commit;
using minigit::CompressionError;
using minigit::HashError;
using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
using minigit::Signature;
using minigit::TreeEntry;
using minigit::test::git_available;
using minigit::test::git_in;
using minigit::test::read_text;
using minigit::test::TempDir;
using minigit::test::write_text;
namespace fs = std::filesystem;

// `printf 'hello\n' | git hash-object --stdin`
constexpr const char* kHelloBlob = "ce013625030ba8dba906f756967f9e9ca394464a";
// `git hash-object /dev/null`
constexpr const char* kEmptyBlob = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";

// Reference vectors from FIPS 180-1.
TEST(Sha1, KnownVectors) {
  EXPECT_EQ(minigit::sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(minigit::sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(minigit::sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1, IncrementalMatchesOneShot) {
  minigit::Sha1 hasher;
  hasher.update("ab");
  hasher.update("");
  hasher.update("c");
  EXPECT_EQ(hasher.hex_digest(), minigit::sha1_hex("abc"));
}

TEST(Sha1, HandlesEmbeddedNulBytes) {
  const std::string data("a\0b", 3);
  EXPECT_NE(minigit::sha1_hex(data), minigit::sha1_hex("a"));
  EXPECT_EQ(minigit::sha1_hex(data).size(), minigit::kSha1HexSize);
}

TEST(Sha1, UpdateAfterDigestThrows) {
  minigit::Sha1 hasher;
  hasher.update("x");
  (void)hasher.raw_digest();
  EXPECT_THROW(hasher.update("y"), HashError);
  EXPECT_THROW((void)hasher.raw_digest(), HashError);
}

TEST(Hex, RoundTrip) {
  const std::string bytes("\x00\x01\x7f\x80\xff", 5);
  EXPECT_EQ(minigit::hex_encode(bytes), "00017f80ff");
  EXPECT_EQ(minigit::hex_decode("00017f80ff"), bytes);
  EXPECT_EQ(minigit::hex_decode("ABcd"), "\xab\xcd");
}

TEST(Hex, RejectsMalformedInput) {
  EXPECT_THROW(minigit::hex_decode("abc"), HashError);
  EXPECT_THROW(minigit::hex_decode("zz"), HashError);
}

TEST(Hex, ValidatesObjectIds) {
  EXPECT_TRUE(minigit::is_valid_oid("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
  EXPECT_FALSE(minigit::is_valid_oid("da39a3ee5e6b4b0d3255bfef95601890afd8070"));   // 39 chars
  EXPECT_FALSE(minigit::is_valid_oid("DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"));  // uppercase
  EXPECT_FALSE(minigit::is_valid_oid("../../../../../../../../../../etc/passwd"));
}

TEST(Zlib, RoundTripsTextAndBinary) {
  std::string binary;
  for (int i = 0; i < 100000; ++i) binary.push_back(static_cast<char>(i * 31 % 256));
  for (const std::string& input : {std::string(), std::string("hello\n"), binary}) {
    EXPECT_EQ(minigit::zlib_decompress(minigit::zlib_compress(input)), input);
  }
}

TEST(Zlib, CompressedOutputHasZlibHeader) {
  const std::string z = minigit::zlib_compress("hello");
  ASSERT_GE(z.size(), 2u);
  EXPECT_EQ(static_cast<unsigned char>(z[0]), 0x78);  // deflate, 32K window
}

TEST(Zlib, RejectsCorruptData) {
  EXPECT_THROW(minigit::zlib_decompress("not zlib at all"), CompressionError);
}

TEST(Zlib, RejectsTruncatedData) {
  const std::string z = minigit::zlib_compress(std::string(1000, 'x') + "tail");
  EXPECT_THROW(minigit::zlib_decompress(z.substr(0, z.size() - 4)), CompressionError);
  EXPECT_THROW(minigit::zlib_decompress(""), CompressionError);
}

TEST(Zlib, RejectsTrailingBytes) {
  EXPECT_THROW(minigit::zlib_decompress(minigit::zlib_compress("abc") + "junk"),
               CompressionError);
}

// --- Object framing ---------------------------------------------------------

TEST(ObjectFraming, FramesWithTypeAndSize) {
  EXPECT_EQ(minigit::frame_object(ObjectType::Blob, "hello\n"), std::string("blob 6\0hello\n", 13));
  EXPECT_EQ(minigit::frame_object(ObjectType::Tree, ""), std::string("tree 0\0", 7));
}

TEST(ObjectFraming, HashesMatchGitFixtures) {
  EXPECT_EQ(minigit::hash_object(ObjectType::Blob, "hello\n"), kHelloBlob);
  EXPECT_EQ(minigit::hash_object(ObjectType::Blob, ""), kEmptyBlob);
  // The empty tree id is hard-coded inside Git itself.
  EXPECT_EQ(minigit::hash_object(ObjectType::Tree, ""), "4b825dc642cb6eb9a060e54bf8d69288fbee4904");
}

TEST(ObjectFraming, ParseRoundTrip) {
  const std::string content("bin\0ary", 7);
  const auto obj = minigit::parse_framed_object(minigit::frame_object(ObjectType::Commit, content));
  EXPECT_EQ(obj.type, ObjectType::Commit);
  EXPECT_EQ(obj.content, content);
}

TEST(ObjectFraming, RejectsMalformedHeaders) {
  using minigit::parse_framed_object;
  EXPECT_THROW(parse_framed_object("blob 5"), ObjectError);                         // no NUL
  EXPECT_THROW(parse_framed_object(std::string("blob\0x", 6)), ObjectError);        // no size
  EXPECT_THROW(parse_framed_object(std::string("tag 1\0x", 7)), ObjectError);       // unsupported
  EXPECT_THROW(parse_framed_object(std::string("blob 2\0x", 8)), ObjectError);      // too short
  EXPECT_THROW(parse_framed_object(std::string("blob 0\0x", 8)), ObjectError);      // too long
  EXPECT_THROW(parse_framed_object(std::string("blob -1\0", 8)), ObjectError);      // sign
  EXPECT_THROW(parse_framed_object(std::string("blob 01\0x", 9)), ObjectError);     // leading 0
  EXPECT_THROW(parse_framed_object(std::string("blob 1x\0x", 9)), ObjectError);     // junk
}

// --- Loose object store -------------------------------------------------------

class ObjectStoreTest : public ::testing::Test {
 protected:
  TempDir tmp;
  Repository repo = Repository::init(tmp.path());
  ObjectStore store{repo.objects_dir()};
};

TEST_F(ObjectStoreTest, WritesUnderFanOutDirectory) {
  const std::string oid = store.write(ObjectType::Blob, "hello\n");
  EXPECT_EQ(oid, kHelloBlob);
  const fs::path expected = repo.objects_dir() / "ce" / "013625030ba8dba906f756967f9e9ca394464a";
  EXPECT_EQ(store.object_path(oid), expected);
  EXPECT_TRUE(fs::is_regular_file(expected));
  EXPECT_TRUE(store.contains(oid));
  EXPECT_EQ(minigit::zlib_decompress(read_text(expected)), std::string("blob 6\0hello\n", 13));
}

TEST_F(ObjectStoreTest, ReadBackRoundTrip) {
  std::string binary;
  for (int i = 0; i < 5000; ++i) binary.push_back(static_cast<char>(i % 256));
  const std::string oid = store.write(ObjectType::Blob, binary);
  const auto obj = store.read(oid);
  EXPECT_EQ(obj.type, ObjectType::Blob);
  EXPECT_EQ(obj.content, binary);
}

TEST_F(ObjectStoreTest, RewritingIsIdempotentAndLeavesNoTempFiles) {
  const std::string a = store.write(ObjectType::Blob, "same");
  const std::string b = store.write(ObjectType::Blob, "same");
  EXPECT_EQ(a, b);
  int files = 0;
  for (const auto& entry : fs::recursive_directory_iterator(repo.objects_dir())) {
    if (entry.is_regular_file()) ++files;
  }
  EXPECT_EQ(files, 1);
}

TEST_F(ObjectStoreTest, MissingObjectIsReported) {
  EXPECT_FALSE(store.contains(kHelloBlob));
  try {
    store.read(kHelloBlob);
    FAIL() << "expected ObjectError";
  } catch (const ObjectError& e) {
    EXPECT_NE(std::string(e.what()).find("not found"), std::string::npos);
  }
}

TEST_F(ObjectStoreTest, InvalidIdsAreRejected) {
  EXPECT_THROW(store.read("abc"), ObjectError);
  EXPECT_THROW(store.read("../../../../../../../../../../../../etc/x"), ObjectError);
  EXPECT_FALSE(store.contains("not-an-id"));
}

TEST_F(ObjectStoreTest, CorruptCompressedDataIsReported) {
  const std::string oid = store.write(ObjectType::Blob, "hello\n");
  write_text(store.object_path(oid), "garbage");
  EXPECT_THROW(store.read(oid), ObjectError);
}

TEST_F(ObjectStoreTest, HashMismatchIsReported) {
  // Put valid object bytes under the wrong id.
  const std::string oid = store.write(ObjectType::Blob, "hello\n");
  const fs::path wrong = store.object_path(kEmptyBlob);
  fs::create_directories(wrong.parent_path());
  fs::copy_file(store.object_path(oid), wrong);
  try {
    store.read(kEmptyBlob);
    FAIL() << "expected ObjectError";
  } catch (const ObjectError& e) {
    EXPECT_NE(std::string(e.what()).find("hash mismatch"), std::string::npos);
  }
}

TEST_F(ObjectStoreTest, MalformedHeaderIsReported) {
  // A well-compressed object whose header is wrong; stored under its true hash.
  const std::string framed("blob 99\0short", 13);
  const std::string oid = minigit::sha1_hex(framed);
  fs::create_directories(store.object_path(oid).parent_path());
  write_text(store.object_path(oid), minigit::zlib_compress(framed));
  try {
    store.read(oid);
    FAIL() << "expected ObjectError";
  } catch (const ObjectError& e) {
    EXPECT_NE(std::string(e.what()).find("malformed"), std::string::npos);
  }
}

// Real Git reads objects that MiniGit++ wrote ...
TEST_F(ObjectStoreTest, GitOracleReadsOurBlob) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  const std::string oid = store.write(ObjectType::Blob, "hello\n");
  const auto type = git_in(tmp.path(), "cat-file -t " + oid);
  ASSERT_EQ(type.status, 0);
  EXPECT_EQ(type.output, "blob");
  const auto content = git_in(tmp.path(), "cat-file -p " + oid);
  ASSERT_EQ(content.status, 0);
  EXPECT_EQ(content.output, "hello");
}

// ... and MiniGit++ reads objects that real Git wrote.
TEST_F(ObjectStoreTest, ReadsBlobWrittenByGit) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  write_text(tmp.path() / "file.txt", "written by git\n");
  const auto r = git_in(tmp.path(), "hash-object -w file.txt");
  ASSERT_EQ(r.status, 0);
  const auto obj = store.read(r.output);
  EXPECT_EQ(obj.type, ObjectType::Blob);
  EXPECT_EQ(obj.content, "written by git\n");
}

// --- Trees --------------------------------------------------------------------

TEST(Tree, SortsLikeGitWithDirectorySlash) {
  // Byte order: '-' (0x2d) < '.' (0x2e) < '/' (0x2f) < '0' (0x30).
  const std::vector<TreeEntry> entries = {
      {"100644", "a0", kHelloBlob},
      {"40000", "a", kEmptyBlob},
      {"100644", "a.txt", kHelloBlob},
      {"100644", "a-b", kHelloBlob},
  };
  const auto parsed = minigit::parse_tree(minigit::serialize_tree(entries));
  ASSERT_EQ(parsed.size(), 4u);
  EXPECT_EQ(parsed[0].name, "a-b");
  EXPECT_EQ(parsed[1].name, "a.txt");
  EXPECT_EQ(parsed[2].name, "a");
  EXPECT_EQ(parsed[3].name, "a0");
}

TEST(Tree, FileSortsWithoutSlash) {
  // As a file, "a" sorts before "a.txt"; as a directory it sorts after.
  EXPECT_TRUE(minigit::tree_entry_less({"100644", "a", kHelloBlob}, {"100644", "a.txt", kHelloBlob}));
  EXPECT_FALSE(minigit::tree_entry_less({"40000", "a", kHelloBlob}, {"100644", "a.txt", kHelloBlob}));
}

TEST(Tree, SerializesBinaryIds) {
  const std::string data = minigit::serialize_tree({{"100644", "hello.txt", kHelloBlob}});
  EXPECT_EQ(data, std::string("100644 hello.txt\0", 17) + minigit::hex_decode(kHelloBlob));
  EXPECT_EQ(minigit::serialize_tree({}), "");
}

TEST(Tree, RejectsInvalidEntries) {
  using minigit::serialize_tree;
  EXPECT_THROW(serialize_tree({{"100600", "x", kHelloBlob}}), ObjectError);
  EXPECT_THROW(serialize_tree({{"100644", "", kHelloBlob}}), ObjectError);
  EXPECT_THROW(serialize_tree({{"100644", "a/b", kHelloBlob}}), ObjectError);
  EXPECT_THROW(serialize_tree({{"100644", "..", kHelloBlob}}), ObjectError);
  EXPECT_THROW(serialize_tree({{"100644", "x", "nothex"}}), ObjectError);
  EXPECT_THROW(serialize_tree({{"100644", "x", kHelloBlob}, {"100755", "x", kEmptyBlob}}),
               ObjectError);
}

TEST(Tree, RejectsTruncatedData) {
  const std::string data = minigit::serialize_tree({{"100644", "f", kHelloBlob}});
  EXPECT_THROW(minigit::parse_tree(data.substr(0, data.size() - 1)), ObjectError);
  EXPECT_THROW(minigit::parse_tree("100644 f"), ObjectError);
  EXPECT_THROW(minigit::parse_tree("100644"), ObjectError);
}

// --- Commits ------------------------------------------------------------------

Commit sample_commit() {
  Commit c;
  c.tree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
  c.parents = {kHelloBlob};
  c.author = {"Ada Lovelace", "ada@example.com", 1700000000, "+0530"};
  c.committer = {"Grace Hopper", "grace@example.com", 1700000100, "-0800"};
  c.message = "Add analytical engine\n\nLonger body.\n";
  return c;
}

TEST(CommitObject, SerializesInGitHeaderOrder) {
  EXPECT_EQ(minigit::serialize_commit(sample_commit()),
            "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
            "parent ce013625030ba8dba906f756967f9e9ca394464a\n"
            "author Ada Lovelace <ada@example.com> 1700000000 +0530\n"
            "committer Grace Hopper <grace@example.com> 1700000100 -0800\n"
            "\n"
            "Add analytical engine\n\nLonger body.\n");
}

TEST(CommitObject, ParseRoundTrip) {
  const Commit c = sample_commit();
  EXPECT_EQ(minigit::parse_commit(minigit::serialize_commit(c)), c);

  Commit root = c;
  root.parents.clear();
  EXPECT_EQ(minigit::parse_commit(minigit::serialize_commit(root)), root);
}

TEST(CommitObject, SkipsUnknownAndMultiLineHeaders) {
  const std::string text =
      "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
      "author A <a@x> 1 +0000\n"
      "committer B <b@x> 2 +0000\n"
      "encoding UTF-8\n"
      "gpgsig -----BEGIN PGP SIGNATURE-----\n"
      " abc\n"
      " -----END PGP SIGNATURE-----\n"
      "\n"
      "msg\n";
  const Commit c = minigit::parse_commit(text);
  EXPECT_EQ(c.author.name, "A");
  EXPECT_EQ(c.committer.timestamp, 2);
  EXPECT_EQ(c.message, "msg\n");
}

TEST(CommitObject, RejectsMalformedCommits) {
  using minigit::parse_commit;
  const std::string tree = "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n";
  const std::string sigs = "author A <a@x> 1 +0000\ncommitter B <b@x> 2 +0000\n";
  EXPECT_THROW(parse_commit(sigs + "\nmsg"), ObjectError);                      // no tree
  EXPECT_THROW(parse_commit(tree + "committer B <b@x> 2 +0000\n\n"), ObjectError);  // no author
  EXPECT_THROW(parse_commit(tree + sigs), ObjectError);                         // no blank line
  EXPECT_THROW(parse_commit("tree abc\n" + sigs + "\n"), ObjectError);          // bad tree id
  EXPECT_THROW(parse_commit(tree + "parent xyz\n" + sigs + "\n"), ObjectError);  // bad parent
  EXPECT_THROW(parse_commit(tree + "author A a@x 1 +0000\ncommitter B <b@x> 2 +0000\n\n"),
               ObjectError);  // no angle brackets
  EXPECT_THROW(parse_commit(tree + "author A <a@x> soon +0000\ncommitter B <b@x> 2 +0000\n\n"),
               ObjectError);  // bad timestamp
  EXPECT_THROW(parse_commit(tree + "author A <a@x> 1 UTC\ncommitter B <b@x> 2 +0000\n\n"),
               ObjectError);  // bad timezone
}

TEST(CommitObject, RejectsUnrepresentableSignatures) {
  Commit c = sample_commit();
  c.author.name = "Evil <name>";
  EXPECT_THROW(minigit::serialize_commit(c), ObjectError);
  c = sample_commit();
  c.author.email = "a\n@b";
  EXPECT_THROW(minigit::serialize_commit(c), ObjectError);
}

// --- Git oracle for trees and commits -----------------------------------------

// Builds the same nested tree with `git add` + `git write-tree` and with our
// own serializer, and checks that the ids agree.
TEST_F(ObjectStoreTest, TreeIdsMatchGitWriteTree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  write_text(tmp.path() / "a.txt", "dot\n");
  write_text(tmp.path() / "a-b", "dash\n");
  write_text(tmp.path() / "a0", "zero\n");
  write_text(tmp.path() / "a" / "inner.txt", "inner\n");
  write_text(tmp.path() / "empty", "");
  ASSERT_EQ(git_in(tmp.path(), "add .").status, 0);
  const auto git_tree = git_in(tmp.path(), "write-tree");
  ASSERT_EQ(git_tree.status, 0);

  auto blob = [&](const std::string& text) { return store.write(ObjectType::Blob, text); };
  const std::string sub =
      store.write(ObjectType::Tree, minigit::serialize_tree({{"100644", "inner.txt", blob("inner\n")}}));
  const std::string ours = store.write(
      ObjectType::Tree, minigit::serialize_tree({
                            {"100644", "a0", blob("zero\n")},
                            {"100644", "empty", blob("")},
                            {"40000", "a", sub},
                            {"100644", "a.txt", blob("dot\n")},
                            {"100644", "a-b", blob("dash\n")},
                        }));
  EXPECT_EQ(ours, git_tree.output);

  // And our parser understands the tree Git wrote, in Git's order.
  const auto parsed = minigit::parse_tree(store.read(git_tree.output).content);
  std::vector<std::string> names;
  for (const auto& e : parsed) names.push_back(e.name);
  EXPECT_EQ(names, (std::vector<std::string>{"a-b", "a.txt", "a", "a0", "empty"}));
  EXPECT_TRUE(parsed[2].is_tree());
}

TEST_F(ObjectStoreTest, ExecutableModeMatchesGitMktree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  const std::string a = store.write(ObjectType::Blob, "#!/bin/sh\n");
  write_text(tmp.path() / "mktree.txt", "100755 blob " + a + "\trun.sh\n100644 blob " +
                                            std::string(kHelloBlob) + "\treadme\n");
  store.write(ObjectType::Blob, "hello\n");  // mktree checks that entries exist
  const auto git_tree = git_in(tmp.path(), "mktree < " + minigit::test::quote(tmp.path() / "mktree.txt"));
  ASSERT_EQ(git_tree.status, 0);
  EXPECT_EQ(minigit::hash_object(ObjectType::Tree,
                                 minigit::serialize_tree({{"100644", "readme", kHelloBlob},
                                                          {"100755", "run.sh", a}})),
            git_tree.output);
}

TEST_F(ObjectStoreTest, CommitIdsMatchGitCommitTree) {
  if (!git_available()) GTEST_SKIP() << "git not installed";
  using minigit::test::ScopedEnv;
  ScopedEnv an("GIT_AUTHOR_NAME", "Ada Lovelace"), ae("GIT_AUTHOR_EMAIL", "ada@example.com"),
      ad("GIT_AUTHOR_DATE", "1700000000 +0530"), cn("GIT_COMMITTER_NAME", "Grace Hopper"),
      ce("GIT_COMMITTER_EMAIL", "grace@example.com"),
      cd("GIT_COMMITTER_DATE", "1700000100 -0800");

  const std::string tree = store.write(ObjectType::Tree, "");
  const auto root = git_in(tmp.path(), "commit-tree " + tree + " -m first");
  ASSERT_EQ(root.status, 0);
  const auto child = git_in(tmp.path(), "commit-tree " + tree + " -p " + root.output + " -m second");
  ASSERT_EQ(child.status, 0);

  Commit c;
  c.tree = tree;
  c.author = {"Ada Lovelace", "ada@example.com", 1700000000, "+0530"};
  c.committer = {"Grace Hopper", "grace@example.com", 1700000100, "-0800"};
  c.message = "first\n";
  EXPECT_EQ(minigit::hash_object(ObjectType::Commit, minigit::serialize_commit(c)), root.output);

  // Parse Git's child commit and check every field.
  const auto obj = store.read(child.output);
  ASSERT_EQ(obj.type, ObjectType::Commit);
  const Commit parsed = minigit::parse_commit(obj.content);
  EXPECT_EQ(parsed.tree, tree);
  EXPECT_EQ(parsed.parents, std::vector<std::string>{root.output});
  EXPECT_EQ(parsed.author, c.author);
  EXPECT_EQ(parsed.committer, c.committer);
  EXPECT_EQ(parsed.message, "second\n");
}

}  // namespace
