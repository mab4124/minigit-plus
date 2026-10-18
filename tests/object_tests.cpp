#include <gtest/gtest.h>

#include <string>

#include "minigit/compression.hpp"
#include "minigit/hash.hpp"

namespace {

using minigit::CompressionError;
using minigit::HashError;

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

}  // namespace
