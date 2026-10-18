#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minigit {

// Raised when hashing fails or a hex object id is malformed.
class HashError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Number of bytes in a raw SHA-1 digest and characters in its hex form.
inline constexpr std::size_t kSha1RawSize = 20;
inline constexpr std::size_t kSha1HexSize = 40;

// Incremental SHA-1 hasher built on OpenSSL's EVP interface.
//
// The OpenSSL context is owned through a unique_ptr with a custom deleter, so
// it is released even if a caller throws between update() calls.
class Sha1 {
 public:
  Sha1();
  ~Sha1();
  Sha1(const Sha1&) = delete;
  Sha1& operator=(const Sha1&) = delete;
  Sha1(Sha1&&) noexcept;
  Sha1& operator=(Sha1&&) noexcept;

  void update(std::string_view data);

  // Finishes the hash and returns the 20 raw digest bytes. The hasher cannot
  // be updated afterwards.
  std::string raw_digest();
  std::string hex_digest();

 private:
  struct Context;
  struct ContextDeleter {
    void operator()(Context* ctx) const;
  };
  std::unique_ptr<Context, ContextDeleter> ctx_;
  bool finished_ = false;
};

// One-shot helper: lowercase hex SHA-1 of `data`.
std::string sha1_hex(std::string_view data);

// Lowercase hex encoding of arbitrary bytes, and its inverse.
// hex_decode throws HashError on odd length or non-hex characters.
std::string hex_encode(std::string_view bytes);
std::string hex_decode(std::string_view hex);

// True if `oid` is exactly 40 lowercase hex characters.
bool is_valid_oid(std::string_view oid);

}  // namespace minigit
