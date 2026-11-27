#pragma once

// Pure C++17 sensor decision logic: plausibility filtering, home-server JSON
// parsing, sensor selection/averaging and automatic fallback to the board
// sensor.
//
// No Arduino, hardware or third-party JSON dependency is allowed here:
// tools/test_host.py compiles this with g++ -std=c++17 -Wall -Wextra -Werror.
// Temperatures are int16_t centi-degC (0.01 degC, 2150 == 21.50 degC) matching
// the Matter temperature unit; absolute times are int64_t Unix UTC seconds.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thermostat::sensor {

// Maximum number of remote sensors kept from one home-server response.
constexpr size_t kMaxRemoteSensors = 8;

// Moving-average window length for local temperature filtering.
constexpr size_t kFilterWindowSize = 5;

// Invalid temperature sentinel (INT16_MIN), shared with the Matter layer.
constexpr int16_t kInvalidTempCenti = -32768;

// Plausible indoor range: -10.0 degC .. 60.0 degC.
constexpr int16_t kMinPlausibleTempCenti = -1000;
constexpr int16_t kMaxPlausibleTempCenti = 6000;

// A single sample that jumps more than 5.0 degC is treated as a glitch.
constexpr int16_t kMaxStepDeltaCenti = 500;

// Age thresholds for the UI temperature-source badge.
constexpr int64_t kWarnAgeSec = 300;   // > 5 min: warning colour
constexpr int64_t kStaleAgeSec = 600;  // > 10 min: fall back to the board sensor

// A remote value whose last report is older than kStaleAgeSec is still used
// when the home server confirmed the device online within kStaleAgeSec and
// the report is not older than this cap (guards a stuck temperature value).
constexpr int64_t kOnlineMaxReportAgeSec = 12 * 3600;

// Consecutive home-server failures that trigger the board sensor fallback.
constexpr uint8_t kMaxConsecutiveFailures = 3;

// Formats an elapsed time the way the Korean UI shows it:
//   < 60 s -> "방금", otherwise "<N>분 전" (N = age_sec / 60).
std::string format_age_korean(int64_t age_sec);

// Moving-average filter with range and step plausibility checks.
class TempFilter {
 public:
  // Accepts a raw centi-degC sample and returns true when it entered the
  // window. Out-of-range samples are rejected. A single in-range sample that
  // jumps more than kMaxStepDeltaCenti from the last accepted value is
  // rejected; three consecutive such samples are accepted as a real
  // environment change (the window is reset first so the filter cannot stick
  // to the old value forever).
  bool push(int16_t raw_centi);
  bool has_value() const;
  // Rounded mean of the samples currently in the window, or
  // kInvalidTempCenti when no sample has been accepted yet.
  int16_t filtered_centi() const;
  void reset();

 private:
  int16_t window_[kFilterWindowSize] = {};
  size_t count_ = 0;
  size_t next_index_ = 0;
  int16_t last_accepted_centi_ = 0;
  bool has_last_accepted_ = false;
  uint8_t spike_streak_ = 0;
};

// Parses an ISO-8601 timestamp into Unix UTC seconds without relying on the
// host timezone (no mktime; Howard Hinnant civil-calendar arithmetic).
// Accepted forms: "2026-11-24T21:15:00+09:00", "2026-11-24T12:15:00Z",
// optional fractional seconds (".123") and "-05:00" offsets. On failure the
// output value is left untouched and false is returned.
bool parse_iso8601_to_epoch(const char* iso, int64_t* out_epoch_sec);

struct RemoteSensorReading {
  std::string id;
  std::string name;
  int16_t temp_centi = kInvalidTempCenti;
  int64_t updated_epoch_sec = 0;
  // Optional home-server health fields. online_known is true only when both
  // "online" (a JSON true/false) and "checked_at" (an ISO-8601 timestamp with
  // offset) were parsed. Missing or malformed fields leave online_known false
  // and never discard the entry.
  bool online_known = false;
  bool online = false;
  int64_t checked_epoch_sec = 0;
};

// Parses the home-server response documented in CLAUDE.md section 10:
//   { "sensors": [ { "id": ..., "name": ..., "temperature": ...,
//                    "updated_at": "...", "online": true,
//                    "checked_at": "..." } ] }
// *out is cleared first. Structural JSON errors return false. Entries with a
// missing/invalid required field are skipped. The optional "online" and
// "checked_at" fields never make an entry incomplete. At most kMaxRemoteSensors
// entries are kept. A top-level object without a "sensors" array is invalid.
bool parse_homeserver_sensors_json(const std::string& json,
                                   std::vector<RemoteSensorReading>* out);

