#include "sensor_logic.h"

#include <cstring>
#include <utility>

namespace thermostat::sensor {
namespace {

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kSecondsPerHour = 3600;
constexpr int64_t kSecondsPerMinute = 60;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Reads exactly `count` ASCII digits and advances the cursor.
bool read_fixed_digits(const char*& cursor, int count, int* out) {
  int value = 0;
  for (int i = 0; i < count; ++i) {
    if (!is_digit(cursor[i])) {
      return false;
    }
    value = value * 10 + (cursor[i] - '0');
  }
  *out = value;
  cursor += count;
  return true;
}

bool is_leap_year(int64_t year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int64_t year, int month) {
  static const int kDaysPerMonth[12] = {31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kDaysPerMonth[month - 1];
}

// Howard Hinnant's days_from_civil: days since 1970-01-01 for a civil date.
// Works for any proleptic Gregorian year without consulting the host timezone.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= static_cast<int64_t>(m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153U * (m > 2 ? m - 3 : m + 9) + 2U) / 5U + d - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool is_plausible_temp(int16_t centi) {
  return centi != kInvalidTempCenti && centi >= kMinPlausibleTempCenti &&
         centi <= kMaxPlausibleTempCenti;
}

std::string ensure_sensor_suffix(const std::string& name) {
  static const std::string kSuffix = "센서";
  if (name.size() >= kSuffix.size() &&
      name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) ==
          0) {
    return name;
  }
  return name + " 센서";
}

// "최근 N분 전" for the averaged label, but plain "방금" for a fresh value.
std::string recent_age_text(int64_t age_sec) {
  const std::string formatted = format_age_korean(age_sec);
  if (formatted == "방금") {
    return formatted;
  }
  return "최근 " + formatted;
}

int16_t apply_board_offset(int16_t temp_centi, int16_t offset_centi) {
  int32_t value = static_cast<int32_t>(temp_centi) + offset_centi;
  if (value > 32767) {
    value = 32767;
  } else if (value < -32768) {
    value = -32768;
  }
  return static_cast<int16_t>(value);
}

void append_utf8(uint32_t code_point, std::string* out) {
  if (code_point <= 0x7FU) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    out->push_back(static_cast<char>(0xC0U | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    out->push_back(static_cast<char>(0xE0U | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    out->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out->push_back(static_cast<char>(0xF0U | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
    out->push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
    out->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

// Minimal recursive-descent JSON reader. It never allocates more than the
// string it is given and bounds nesting depth, so corrupted or hostile
// responses are rejected instead of crashing.
class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  bool at_end() const { return pos_ >= text_.size(); }

  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

  size_t position() const { return pos_; }

  void set_position(size_t position) { pos_ = position; }

  void skip_whitespace() {
    while (!at_end()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        break;
      }
      ++pos_;
    }
  }

  bool consume(char expected) {
    if (peek() != expected) {
      return false;
    }
    ++pos_;
    return true;
  }

  bool parse_literal(const char* literal) {
    const size_t length = std::strlen(literal);
    if (pos_ > text_.size() || text_.compare(pos_, length, literal) != 0) {
      return false;
    }
    pos_ += length;
    return true;
  }

  // Parses a JSON true/false literal. Returns false (position unspecified) for
  // any other value; callers that tolerate non-bool values rewind to a saved
  // mark and skip_value instead (same pattern as parse_temperature).
  bool parse_bool(bool* out) {
    skip_whitespace();
    if (parse_literal("true")) {
      *out = true;
      return true;
    }
    if (parse_literal("false")) {
      *out = false;
      return true;
    }
    return false;
  }

  bool parse_string(std::string* out) {
    if (!consume('"')) {
      return false;
    }
    out->clear();
    while (!at_end()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        return false;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (at_end()) {
        return false;
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t code_point = 0;
          if (!parse_hex4(&code_point)) {
            return false;
          }
          if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            // Combine a UTF-16 surrogate pair into one code point.
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u') {
              return false;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!parse_hex4(&low)) {
              return false;
            }
            if (low < 0xDC00U || low > 0xDFFFU) {
              return false;
            }
            code_point =
                0x10000U + ((code_point - 0xD800U) << 10) + (low - 0xDC00U);
          } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            return false;
          }
          append_utf8(code_point, out);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool skip_value(int depth = 0) {
    if (depth > 32) {
      return false;
    }
    skip_whitespace();
    const char c = peek();
    if (c == '"') {
      std::string discarded;
      return parse_string(&discarded);
    }
    if (c == '{') {
      ++pos_;
      skip_whitespace();
      if (consume('}')) {
        return true;
      }
      while (true) {
        skip_whitespace();
        std::string key;
        if (!parse_string(&key)) {
          return false;
        }
        skip_whitespace();
        if (!consume(':')) {
          return false;
        }
        if (!skip_value(depth + 1)) {
          return false;
        }
        skip_whitespace();
        if (consume(',')) {
          continue;
        }
        return consume('}');
      }
    }
    if (c == '[') {
      ++pos_;
      skip_whitespace();
      if (consume(']')) {
        return true;
      }
      while (true) {
        if (!skip_value(depth + 1)) {
          return false;
        }
        skip_whitespace();
        if (consume(',')) {
          continue;
        }
        return consume(']');
      }
    }
    if (c == 't') {
      return parse_literal("true");
    }
    if (c == 'f') {
      return parse_literal("false");
    }
    if (c == 'n') {
      return parse_literal("null");
    }
    if (c == '-' || is_digit(c)) {
      return skip_number();
    }
    return false;
  }

  // Parses a JSON number and converts it to centi-degC without floating
  // point, rounding half away from zero at the hundredth of a degree.
  // Returns false (position unspecified) on a malformed or absurd value.
  bool parse_temperature(int16_t* out_centi) {
    skip_whitespace();
    bool negative = false;
    if (peek() == '-') {
      negative = true;
      ++pos_;
    }
    if (!is_digit(peek())) {
      return false;
    }
    int64_t mantissa = 0;
    int integer_digits = 0;
    while (is_digit(peek())) {
      if (integer_digits >= 9) {
        return false;
      }
      mantissa = mantissa * 10 + (peek() - '0');
      ++integer_digits;
      ++pos_;
    }
    int fraction_digits = 0;
    if (peek() == '.') {
      ++pos_;
      if (!is_digit(peek())) {
        return false;
      }
      while (is_digit(peek())) {
        if (fraction_digits < 9) {
          mantissa = mantissa * 10 + (peek() - '0');
          ++fraction_digits;
        }
        ++pos_;
      }
    }
    int exponent = 0;
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      int sign = 1;
      if (peek() == '+') {
        ++pos_;
      } else if (peek() == '-') {
        sign = -1;
        ++pos_;
      }
      if (!is_digit(peek())) {
        return false;
      }
      int magnitude = 0;
      while (is_digit(peek())) {
        magnitude = magnitude * 10 + (peek() - '0');
        if (magnitude > 1000) {
          return false;
        }
        ++pos_;
      }
      exponent = sign * magnitude;
    }
    // centi = mantissa * 10^(exponent - fraction_digits + 2)
    const int power = exponent - fraction_digits + 2;
    int64_t centi = 0;
    if (power >= 0) {
      if (power > 6) {
        return false;
      }
      for (int i = 0; i < power; ++i) {
        mantissa *= 10;
        if (mantissa > 1000000000LL) {
          return false;
        }
      }
      centi = mantissa;
    } else {
      const int divide_power = -power;
      if (divide_power > 6) {
        centi = 0;
      } else {
        int64_t divisor = 1;
        for (int i = 0; i < divide_power; ++i) {
          divisor *= 10;
        }
        centi = (mantissa + divisor / 2) / divisor;
      }
    }
    if (negative) {
      centi = -centi;
    }
    // Anything beyond this is not a plausible indoor temperature and will be
    // rejected again by the plausibility checks; reject it here as garbage.
    if (centi < -32000 || centi > 32000) {
      return false;
    }
    *out_centi = static_cast<int16_t>(centi);
    return true;
  }

 private:
  bool parse_hex4(uint32_t* out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      if (at_end()) {
        return false;
      }
      const char c = text_[pos_++];
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      value = value * 16U + static_cast<uint32_t>(digit);
    }
    *out = value;
    return true;
  }

  bool skip_number() {
    const size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (peek() == '0') {
      ++pos_;
    } else if (is_digit(peek())) {
      while (is_digit(peek())) {
        ++pos_;
      }
    } else {
      pos_ = start;
      return false;
    }
    if (peek() == '.') {
      ++pos_;
      if (!is_digit(peek())) {
        pos_ = start;
        return false;
      }
      while (is_digit(peek())) {
        ++pos_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      if (!is_digit(peek())) {
        pos_ = start;
        return false;
      }
      while (is_digit(peek())) {
        ++pos_;
      }
    }
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
};

bool parse_sensor_object(JsonReader& reader,
                         std::vector<RemoteSensorReading>* out) {
  reader.skip_whitespace();
  if (!reader.consume('{')) {
    return false;
  }
  bool has_id = false;
  bool has_name = false;
  bool has_temperature = false;
  bool has_updated_at = false;
  bool has_online = false;
  bool has_checked_at = false;
  RemoteSensorReading reading;
  reader.skip_whitespace();
  if (!reader.consume('}')) {
    while (true) {
      reader.skip_whitespace();
      std::string key;
      if (!reader.parse_string(&key)) {
        return false;
      }
      reader.skip_whitespace();
      if (!reader.consume(':')) {
        return false;
      }
      reader.skip_whitespace();
      if (key == "id") {
        if (!reader.parse_string(&reading.id)) {
          return false;
        }
        has_id = true;
      } else if (key == "name") {
        if (!reader.parse_string(&reading.name)) {
          return false;
        }
        has_name = true;
      } else if (key == "temperature") {
        const size_t mark = reader.position();
        int16_t centi = kInvalidTempCenti;
        if (reader.parse_temperature(&centi)) {
          reading.temp_centi = centi;
          has_temperature = true;
        } else {
          reader.set_position(mark);
          if (!reader.skip_value()) {
            return false;
          }
        }
      } else if (key == "updated_at") {
        std::string updated_at;
        if (!reader.parse_string(&updated_at)) {
          return false;
        }
        int64_t epoch_sec = 0;
        if (parse_iso8601_to_epoch(updated_at.c_str(), &epoch_sec)) {
          reading.updated_epoch_sec = epoch_sec;
          has_updated_at = true;
        }
      } else if (key == "online") {
        // Optional field: only a true/false literal counts. null, strings and
        // numbers are skipped and leave the reading with no health information,
        // but never discard the entry.
        const size_t mark = reader.position();
        bool online = false;
        if (reader.parse_bool(&online)) {
          reading.online = online;
          has_online = true;
        } else {
          reader.set_position(mark);
          if (!reader.skip_value()) {
            return false;
          }
        }
      } else if (key == "checked_at") {
        // Optional field: only a string with a timezone offset counts. A
        // non-string value is skipped; an unparsable timestamp leaves the
        // reading with no health information. Either way the entry survives.
        const size_t mark = reader.position();
        std::string checked_at;
        if (!reader.parse_string(&checked_at)) {
          reader.set_position(mark);
          if (!reader.skip_value()) {
            return false;
          }
        } else {
          int64_t epoch_sec = 0;
          if (parse_iso8601_to_epoch(checked_at.c_str(), &epoch_sec)) {
            reading.checked_epoch_sec = epoch_sec;
            has_checked_at = true;
          }
        }
      } else {
        if (!reader.skip_value()) {
          return false;
        }
      }
      reader.skip_whitespace();
      if (reader.consume(',')) {
        continue;
      }
      if (reader.consume('}')) {
        break;
      }
      return false;
    }
  }
  // Both health fields must be usable; a half-filled pair is treated as "no
  // online information" and the selector keeps its previous behaviour.
  reading.online_known = has_online && has_checked_at;
  const bool complete = has_id && has_name && !reading.id.empty() &&
                        !reading.name.empty() && has_temperature &&
                        has_updated_at;
  if (complete && out->size() < kMaxRemoteSensors) {
    out->push_back(std::move(reading));
  }
  return true;
}

bool parse_sensors_array(JsonReader& reader,
                         std::vector<RemoteSensorReading>* out) {
  reader.skip_whitespace();
  if (!reader.consume('[')) {
    return false;
  }
  reader.skip_whitespace();
  if (reader.consume(']')) {
    return true;
  }
  while (true) {
    if (!parse_sensor_object(reader, out)) {
      return false;
    }
    reader.skip_whitespace();
    if (reader.consume(',')) {
      reader.skip_whitespace();
      continue;
    }
    return reader.consume(']');
  }
}

}  // namespace

std::string format_age_korean(int64_t age_sec) {
  if (age_sec < kSecondsPerMinute) {
    return "방금";
  }
  return std::to_string(age_sec / kSecondsPerMinute) + "분 전";
}

bool TempFilter::push(int16_t raw_centi) {
  if (!is_plausible_temp(raw_centi)) {
    spike_streak_ = 0;
    return false;
  }
  if (has_last_accepted_) {
    const int32_t delta =
        static_cast<int32_t>(raw_centi) - static_cast<int32_t>(last_accepted_centi_);
    if (delta > kMaxStepDeltaCenti || delta < -kMaxStepDeltaCenti) {
      ++spike_streak_;
      if (spike_streak_ < 3) {
        return false;
      }
      // Three consecutive jumps: treat it as a real change and restart the
      // window from the new level.
      count_ = 0;
      next_index_ = 0;
      spike_streak_ = 0;
    } else {
      spike_streak_ = 0;
    }
  } else {
    spike_streak_ = 0;
  }
  window_[next_index_] = raw_centi;
  next_index_ = (next_index_ + 1) % kFilterWindowSize;
  if (count_ < kFilterWindowSize) {
    ++count_;
  }
  last_accepted_centi_ = raw_centi;
  has_last_accepted_ = true;
  return true;
}

bool TempFilter::has_value() const { return count_ > 0; }

int16_t TempFilter::filtered_centi() const {
  if (count_ == 0) {
    return kInvalidTempCenti;
  }
  int32_t sum = 0;
  for (size_t i = 0; i < count_; ++i) {
    sum += window_[i];
  }
  const int32_t count = static_cast<int32_t>(count_);
  // Round half away from zero.
  return static_cast<int16_t>(sum >= 0 ? (sum + count / 2) / count
                                       : (sum - count / 2) / count);
}

void TempFilter::reset() {
  count_ = 0;
  next_index_ = 0;
  last_accepted_centi_ = 0;
  has_last_accepted_ = false;
  spike_streak_ = 0;
}

bool parse_iso8601_to_epoch(const char* iso, int64_t* out_epoch_sec) {
  if (iso == nullptr || out_epoch_sec == nullptr) {
    return false;
  }
  const char* cursor = iso;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!read_fixed_digits(cursor, 4, &year) || *cursor != '-') {
    return false;
  }
  ++cursor;
  if (!read_fixed_digits(cursor, 2, &month) || *cursor != '-') {
    return false;
  }
  ++cursor;
  if (!read_fixed_digits(cursor, 2, &day) || *cursor != 'T') {
    return false;
  }
  ++cursor;
  if (!read_fixed_digits(cursor, 2, &hour) || *cursor != ':') {
    return false;
  }
  ++cursor;
  if (!read_fixed_digits(cursor, 2, &minute) || *cursor != ':') {
    return false;
  }
  ++cursor;
  if (!read_fixed_digits(cursor, 2, &second)) {
    return false;
  }
  if (*cursor == '.') {
    ++cursor;
    if (!is_digit(*cursor)) {
      return false;
    }
    while (is_digit(*cursor)) {
      ++cursor;
    }
  }
  int64_t offset_seconds = 0;
  if (*cursor == 'Z' || *cursor == 'z') {
    ++cursor;
  } else if (*cursor == '+' || *cursor == '-') {
    const int offset_sign = (*cursor == '+') ? 1 : -1;
    ++cursor;
    int offset_hour = 0;
    int offset_minute = 0;
    if (!read_fixed_digits(cursor, 2, &offset_hour) || *cursor != ':') {
      return false;
    }
    ++cursor;
    if (!read_fixed_digits(cursor, 2, &offset_minute)) {
      return false;
    }
    if (offset_hour > 23 || offset_minute > 59) {
      return false;
    }
    offset_seconds =
        offset_sign * (offset_hour * kSecondsPerHour +
                       offset_minute * kSecondsPerMinute);
  } else {
    // The home-server API always includes a timezone; refuse to guess.
    return false;
  }
  if (*cursor != '\0') {
    return false;
  }
  if (month < 1 || month > 12) {
    return false;
  }
  if (day < 1 || day > days_in_month(year, month)) {
    return false;
  }
  if (hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  const int64_t days = days_from_civil(year, static_cast<unsigned>(month),
                                       static_cast<unsigned>(day));
  *out_epoch_sec = days * kSecondsPerDay + hour * kSecondsPerHour +
                   minute * kSecondsPerMinute + second - offset_seconds;
  return true;
}

bool parse_homeserver_sensors_json(const std::string& json,
                                   std::vector<RemoteSensorReading>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  JsonReader reader(json);
  reader.skip_whitespace();
  if (!reader.consume('{')) {
    return false;
  }
  bool found_sensors = false;
  reader.skip_whitespace();
  if (reader.consume('}')) {
    return false;
  }
  while (true) {
    reader.skip_whitespace();
    std::string key;
    if (!reader.parse_string(&key)) {
      return false;
    }
    reader.skip_whitespace();
    if (!reader.consume(':')) {
      return false;
    }
    reader.skip_whitespace();
    if (key == "sensors") {
      if (found_sensors) {
        return false;
      }
      found_sensors = true;
      if (!parse_sensors_array(reader, out)) {
        return false;
      }
    } else {
      if (!reader.skip_value()) {
        return false;
      }
    }
    reader.skip_whitespace();
    if (reader.consume(',')) {
      continue;
    }
    if (!reader.consume('}')) {
      return false;
    }
    break;
  }
  reader.skip_whitespace();
  if (!reader.at_end()) {
    return false;
  }
  return found_sensors;
}

bool parse_homeserver_time_json(const std::string& json,
                                int64_t* out_epoch_sec) {
  if (out_epoch_sec == nullptr) {
    return false;
  }
  JsonReader reader(json);
  reader.skip_whitespace();
  if (!reader.consume('{')) {
    return false;
  }
  bool found_now = false;
  int64_t parsed_epoch_sec = 0;
  reader.skip_whitespace();
  if (reader.consume('}')) {
    // "now" is required.
    return false;
  }
  while (true) {
    reader.skip_whitespace();
    std::string key;
    if (!reader.parse_string(&key)) {
      return false;
    }
    reader.skip_whitespace();
    if (!reader.consume(':')) {
      return false;
    }
    reader.skip_whitespace();
    if (key == "now") {
      if (found_now) {
        return false;
      }
      found_now = true;
      std::string now_text;
      if (!reader.parse_string(&now_text)) {
        return false;
      }
      // The home-server API always includes a timezone offset; refuse to guess
      // one from the board's local time.
      if (!parse_iso8601_to_epoch(now_text.c_str(), &parsed_epoch_sec)) {
        return false;
      }
    } else {
      // "epoch" and any future fields are skipped; the ISO string is the
      // authoritative value.
      if (!reader.skip_value()) {
        return false;
      }
    }
    reader.skip_whitespace();
    if (reader.consume(',')) {
      continue;
    }
    if (reader.consume('}')) {
      break;
    }
    return false;
  }
  reader.skip_whitespace();
  if (!reader.at_end() || !found_now) {
    return false;
  }
  *out_epoch_sec = parsed_epoch_sec;
  return true;
}

void SensorSelector::set_config(const SensorSelectionConfig& cfg) {
  config_ = cfg;
}

const SensorSelectionConfig& SensorSelector::config() const { return config_; }

void SensorSelector::update_board_temp(int16_t raw_centi,
                                       int64_t now_epoch_sec) {
  if (!is_plausible_temp(raw_centi)) {
    // The local sensor is read directly; an invalid sample is a board fault,
    // not a remote-style transient gap.
    board_filter_.reset();
    board_updated_epoch_sec_ = 0;
    return;
  }
  if (board_filter_.push(raw_centi)) {
    board_updated_epoch_sec_ = now_epoch_sec;
  }
}

void SensorSelector::record_remote_success(
    const std::vector<RemoteSensorReading>& readings, int64_t now_epoch_sec) {
  // now_epoch_sec is intentionally unused: the freshness of a remote value is
  // defined by the sensor's own updated_at, not by the fetch time.
  static_cast<void>(now_epoch_sec);
  last_readings_ = readings;
  consecutive_failures_ = 0;
}

void SensorSelector::record_remote_failure() {
  if (consecutive_failures_ < 255) {
    ++consecutive_failures_;
  }
}

uint8_t SensorSelector::consecutive_failures() const {
  return consecutive_failures_;
}

ResolvedTemperature SensorSelector::fault_result(bool tried_board_fallback) const {
  ResolvedTemperature out;
  out.valid = false;
  out.temp_centi = kInvalidTempCenti;
  out.used_fallback_board = tried_board_fallback;
  out.freshness = Freshness::kFault;
  out.age_sec = 0;
  out.source_label = "센서 오류 · 온도 확인 불가";
  return out;
}

bool SensorSelector::board_value(int64_t now_epoch_sec, int16_t* out_temp_centi,
                                 int64_t* out_age_sec) const {
  if (!board_filter_.has_value()) {
    return false;
  }
  int64_t age_sec = now_epoch_sec - board_updated_epoch_sec_;
  if (age_sec < 0) {
    age_sec = 0;
  }
  if (age_sec > kStaleAgeSec) {
    return false;
  }
  *out_temp_centi =
      apply_board_offset(board_filter_.filtered_centi(), config_.board_offset_centi);
  *out_age_sec = age_sec;
  return true;
}

ResolvedTemperature SensorSelector::resolve_board_only(
    int64_t now_epoch_sec) const {
  int16_t temp_centi = kInvalidTempCenti;
  int64_t age_sec = 0;
  if (!board_value(now_epoch_sec, &temp_centi, &age_sec)) {
    return fault_result(false);
  }
  ResolvedTemperature out;
  out.valid = true;
  out.temp_centi = temp_centi;
  out.age_sec = age_sec;
  out.freshness = age_sec > kWarnAgeSec ? Freshness::kWarn : Freshness::kFresh;
  out.source_label = "보드 센서 · " + format_age_korean(age_sec);
  return out;
}

ResolvedTemperature SensorSelector::resolve_board_fallback(
    int64_t now_epoch_sec, const char* reason) const {
  int16_t temp_centi = kInvalidTempCenti;
  int64_t age_sec = 0;
  if (!board_value(now_epoch_sec, &temp_centi, &age_sec)) {
    // The board sensor is the last resort; without it there is no safe
    // temperature reading at all.
    return fault_result(true);
  }
  ResolvedTemperature out;
  out.valid = true;
  out.temp_centi = temp_centi;
  out.used_fallback_board = true;
  out.freshness = Freshness::kStaleOrFallback;
  out.age_sec = age_sec;
  out.source_label = std::string("보드 센서 · ") + reason;
  return out;
}

ResolvedTemperature SensorSelector::resolve_remote(int64_t now_epoch_sec) const {
  if (consecutive_failures_ >= kMaxConsecutiveFailures) {
    return resolve_board_fallback(now_epoch_sec, "원격 센서 응답 없음");
  }
  if (config_.selected_remote_ids.empty()) {
    return resolve_board_fallback(now_epoch_sec, "원격 센서 응답 없음");
  }

  struct Candidate {
    const RemoteSensorReading* reading;
    int64_t age_sec;
    bool online_confirmed;
  };
  std::vector<Candidate> candidates;
  bool found_any_reading = false;
  for (const std::string& id : config_.selected_remote_ids) {
    const RemoteSensorReading* match = nullptr;
    for (const RemoteSensorReading& reading : last_readings_) {
      if (reading.id == id) {
        match = &reading;
        break;
      }
    }
    if (match == nullptr || !is_plausible_temp(match->temp_centi)) {
      continue;
    }
    found_any_reading = true;
    int64_t age_sec = now_epoch_sec - match->updated_epoch_sec;
    if (age_sec < 0) {
      age_sec = 0;
    }
    bool online_confirmed = false;
    if (age_sec > kStaleAgeSec) {
      // A stale report is still usable when the home server checked the device
      // health recently; the report-age cap guards a value that is stuck while
      // the device stays online.
      if (!match->online_known || !match->online) {
        continue;
      }
      int64_t checked_age_sec = now_epoch_sec - match->checked_epoch_sec;
      if (checked_age_sec < 0) {
        checked_age_sec = 0;
      }
      if (checked_age_sec > kStaleAgeSec ||
          age_sec > kOnlineMaxReportAgeSec) {
        continue;
      }
      online_confirmed = true;
    }
    candidates.push_back(Candidate{match, age_sec, online_confirmed});
    if (config_.mode == SourceMode::kSingleRemote) {
      break;
    }
  }

  if (candidates.empty()) {
    return resolve_board_fallback(
        now_epoch_sec,
        found_any_reading ? "원격 센서 값 오래됨" : "원격 센서 응답 없음");
  }

  int64_t max_age_sec = 0;
  int32_t sum_centi = 0;
  bool any_warning_age = false;
  for (const Candidate& candidate : candidates) {
    if (candidate.age_sec > max_age_sec) {
      max_age_sec = candidate.age_sec;
    }
    // A reading approved by the online check is expected to be old (the sensor
    // only reports on change) and does not get the warning colour.
    if (!candidate.online_confirmed && candidate.age_sec > kWarnAgeSec) {
      any_warning_age = true;
    }
    sum_centi += candidate.reading->temp_centi;
  }
  const int32_t count = static_cast<int32_t>(candidates.size());
  const int16_t average_centi = static_cast<int16_t>(
      sum_centi >= 0 ? (sum_centi + count / 2) / count
                     : (sum_centi - count / 2) / count);

  ResolvedTemperature out;
  out.valid = true;
  out.temp_centi = average_centi;
  out.used_fallback_board = false;
  out.freshness = any_warning_age ? Freshness::kWarn : Freshness::kFresh;
  out.age_sec = max_age_sec;
  if (config_.mode == SourceMode::kSingleRemote) {
    out.source_label = ensure_sensor_suffix(candidates.front().reading->name) +
                       " · " + format_age_korean(max_age_sec);
  } else {
    std::string names;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i != 0) {
        names += ", ";
      }
      names += candidates[i].reading->name;
    }
    out.source_label =
        "평균(" + names + ") · " + recent_age_text(max_age_sec);
  }
  return out;
}

ResolvedTemperature SensorSelector::resolve(int64_t now_epoch_sec) const {
  switch (config_.mode) {
    case SourceMode::kBoardOnly:
      return resolve_board_only(now_epoch_sec);
    case SourceMode::kSingleRemote:
    case SourceMode::kAverageRemote:
      return resolve_remote(now_epoch_sec);
  }
  return resolve_board_only(now_epoch_sec);
}

}  // namespace thermostat::sensor
