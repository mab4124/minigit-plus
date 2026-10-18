#include "minigit/hash.hpp"

#include <openssl/evp.h>

namespace minigit {

// Wraps the opaque OpenSSL type so the header does not need OpenSSL includes.
struct Sha1::Context {
  EVP_MD_CTX* md = nullptr;
};

void Sha1::ContextDeleter::operator()(Context* ctx) const {
  if (ctx == nullptr) return;
  EVP_MD_CTX_free(ctx->md);
  delete ctx;
}

Sha1::Sha1() : ctx_(new Context) {
  ctx_->md = EVP_MD_CTX_new();
  if (ctx_->md == nullptr) throw HashError("EVP_MD_CTX_new failed");
  if (EVP_DigestInit_ex(ctx_->md, EVP_sha1(), nullptr) != 1) {
    throw HashError("EVP_DigestInit_ex(SHA-1) failed");
  }
}

Sha1::~Sha1() = default;
Sha1::Sha1(Sha1&&) noexcept = default;
Sha1& Sha1::operator=(Sha1&&) noexcept = default;

void Sha1::update(std::string_view data) {
  if (finished_) throw HashError("Sha1::update called after the digest was taken");
  if (EVP_DigestUpdate(ctx_->md, data.data(), data.size()) != 1) {
    throw HashError("EVP_DigestUpdate failed");
  }
}

std::string Sha1::raw_digest() {
  if (finished_) throw HashError("Sha1 digest already taken");
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(ctx_->md, digest, &length) != 1 || length != kSha1RawSize) {
    throw HashError("EVP_DigestFinal_ex failed");
  }
  finished_ = true;
  return std::string(reinterpret_cast<const char*>(digest), length);
}

std::string Sha1::hex_digest() { return hex_encode(raw_digest()); }

std::string sha1_hex(std::string_view data) {
  Sha1 hasher;
  hasher.update(data);
  return hasher.hex_digest();
}

std::string hex_encode(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    hex.push_back(kDigits[byte >> 4]);
    hex.push_back(kDigits[byte & 0x0f]);
  }
  return hex;
}

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) throw HashError("hex string has odd length");
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_value(hex[i]);
    const int lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) throw HashError("invalid hex character in '" + std::string(hex) + "'");
    bytes.push_back(static_cast<char>((hi << 4) | lo));
  }
  return bytes;
}

bool is_valid_oid(std::string_view oid) {
  if (oid.size() != kSha1HexSize) return false;
  for (const char c : oid) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace minigit
