#include "minigit/log.hpp"

#include <array>
#include <ostream>

#include "minigit/refs.hpp"

namespace minigit {
namespace {

struct CivilDate {
  std::int64_t year;
  unsigned month;  // 1-12
  unsigned day;    // 1-31
};

// Inverse of days_from_civil (Howard Hinnant's civil_from_days): turns days
// since 1970-01-01 into a proleptic Gregorian date without gmtime(), whose
// range and thread safety vary between platforms.
CivilDate civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);
  return {y, m, d};
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

// "+0530" -> 19800 seconds. The timezone was validated when it was parsed.
std::int64_t parse_utc_offset(std::string_view tz) {
  if (tz.size() != 5) return 0;
  const int hh = (tz[1] - '0') * 10 + (tz[2] - '0');
  const int mm = (tz[3] - '0') * 10 + (tz[4] - '0');
  const std::int64_t seconds = (hh * 60 + mm) * 60;
  return tz[0] == '-' ? -seconds : seconds;
}

std::string two_digits(std::int64_t n) {
  return {static_cast<char>('0' + n / 10), static_cast<char>('0' + n % 10)};
}

}  // namespace

std::vector<LogEntry> first_parent_log(const ObjectStore& store, const std::string& start,
                                       std::optional<std::size_t> limit) {
  std::vector<LogEntry> entries;
  std::optional<std::string> next = start;
  while (next && (!limit || entries.size() < *limit)) {
    Commit commit = read_commit(store, *next);
    std::optional<std::string> parent;
    if (!commit.parents.empty()) parent = commit.parents.front();
    entries.push_back({std::move(*next), std::move(commit)});
    next = std::move(parent);
  }
  return entries;
}

std::string format_git_date(std::int64_t timestamp, std::string_view timezone) {
  static constexpr std::array<const char*, 7> kDays = {"Sun", "Mon", "Tue", "Wed",
                                                       "Thu", "Fri", "Sat"};
  static constexpr std::array<const char*, 12> kMonths = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  // Git shows the time as it was on the committer's clock, not ours.
  const std::int64_t local = timestamp + parse_utc_offset(timezone);
  const std::int64_t days = floor_div(local, 86400);
  const std::int64_t secs = local - days * 86400;
  const CivilDate date = civil_from_days(days);
  const std::int64_t weekday = ((days + 4) % 7 + 7) % 7;  // 1970-01-01 was a Thursday

  return std::string(kDays[weekday]) + ' ' + kMonths[date.month - 1] + ' ' +
         std::to_string(date.day) + ' ' + two_digits(secs / 3600) + ':' +
         two_digits(secs / 60 % 60) + ':' + two_digits(secs % 60) + ' ' +
         std::to_string(date.year) + ' ' + std::string(timezone);
}

void print_log(std::ostream& out, const std::vector<LogEntry>& entries) {
  bool first = true;
  for (const auto& [oid, commit] : entries) {
    if (!first) out << '\n';
    first = false;
    out << "commit " << oid << '\n';
    if (commit.parents.size() > 1) {
      out << "Merge:";
      for (const auto& parent : commit.parents) out << ' ' << parent.substr(0, 7);
      out << '\n';
    }
    out << "Author: " << commit.author.name << " <" << commit.author.email << ">\n";
    out << "Date:   " << format_git_date(commit.author.timestamp, commit.author.timezone)
        << "\n\n";

    std::string_view message = commit.message;
    while (!message.empty() && message.back() == '\n') message.remove_suffix(1);
    std::size_t pos = 0;
    while (pos <= message.size()) {
      auto eol = message.find('\n', pos);
      if (eol == std::string_view::npos) eol = message.size();
      out << "    " << message.substr(pos, eol - pos) << '\n';
      pos = eol + 1;
    }
  }
}

}  // namespace minigit
