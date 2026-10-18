#include "minigit/compression.hpp"

#include <zlib.h>

#include <climits>

namespace minigit {
namespace {

constexpr std::size_t kChunkSize = 16 * 1024;

// Owns a z_stream for one deflate or inflate operation and always calls the
// matching *End function, even when we throw midway.
class ZStream {
 public:
  enum class Mode { Deflate, Inflate };

  explicit ZStream(Mode mode) : mode_(mode) {
    const int rc = mode == Mode::Deflate ? deflateInit(&stream_, Z_DEFAULT_COMPRESSION)
                                         : inflateInit(&stream_);
    if (rc != Z_OK) throw CompressionError(std::string("zlib init failed: ") + message(rc));
  }
  ~ZStream() {
    if (mode_ == Mode::Deflate) {
      deflateEnd(&stream_);
    } else {
      inflateEnd(&stream_);
    }
  }
  ZStream(const ZStream&) = delete;
  ZStream& operator=(const ZStream&) = delete;

  z_stream* get() { return &stream_; }

  const char* message(int rc) const {
    if (stream_.msg != nullptr) return stream_.msg;
    return zError(rc);
  }

 private:
  Mode mode_;
  z_stream stream_{};
};

void check_size(std::string_view data) {
  // zlib's avail_in is a 32-bit uInt; MiniGit++ objects are far smaller.
  if (data.size() > UINT_MAX) throw CompressionError("input too large for zlib");
}

}  // namespace

std::string zlib_compress(std::string_view data) {
  check_size(data);
  ZStream zs(ZStream::Mode::Deflate);
  z_stream* s = zs.get();
  s->next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  s->avail_in = static_cast<uInt>(data.size());

  std::string out;
  char buffer[kChunkSize];
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    s->next_out = reinterpret_cast<Bytef*>(buffer);
    s->avail_out = sizeof(buffer);
    rc = deflate(s, Z_FINISH);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
      throw CompressionError(std::string("deflate failed: ") + zs.message(rc));
    }
    out.append(buffer, sizeof(buffer) - s->avail_out);
  }
  return out;
}

std::string zlib_decompress(std::string_view data) {
  check_size(data);
  ZStream zs(ZStream::Mode::Inflate);
  z_stream* s = zs.get();
  s->next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  s->avail_in = static_cast<uInt>(data.size());

  std::string out;
  char buffer[kChunkSize];
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    s->next_out = reinterpret_cast<Bytef*>(buffer);
    s->avail_out = sizeof(buffer);
    rc = inflate(s, Z_NO_FLUSH);
    if (rc == Z_BUF_ERROR && s->avail_in == 0) {
      throw CompressionError("compressed data is truncated");
    }
    if (rc != Z_OK && rc != Z_STREAM_END) {
      throw CompressionError(std::string("compressed data is corrupt: ") + zs.message(rc));
    }
    out.append(buffer, sizeof(buffer) - s->avail_out);
  }
  if (s->avail_in != 0) throw CompressionError("trailing bytes after compressed data");
  return out;
}

}  // namespace minigit