// Parses GET /api/thermostat/time:
//   { "now": "<ISO-8601 with offset>", "epoch": <int> }
// Uses "now" (parse_iso8601_to_epoch); the integer "epoch" field is only
// skipped so the wall-clock string is the single source of truth. Unknown keys
// are skipped as well. Returns false on structural errors or when "now" is
// missing/invalid; *out_epoch_sec is left untouched then.
bool parse_homeserver_time_json(const std::string& json, int64_t* out_epoch_sec);

enum class SourceMode : uint8_t {
  kBoardOnly = 0,
  kSingleRemote = 1,
  kAverageRemote = 2,
};

enum class Freshness : uint8_t {
  kFresh = 0,
  kWarn = 1,
  kStaleOrFallback = 2,
  kFault = 3,
};

struct SensorSelectionConfig {
  SourceMode mode = SourceMode::kBoardOnly;
  // Remote sensor ids in UI order. kSingleRemote uses the first id that has a
  // usable reading; kAverageRemote averages every usable id.
  std::vector<std::string> selected_remote_ids;
  // Calibration offset applied to the board sensor, in centi-degC.
  int16_t board_offset_centi = 0;
};

struct ResolvedTemperature {
  bool valid = false;
  int16_t temp_centi = kInvalidTempCenti;
  bool used_fallback_board = false;
  Freshness freshness = Freshness::kFault;
  int64_t age_sec = 0;
  // Korean UI label, e.g. "거실 센서 · 3분 전",
  // "평균(거실, 안방) · 최근 2분 전",
  // "보드 센서 · 원격 센서 응답 없음".
  std::string source_label;
};

// Chooses the temperature source, filters the board sensor and applies the
// automatic fallback rules from CLAUDE.md section 10/11:
//   * remote values older than kStaleAgeSec are excluded unless the home
//     server confirmed the device online within kStaleAgeSec and the report
//     is not older than kOnlineMaxReportAgeSec (all excluded -> board),
//   * kMaxConsecutiveFailures consecutive fetch failures -> board,
//   * a reading approved by the online check does not trigger the warning
//     colour; every other reading older than kWarnAgeSec does,
//   * if the board sensor is also unavailable the result is invalid/kFault.
class SensorSelector {
 public:
  void set_config(const SensorSelectionConfig& cfg);
  const SensorSelectionConfig& config() const;

  // Feeds one board-sensor sample. The sample is moving-average filtered and
  // timestamped with now_epoch_sec. An invalid/implausible sample marks the
  // board sensor faulted immediately (the local sensor is read directly, so a
  // bad sample is not a transient remote glitch).
  void update_board_temp(int16_t raw_centi, int64_t now_epoch_sec);

  // Stores the latest successful home-server response and clears the failure
  // counter. now_epoch_sec is reserved for diagnostics; per-reading ages come
  // from updated_epoch_sec.
  void record_remote_success(const std::vector<RemoteSensorReading>& readings,
                             int64_t now_epoch_sec);

  // Increments the consecutive home-server failure counter. Stored readings
  // are kept so 1..2 failures still resolve to the previous remote value.
  void record_remote_failure();

  uint8_t consecutive_failures() const;

  ResolvedTemperature resolve(int64_t now_epoch_sec) const;

 private:
  bool board_value(int64_t now_epoch_sec, int16_t* out_temp_centi,
                   int64_t* out_age_sec) const;
  ResolvedTemperature resolve_board_only(int64_t now_epoch_sec) const;
  ResolvedTemperature resolve_board_fallback(int64_t now_epoch_sec,
                                             const char* reason) const;
  ResolvedTemperature resolve_remote(int64_t now_epoch_sec) const;
  ResolvedTemperature fault_result(bool tried_board_fallback) const;

  SensorSelectionConfig config_;
  TempFilter board_filter_;
  int64_t board_updated_epoch_sec_ = 0;
  std::vector<RemoteSensorReading> last_readings_;
  uint8_t consecutive_failures_ = 0;
};

}  // namespace thermostat::sensor
