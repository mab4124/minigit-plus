#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace minigit {

// Raised when zlib reports an error or the compressed data is corrupt.
class CompressionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Compresses `data` into a zlib stream (the format Git uses for loose objects).
std::string zlib_compress(std::string_view data);

// Inflates a complete zlib stream. Throws CompressionError if the stream is
// corrupt, truncated, or followed by trailing bytes.
std::string zlib_decompress(std::string_view data);

}  // namespace minigit
