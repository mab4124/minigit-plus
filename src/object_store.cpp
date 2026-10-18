#include "minigit/object_store.hpp"

#include <charconv>
#include <system_error>

#include "minigit/compression.hpp"
#include "minigit/file_util.hpp"
#include "minigit/hash.hpp"

namespace minigit {

std::string_view to_string(ObjectType type) {
  switch (type) {
    case ObjectType::Blob:
      return "blob";
    case ObjectType::Tree:
      return "tree";
    case ObjectType::Commit:
      return "commit";
  }
  return "unknown";
}

std::optional<ObjectType> parse_object_type(std::string_view name) {
  if (name == "blob") return ObjectType::Blob;
  if (name == "tree") return ObjectType::Tree;
  if (name == "commit") return ObjectType::Commit;
  return std::nullopt;
}

std::string frame_object(ObjectType type, std::string_view content) {
  std::string framed(to_string(type));
  framed += ' ';
  framed += std::to_string(content.size());
  framed += '\0';
  framed.append(content);
  return framed;
}

RawObject parse_framed_object(std::string_view framed) {
  const auto nul = framed.find('\0');
  if (nul == std::string_view::npos) throw ObjectError("object header has no NUL terminator");
  const std::string_view header = framed.substr(0, nul);
  const auto space = header.find(' ');
  if (space == std::string_view::npos) throw ObjectError("object header has no size");

  const std::string_view type_name = header.substr(0, space);
  const auto type = parse_object_type(type_name);
  if (!type) throw ObjectError("unsupported object type '" + std::string(type_name) + "'");

  const std::string_view size_text = header.substr(space + 1);
  std::size_t size = 0;
  const auto [end, ec] = std::from_chars(size_text.data(), size_text.data() + size_text.size(), size);
  if (size_text.empty() || ec != std::errc() || end != size_text.data() + size_text.size() ||
      (size_text.size() > 1 && size_text.front() == '0')) {
    throw ObjectError("object header has invalid size '" + std::string(size_text) + "'");
  }

  const std::string_view content = framed.substr(nul + 1);
  if (content.size() != size) {
    throw ObjectError("object size mismatch: header says " + std::to_string(size) +
                      " bytes, found " + std::to_string(content.size()));
  }
  return RawObject{*type, std::string(content)};
}

std::string hash_object(ObjectType type, std::string_view content) {
  return sha1_hex(frame_object(type, content));
}

fs::path ObjectStore::object_path(std::string_view oid) const {
  // Validating first also prevents ids like "../../x" from escaping objects/.
  if (!is_valid_oid(oid)) throw ObjectError("not a valid object id: '" + std::string(oid) + "'");
  return objects_dir_ / std::string(oid.substr(0, 2)) / std::string(oid.substr(2));
}

bool ObjectStore::contains(std::string_view oid) const {
  if (!is_valid_oid(oid)) return false;
  std::error_code ec;
  return fs::is_regular_file(object_path(oid), ec);
}

std::string ObjectStore::write(ObjectType type, std::string_view content) const {
  const std::string framed = frame_object(type, content);
  const std::string oid = sha1_hex(framed);
  if (contains(oid)) return oid;  // objects are immutable: same id, same bytes

  const fs::path path = object_path(oid);
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) throw ObjectError("cannot create " + path.parent_path().string() + ": " + ec.message());
  write_file_atomic(path, zlib_compress(framed));
  return oid;
}

RawObject ObjectStore::read(std::string_view oid) const {
  const fs::path path = object_path(oid);
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) throw ObjectError("object " + std::string(oid) + " not found");

  std::string framed;
  try {
    framed = zlib_decompress(read_file(path));
  } catch (const CompressionError& e) {
    throw ObjectError("object " + std::string(oid) + " is corrupt: " + e.what());
  }
  if (sha1_hex(framed) != oid) {
    throw ObjectError("object " + std::string(oid) + " is corrupt: content hash mismatch");
  }
  try {
    return parse_framed_object(framed);
  } catch (const ObjectError& e) {
    throw ObjectError("object " + std::string(oid) + " is malformed: " + e.what());
  }
}

}  // namespace minigit
