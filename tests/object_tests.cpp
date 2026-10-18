#include <gtest/gtest.h>

#include <string>

#include "minigit/compression.hpp"
#include "minigit/hash.hpp"
#include "minigit/object_store.hpp"
#include "minigit/repository.hpp"
#include "test_support.hpp"

namespace {

using minigit::CompressionError;
using minigit::HashError;
using minigit::ObjectError;
using minigit::ObjectStore;
using minigit::ObjectType;
using minigit::Repository;
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

}  // namespace
