#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace minigit {

namespace fs = std::filesystem;

// Raised for missing, malformed or corrupt objects.
class ObjectError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The object types MiniGit++ supports. Git's annotated tags are out of scope.
enum class ObjectType { Blob, Tree, Commit };

std::string_view to_string(ObjectType type);
std::optional<ObjectType> parse_object_type(std::string_view name);

// An object's type and content, without the header.
struct RawObject {
  ObjectType type;
  std::string content;
};

// Git framing: "<type> <decimal size>\0<content>". The object id is the SHA-1
// of exactly these bytes.
std::string frame_object(ObjectType type, std::string_view content);

// Splits framed bytes back into type and content. Throws ObjectError if the
// header is malformed, the type is unknown, or the size does not match.
RawObject parse_framed_object(std::string_view framed);

// The id an object would have, without storing it.
std::string hash_object(ObjectType type, std::string_view content);

// Loose-object storage: each object is zlib-compressed and stored at
// objects/<first 2 hex chars>/<remaining 38 hex chars>, as Git does.
// Pack files are not supported.
class ObjectStore {
 public:
  explicit ObjectStore(fs::path objects_dir) : objects_dir_(std::move(objects_dir)) {}

  // Stores the object (atomically, skipping the write if it already exists)
  // and returns its id.
  std::string write(ObjectType type, std::string_view content) const;

  // Reads and validates an object. Throws ObjectError if the id is malformed,
  // the object is missing, or the stored bytes are corrupt or do not hash to
  // the requested id.
  RawObject read(std::string_view oid) const;

  bool contains(std::string_view oid) const;

  // Location of the loose object file. Throws ObjectError for invalid ids.
  fs::path object_path(std::string_view oid) const;

 private:
  fs::path objects_dir_;
};

}  // namespace minigit
