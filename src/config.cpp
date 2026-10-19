#include "minigit/config.hpp"

#include <cctype>
#include <system_error>

#include "minigit/file_util.hpp"

namespace minigit {
namespace {

std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

bool is_name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
}

// What one line of the file means.
struct ParsedLine {
  enum class Kind { Other, Section, Entry } kind = Kind::Other;
  std::string section;  // lowercase; for a subsection header, the whole "name \"sub\""
  std::string key;      // lowercase entry name
  std::string value;
};

// Parses an entry value: strips comments and surrounding whitespace outside
// quotes, removes the quotes and decodes escapes.
std::string parse_value(std::string_view raw, int line_no) {
  std::string value;
  bool quoted = false;
  std::size_t keep = 0;  // value.size() up to the last character to keep
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '"') {
      quoted = !quoted;
      keep = value.size();
    } else if (c == '\\') {
      if (i + 1 == raw.size()) {
        throw ConfigError("config line " + std::to_string(line_no) +
                          ": line continuations are not supported");
      }
      switch (raw[++i]) {
        case '\\': value += '\\'; break;
        case '"': value += '"'; break;
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case 'b': value += '\b'; break;
        default:
          throw ConfigError("config line " + std::to_string(line_no) + ": bad escape '\\" +
                            raw[i] + "'");
      }
      keep = value.size();
    } else if (!quoted && (c == '#' || c == ';')) {
      break;
    } else if (!quoted && is_space(c)) {
      if (!value.empty()) value += c;  // leading whitespace is dropped
    } else {
      value += c;
      keep = value.size();
    }
  }
  if (quoted) throw ConfigError("config line " + std::to_string(line_no) + ": unterminated quote");
  value.resize(keep);  // drops unquoted trailing whitespace
  return value;
}

ParsedLine parse_line(std::string_view line, int line_no) {
  ParsedLine parsed;
  const std::string_view text = trim(line);
  if (text.empty() || text.front() == '#' || text.front() == ';') return parsed;

  if (text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string_view::npos) {
      throw ConfigError("config line " + std::to_string(line_no) + ": unterminated section header");
    }
    const std::string_view rest = trim(text.substr(close + 1));
    if (!rest.empty() && rest.front() != '#' && rest.front() != ';') {
      // Git allows "[section] key = value" on one line; we do not.
      throw ConfigError("config line " + std::to_string(line_no) +
                        ": text after a section header is not supported");
    }
    parsed.kind = ParsedLine::Kind::Section;
    parsed.section = lowercase(trim(text.substr(1, close - 1)));
    return parsed;
  }

  std::size_t end = 0;
  while (end < text.size() && is_name_char(text[end])) ++end;
  if (end == 0 || std::isalpha(static_cast<unsigned char>(text.front())) == 0) {
    throw ConfigError("config line " + std::to_string(line_no) + ": invalid key");
  }
  parsed.kind = ParsedLine::Kind::Entry;
  parsed.key = lowercase(text.substr(0, end));
  const std::string_view after = trim(text.substr(end));
  if (after.empty() || after.front() == '#' || after.front() == ';') {
    parsed.value = "true";  // a bare key is a boolean true in Git
  } else if (after.front() == '=') {
    parsed.value = parse_value(after.substr(1), line_no);
  } else {
    throw ConfigError("config line " + std::to_string(line_no) + ": expected '=' after key");
  }
  return parsed;
}

// Encodes a value so that parse_value (and Git) read it back unchanged.
std::string format_value(std::string_view value) {
  std::string out;
  bool needs_quotes = !value.empty() && (is_space(value.front()) || is_space(value.back()));
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '#':
      case ';':
        needs_quotes = true;
        out += c;
        break;
      default: out += c;
    }
  }
  return needs_quotes ? '"' + out + '"' : out;
}

// Splits a validated "section.name" key into lowercase parts.
std::pair<std::string, std::string> split_key(std::string_view key) {
  Config::validate_key(key);
  const auto dot = key.find('.');
  return {lowercase(key.substr(0, dot)), lowercase(key.substr(dot + 1))};
}

}  // namespace

void Config::validate_key(std::string_view key) {
  const auto dot = key.find('.');
  const auto bad = [&](const char* why) {
    throw ConfigError("invalid config key '" + std::string(key) + "': " + why);
  };
  if (dot == std::string_view::npos) bad("expected <section>.<name>");
  if (key.find('.', dot + 1) != std::string_view::npos) bad("subsections are not supported");
  const std::string_view section = key.substr(0, dot);
  const std::string_view name = key.substr(dot + 1);
  if (section.empty() || name.empty()) bad("empty section or name");
  for (const char c : section) {
    if (!is_name_char(c)) bad("section may contain only letters, digits and '-'");
  }
  if (std::isalpha(static_cast<unsigned char>(name.front())) == 0) bad("name must start with a letter");
  for (const char c : name) {
    if (!is_name_char(c)) bad("name may contain only letters, digits and '-'");
  }
}

Config Config::load(const fs::path& file) {
  Config config(file);
  std::error_code ec;
  if (!fs::exists(file, ec)) return config;

  const std::string text = read_file(file);
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    config.lines_.push_back(std::move(line));
    pos = eol + 1;
  }
  // Parse everything once so a malformed file is reported on load.
  for (std::size_t i = 0; i < config.lines_.size(); ++i) {
    parse_line(config.lines_[i], static_cast<int>(i + 1));
  }
  return config;
}

std::optional<std::string> Config::get(std::string_view key) const {
  const auto [section, name] = split_key(key);
  std::optional<std::string> result;
  std::string current;
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const ParsedLine line = parse_line(lines_[i], static_cast<int>(i + 1));
    if (line.kind == ParsedLine::Kind::Section) {
      current = line.section;
    } else if (line.kind == ParsedLine::Kind::Entry && current == section && line.key == name) {
      result = line.value;  // keep going: the last occurrence wins
    }
  }
  return result;
}

void Config::set(std::string_view key, std::string_view value) {
  if (value.find('\0') != std::string_view::npos) {
    throw ConfigError("config values cannot contain a NUL byte");
  }
  const auto [section, name] = split_key(key);
  const std::string entry =
      '\t' + std::string(key.substr(key.find('.') + 1)) + " = " + format_value(value);

  std::optional<std::size_t> last_entry;  // last line defining the key
  std::optional<std::size_t> section_end;  // insertion point after the last matching section
  std::string current;
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const ParsedLine line = parse_line(lines_[i], static_cast<int>(i + 1));
    if (line.kind == ParsedLine::Kind::Section) current = line.section;
    if (current != section) continue;
    if (line.kind == ParsedLine::Kind::Entry && line.key == name) last_entry = i;
    if (line.kind != ParsedLine::Kind::Other) section_end = i + 1;
  }

  if (last_entry) {
    lines_[*last_entry] = entry;
  } else if (section_end) {
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(*section_end), entry);
  } else {
    lines_.push_back('[' + std::string(key.substr(0, key.find('.'))) + ']');
    lines_.push_back(entry);
  }
}

void Config::save() const {
  std::string text;
  for (const auto& line : lines_) text += line + '\n';
  write_file_atomic(file_, text);
}

}  // namespace minigit
