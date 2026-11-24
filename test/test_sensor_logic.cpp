// LINK: src/sensor/sensor_logic.cpp
#include "sensor/sensor_logic.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "test_harness.h"

namespace {

using thermostat::sensor::Freshness;
using thermostat::sensor::RemoteSensorReading;
using thermostat::sensor::ResolvedTemperature;
using thermostat::sensor::SensorSelectionConfig;
using thermostat::sensor::SensorSelector;
using thermostat::sensor::SourceMode;
using thermostat::sensor::TempFilter;
using thermostat::sensor::kInvalidTempCenti;

// Fixed reference time so every test is independent of the host clock.
constexpr int64_t kNow = 1795524000;

RemoteSensorReading make_reading(const std::string& id,
                                 const std::string& name, int16_t temp_centi,
                                 int64_t updated_epoch_sec) {
  RemoteSensorReading reading;
  reading.id = id;
  reading.name = name;
  reading.temp_centi = temp_centi;
  reading.updated_epoch_sec = updated_epoch_sec;
  return reading;
}

SensorSelectionConfig make_remote_config(
    SourceMode mode, std::initializer_list<const char*> ids) {
  SensorSelectionConfig config;
  config.mode = mode;
  for (const char* id : ids) {
    config.selected_remote_ids.emplace_back(id);
  }
  return config;
}

}  // namespace

// --- TempFilter -----------------------------------------------------------

TEST_CASE(temp_filter_computes_moving_average) {
  TempFilter filter;
  EXPECT_FALSE(filter.has_value());
  EXPECT_EQ(kInvalidTempCenti, filter.filtered_centi());

  EXPECT_TRUE(filter.push(2000));
  EXPECT_EQ(2000, filter.filtered_centi());
  EXPECT_TRUE(filter.push(2040));
  EXPECT_EQ(2020, filter.filtered_centi());
  EXPECT_TRUE(filter.push(2080));
  EXPECT_EQ(2040, filter.filtered_centi());
  EXPECT_TRUE(filter.push(2120));
  EXPECT_EQ(2060, filter.filtered_centi());
  EXPECT_TRUE(filter.push(2160));
  // Window is full: (2000 + 2040 + 2080 + 2120 + 2160) / 5 == 2080.
  EXPECT_EQ(2080, filter.filtered_centi());

  // Sixth sample slides the window: (2040 + 2080 + 2120 + 2160 + 2200) / 5.
  EXPECT_TRUE(filter.push(2200));
  EXPECT_EQ(2120, filter.filtered_centi());
}

TEST_CASE(temp_filter_rejects_out_of_range_samples) {
  TempFilter filter;
  EXPECT_FALSE(filter.push(-1001));
  EXPECT_FALSE(filter.push(6001));
  EXPECT_FALSE(filter.push(kInvalidTempCenti));
  EXPECT_FALSE(filter.has_value());
  EXPECT_EQ(kInvalidTempCenti, filter.filtered_centi());

  // The documented bounds themselves are plausible.
  EXPECT_TRUE(filter.push(-1000));
  EXPECT_EQ(-1000, filter.filtered_centi());

  TempFilter upper;
  EXPECT_TRUE(upper.push(6000));
  EXPECT_EQ(6000, upper.filtered_centi());
}

TEST_CASE(temp_filter_rejects_spike_then_accepts_persistent_change) {
  TempFilter filter;
  EXPECT_TRUE(filter.push(2000));

  // A single 6.0 degC jump is a glitch and must not move the output.
  EXPECT_FALSE(filter.push(2600));
  EXPECT_EQ(2000, filter.filtered_centi());
  // Second consecutive jump is still rejected.
  EXPECT_FALSE(filter.push(2700));
  EXPECT_EQ(2000, filter.filtered_centi());
  // Third consecutive jump is a real change: accept and restart the window.
  EXPECT_TRUE(filter.push(2800));
  EXPECT_EQ(2800, filter.filtered_centi());

  // Normal samples keep flowing afterwards.
  EXPECT_TRUE(filter.push(2820));
  EXPECT_EQ(2810, filter.filtered_centi());
}

TEST_CASE(temp_filter_reset_clears_state) {
  TempFilter filter;
  EXPECT_TRUE(filter.push(2140));
  EXPECT_TRUE(filter.has_value());
  filter.reset();
  EXPECT_FALSE(filter.has_value());
  EXPECT_EQ(kInvalidTempCenti, filter.filtered_centi());
  EXPECT_TRUE(filter.push(2140));
  EXPECT_EQ(2140, filter.filtered_centi());
}

// --- Age formatting -------------------------------------------------------

TEST_CASE(format_age_korean_matches_ui_rules) {
  EXPECT_STREQ("방금", thermostat::sensor::format_age_korean(0).c_str());
  EXPECT_STREQ("방금", thermostat::sensor::format_age_korean(59).c_str());
  EXPECT_STREQ("1분 전", thermostat::sensor::format_age_korean(60).c_str());
  EXPECT_STREQ("3분 전", thermostat::sensor::format_age_korean(180).c_str());
  EXPECT_STREQ("10분 전", thermostat::sensor::format_age_korean(600).c_str());
  // Clock skew is clamped by callers; the formatter itself still yields 방금.
  EXPECT_STREQ("방금", thermostat::sensor::format_age_korean(-5).c_str());
}

// --- ISO-8601 -------------------------------------------------------------

TEST_CASE(parse_iso8601_handles_offsets_and_fractions) {
  int64_t epoch_sec = 0;
  EXPECT_TRUE(thermostat::sensor::parse_iso8601_to_epoch(
      "2026-11-24T21:15:00+09:00", &epoch_sec));
  EXPECT_EQ(1795522500, epoch_sec);

  EXPECT_TRUE(thermostat::sensor::parse_iso8601_to_epoch(
      "2026-11-24T12:15:00Z", &epoch_sec));
  EXPECT_EQ(1795522500, epoch_sec);

  EXPECT_TRUE(thermostat::sensor::parse_iso8601_to_epoch(
      "2026-11-24T07:15:00-05:00", &epoch_sec));
  EXPECT_EQ(1795522500, epoch_sec);

  EXPECT_TRUE(thermostat::sensor::parse_iso8601_to_epoch(
      "2026-11-24T21:15:00.123+09:00", &epoch_sec));
  EXPECT_EQ(1795522500, epoch_sec);

  EXPECT_TRUE(thermostat::sensor::parse_iso8601_to_epoch(
      "1970-01-01T00:00:00Z", &epoch_sec));
  EXPECT_EQ(0, epoch_sec);

  // Leap day.
  EXPECT_TRUE(thermostat::sensor::parse_iso8601_to_epoch(
      "2024-02-29T00:00:00Z", &epoch_sec));
  EXPECT_EQ(1709164800, epoch_sec);
}

TEST_CASE(parse_iso8601_rejects_invalid_strings) {
  int64_t epoch_sec = 1234;
  const char* const invalid[] = {
      "",
      "garbage",
      "2026-11-24",
      "2026-11-24T21:15:00",
      "2026-13-01T00:00:00Z",
      "2026-00-10T00:00:00Z",
      "2026-02-30T00:00:00Z",
      "2023-02-29T00:00:00Z",
      "2026-11-24T24:00:00Z",
      "2026-11-24T21:60:00Z",
      "2026-11-24T21:15:60Z",
      "2026-11-24T21:15:00+24:00",
      "2026-11-24T21:15:00+09:60",
      "2026-11-24T21:15:00+0900",
      "2026-11-24T21:15:00Zx",
      "2026-11-24T21:15:00.Z",
      "2026-1-24T21:15:00Z",
      "2026-11-24X21:15:00Z",
  };
  for (const char* iso : invalid) {
    EXPECT_FALSE(thermostat::sensor::parse_iso8601_to_epoch(iso, &epoch_sec));
  }
  // A failed parse must not touch the output.
  EXPECT_EQ(1234, epoch_sec);
  EXPECT_FALSE(thermostat::sensor::parse_iso8601_to_epoch(nullptr, &epoch_sec));
  EXPECT_FALSE(thermostat::sensor::parse_iso8601_to_epoch(
      "2026-11-24T12:15:00Z", nullptr));
}

// --- Home-server JSON -----------------------------------------------------

TEST_CASE(parse_homeserver_sample_json) {
  const std::string json = R"json({
  "sensors": [
    {
      "id": "livingroom",
      "name": "거실",
      "temperature": 21.4,
      "updated_at": "2026-11-24T21:15:00+09:00"
    },
    {
      "id": "bedroom",
      "name": "안방",
      "temperature": 20.8,
      "updated_at": "2026-11-24T21:13:00+09:00"
    }
  ]
})json";
  std::vector<RemoteSensorReading> readings;
  EXPECT_TRUE(
      thermostat::sensor::parse_homeserver_sensors_json(json, &readings));
  EXPECT_EQ(static_cast<size_t>(2), readings.size());
  EXPECT_STREQ("livingroom", readings[0].id.c_str());
  EXPECT_STREQ("거실", readings[0].name.c_str());
  EXPECT_EQ(2140, readings[0].temp_centi);
  EXPECT_EQ(1795522500, readings[0].updated_epoch_sec);
  EXPECT_STREQ("bedroom", readings[1].id.c_str());
  EXPECT_STREQ("안방", readings[1].name.c_str());
  EXPECT_EQ(2080, readings[1].temp_centi);
  EXPECT_EQ(1795522380, readings[1].updated_epoch_sec);
  // Documented average: (2140 + 2080) / 2 == 2110.
  EXPECT_EQ(2110, (readings[0].temp_centi + readings[1].temp_centi) / 2);
}

TEST_CASE(parse_homeserver_json_number_forms) {
  std::vector<RemoteSensorReading> readings;
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"a","name":"A","temperature":21,"updated_at":"2026-11-24T12:15:00Z"}]})json",
      &readings));
  EXPECT_EQ(static_cast<size_t>(1), readings.size());
  EXPECT_EQ(2100, readings[0].temp_centi);

  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"b","name":"B","temperature":2.14e1,"updated_at":"2026-11-24T12:15:00Z"}]})json",
      &readings));
  EXPECT_EQ(static_cast<size_t>(1), readings.size());
  EXPECT_EQ(2140, readings[0].temp_centi);

  // 20.005 degC == 2000.5 centi: half away from zero rounds to 2001.
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"c","name":"C","temperature":20.005,"updated_at":"2026-11-24T12:15:00Z"}]})json",
      &readings));
  EXPECT_EQ(static_cast<size_t>(1), readings.size());
  EXPECT_EQ(2001, readings[0].temp_centi);

  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"d","name":"D","temperature":-5.5,"updated_at":"2026-11-24T12:15:00Z"}]})json",
      &readings));
  EXPECT_EQ(static_cast<size_t>(1), readings.size());
  EXPECT_EQ(-550, readings[0].temp_centi);

  // \u escapes (UTF-16 for 거실) decode to UTF-8.
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"e","name":"거실","temperature":21.4,"updated_at":"2026-11-24T12:15:00Z"}]})json",
      &readings));
  EXPECT_EQ(static_cast<size_t>(1), readings.size());
  EXPECT_STREQ("거실", readings[0].name.c_str());
}

TEST_CASE(parse_homeserver_json_skips_incomplete_entries) {
  std::vector<RemoteSensorReading> readings;
  // Missing updated_at.
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"a","name":"A","temperature":21.5}]})json",
      &readings));
  EXPECT_TRUE(readings.empty());

  // Temperature encoded as a string is not a number.
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"a","name":"A","temperature":"21.5","updated_at":"2026-11-24T12:15:00Z"}]})json",
      &readings));
  EXPECT_TRUE(readings.empty());

  // Unparsable timestamp.
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors":[{"id":"a","name":"A","temperature":21.5,"updated_at":"yesterday"}]})json",
      &readings));
  EXPECT_TRUE(readings.empty());

  // Extra unknown fields are ignored, and one broken entry does not discard
  // the usable ones.
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"updated": "x", "sensors":[
        {"id":"bad","name":"Bad","temperature":21.5},
        {"id":"good","name":"Good","temperature":22.5,"updated_at":"2026-11-24T12:15:00Z","extra":[1,2,{"k":"v"}]}
      ]})json",
      &readings));
  EXPECT_EQ(static_cast<size_t>(1), readings.size());
  EXPECT_STREQ("good", readings[0].id.c_str());
  EXPECT_EQ(2250, readings[0].temp_centi);
}

TEST_CASE(parse_homeserver_json_rejects_malformed_documents) {
  std::vector<RemoteSensorReading> readings;
  EXPECT_TRUE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors": []})json", &readings));
  EXPECT_TRUE(readings.empty());

  const char* const malformed[] = {
      "",
      "null",
      "[]",
      R"json({"sensors": [})json",
      R"json({"sensors": 42})json",
      R"json({"other": []})json",
      R"json({"sensors": []})json trailing",
      R"json({"sensors": [1, 2]})json",
  };
  for (const char* json : malformed) {
    EXPECT_FALSE(
        thermostat::sensor::parse_homeserver_sensors_json(json, &readings));
  }
  EXPECT_FALSE(thermostat::sensor::parse_homeserver_sensors_json(
      R"json({"sensors": []})json", nullptr));
}

// --- SensorSelector: board ------------------------------------------------

TEST_CASE(selector_board_only_reports_age_and_freshness) {
  SensorSelector selector;
  selector.update_board_temp(2140, 1000);

  ResolvedTemperature resolved = selector.resolve(1000);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2140, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);
  EXPECT_EQ(0, resolved.age_sec);
  EXPECT_STREQ("보드 센서 · 방금", resolved.source_label.c_str());

  resolved = selector.resolve(1180);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(180, resolved.age_sec);
  EXPECT_STREQ("보드 센서 · 3분 전", resolved.source_label.c_str());
}

TEST_CASE(selector_board_only_warns_and_faults_on_stale_values) {
  SensorSelector selector;
  selector.update_board_temp(2140, kNow);

  ResolvedTemperature resolved = selector.resolve(kNow + 300);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);

  resolved = selector.resolve(kNow + 301);
  EXPECT_TRUE(resolved.valid);
  EXPECT_TRUE(resolved.freshness == Freshness::kWarn);
  EXPECT_STREQ("보드 센서 · 5분 전", resolved.source_label.c_str());

  resolved = selector.resolve(kNow + 600);
  EXPECT_TRUE(resolved.valid);
  EXPECT_TRUE(resolved.freshness == Freshness::kWarn);

  // More than 10 minutes without a board sample is a fault.
  resolved = selector.resolve(kNow + 601);
  EXPECT_FALSE(resolved.valid);
  EXPECT_EQ(kInvalidTempCenti, resolved.temp_centi);
  EXPECT_TRUE(resolved.freshness == Freshness::kFault);
  EXPECT_FALSE(resolved.used_fallback_board);
  EXPECT_STREQ("센서 오류 · 온도 확인 불가", resolved.source_label.c_str());
}

TEST_CASE(selector_board_offset_is_applied) {
  SensorSelector selector;
  SensorSelectionConfig config;
  config.mode = SourceMode::kBoardOnly;
  config.board_offset_centi = 150;
  selector.set_config(config);

  selector.update_board_temp(2140, kNow);
  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2290, resolved.temp_centi);
  EXPECT_STREQ("보드 센서 · 방금", resolved.source_label.c_str());
}

TEST_CASE(selector_board_sensor_faults_on_invalid_sample) {
  SensorSelector selector;
  selector.update_board_temp(2140, kNow);
  EXPECT_TRUE(selector.resolve(kNow).valid);

  // The local SHT40 read failed: the board sensor is faulted immediately.
  selector.update_board_temp(kInvalidTempCenti, kNow);
  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_FALSE(resolved.valid);
  EXPECT_TRUE(resolved.freshness == Freshness::kFault);

  selector.update_board_temp(2140, kNow);
  EXPECT_TRUE(selector.resolve(kNow).valid);
  selector.update_board_temp(7000, kNow);
  EXPECT_FALSE(selector.resolve(kNow).valid);
}

// --- SensorSelector: remote ----------------------------------------------

TEST_CASE(selector_single_remote_uses_latest_reading) {
  SensorSelector selector;
  selector.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  selector.update_board_temp(2000, kNow);
  selector.record_remote_success(
      {make_reading("bedroom", "안방", 2080, kNow),
       make_reading("livingroom", "거실", 2140, kNow - 180)},
      kNow);

  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2140, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);
  EXPECT_EQ(180, resolved.age_sec);
  EXPECT_STREQ("거실 센서 · 3분 전", resolved.source_label.c_str());
}

TEST_CASE(selector_single_remote_avoids_duplicate_sensor_suffix) {
  SensorSelector selector;
  selector.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  selector.update_board_temp(2000, kNow);
  selector.record_remote_success(
      {make_reading("livingroom", "거실 센서", 2140, kNow)}, kNow);

  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_STREQ("거실 센서 · 방금", resolved.source_label.c_str());
}

TEST_CASE(selector_average_remote_reports_average_and_label) {
  SensorSelector selector;
  selector.set_config(make_remote_config(SourceMode::kAverageRemote,
                                         {"livingroom", "bedroom"}));
  selector.update_board_temp(2000, kNow);
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 120),
       make_reading("bedroom", "안방", 2080, kNow - 100)},
      kNow);

  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2110, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);
  EXPECT_EQ(120, resolved.age_sec);
  EXPECT_STREQ("평균(거실, 안방) · 최근 2분 전", resolved.source_label.c_str());

  // Half a centi rounds away from zero: (2140 + 2081) / 2 == 2110.5 -> 2111.
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 60),
       make_reading("bedroom", "안방", 2081, kNow - 60)},
      kNow);
  resolved = selector.resolve(kNow);
  EXPECT_EQ(2111, resolved.temp_centi);
  EXPECT_STREQ("평균(거실, 안방) · 최근 1분 전", resolved.source_label.c_str());
}

TEST_CASE(selector_remote_warns_after_five_minutes) {
  SensorSelector selector;
  selector.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  selector.update_board_temp(2000, kNow);

  // Exactly 5 minutes is still fresh; one second more is a warning.
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 300)}, kNow);
  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);

  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 301)}, kNow);
  resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_TRUE(resolved.freshness == Freshness::kWarn);
  EXPECT_STREQ("거실 센서 · 5분 전", resolved.source_label.c_str());
}

TEST_CASE(selector_remote_stale_falls_back_to_board) {
  SensorSelector selector;
  selector.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  selector.update_board_temp(2100, kNow);

  // Exactly 10 minutes is still usable.
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 600)}, kNow);
  EXPECT_EQ(2140, selector.resolve(kNow).temp_centi);

  // More than 10 minutes old: the board sensor takes over.
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 601)}, kNow);
  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2100, resolved.temp_centi);
  EXPECT_TRUE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kStaleOrFallback);
  EXPECT_EQ(0, resolved.age_sec);
  EXPECT_STREQ("보드 센서 · 원격 센서 값 오래됨",
               resolved.source_label.c_str());
}

TEST_CASE(selector_failures_keep_previous_value_until_third) {
  SensorSelector selector;
  selector.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  selector.update_board_temp(2000, kNow);
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow)}, kNow);

  selector.record_remote_failure();
  EXPECT_EQ(1, static_cast<int>(selector.consecutive_failures()));
  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_EQ(2140, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);

  selector.record_remote_failure();
  EXPECT_EQ(2, static_cast<int>(selector.consecutive_failures()));
  resolved = selector.resolve(kNow);
  EXPECT_EQ(2140, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);

  selector.record_remote_failure();
  EXPECT_EQ(3, static_cast<int>(selector.consecutive_failures()));
  resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2000, resolved.temp_centi);
  EXPECT_TRUE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kStaleOrFallback);
  EXPECT_STREQ("보드 센서 · 원격 센서 응답 없음",
               resolved.source_label.c_str());

  // A successful poll clears the failure counter and restores remote values.
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow)}, kNow);
  resolved = selector.resolve(kNow);
  EXPECT_EQ(2140, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);
}

TEST_CASE(selector_missing_remote_reading_falls_back_to_board) {
  SensorSelector selector;
  selector.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  selector.update_board_temp(2000, kNow);

  // No successful poll stored yet.
  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_EQ(2000, resolved.temp_centi);
  EXPECT_STREQ("보드 센서 · 원격 센서 응답 없음",
               resolved.source_label.c_str());

  // A poll succeeded, but the selected id is absent from the response.
  selector.record_remote_success(
      {make_reading("bedroom", "안방", 2080, kNow)}, kNow);
  resolved = selector.resolve(kNow);
  EXPECT_EQ(2000, resolved.temp_centi);
  EXPECT_TRUE(resolved.used_fallback_board);
}

TEST_CASE(selector_average_ignores_stale_sensor) {
  SensorSelector selector;
  selector.set_config(make_remote_config(SourceMode::kAverageRemote,
                                         {"livingroom", "bedroom"}));
  selector.update_board_temp(2000, kNow);
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 700),
       make_reading("bedroom", "안방", 2080, kNow - 100)},
      kNow);

  ResolvedTemperature resolved = selector.resolve(kNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2080, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);
  EXPECT_EQ(100, resolved.age_sec);
  EXPECT_STREQ("평균(안방) · 최근 1분 전", resolved.source_label.c_str());

  // Both selected sensors stale -> the board sensor takes over.
  selector.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 700),
       make_reading("bedroom", "안방", 2080, kNow - 700)},
      kNow);
  resolved = selector.resolve(kNow);
  EXPECT_EQ(2000, resolved.temp_centi);
  EXPECT_TRUE(resolved.used_fallback_board);
  EXPECT_STREQ("보드 센서 · 원격 센서 값 오래됨",
               resolved.source_label.c_str());
}

TEST_CASE(selector_reports_fault_when_no_sensor_is_usable) {
  SensorSelector board_only;
  ResolvedTemperature resolved = board_only.resolve(kNow);
  EXPECT_FALSE(resolved.valid);
  EXPECT_EQ(kInvalidTempCenti, resolved.temp_centi);
  EXPECT_FALSE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kFault);
  EXPECT_STREQ("센서 오류 · 온도 확인 불가", resolved.source_label.c_str());

  SensorSelector remote;
  remote.set_config(
      make_remote_config(SourceMode::kSingleRemote, {"livingroom"}));
  remote.record_remote_success(
      {make_reading("livingroom", "거실", 2140, kNow - 700)}, kNow);
  resolved = remote.resolve(kNow);
  EXPECT_FALSE(resolved.valid);
  EXPECT_EQ(kInvalidTempCenti, resolved.temp_centi);
  EXPECT_TRUE(resolved.used_fallback_board);
  EXPECT_TRUE(resolved.freshness == Freshness::kFault);
  EXPECT_STREQ("센서 오류 · 온도 확인 불가", resolved.source_label.c_str());
}

// --- Parsed sample feeds the selector ------------------------------------

TEST_CASE(selector_resolves_parsed_homeserver_sample) {
  const std::string json = R"json({
  "sensors": [
    { "id": "livingroom", "name": "거실",
      "temperature": 21.4, "updated_at": "2026-11-24T21:15:00+09:00" },
    { "id": "bedroom", "name": "안방",
      "temperature": 20.8, "updated_at": "2026-11-24T21:13:00+09:00" }
  ]
})json";
  std::vector<RemoteSensorReading> readings;
  EXPECT_TRUE(
      thermostat::sensor::parse_homeserver_sensors_json(json, &readings));

  // Resolve at the sample's own "now" (21:15 KST) so the ages are 0/2 min.
  constexpr int64_t kSampleNow = 1795522500;
  SensorSelector selector;
  selector.set_config(make_remote_config(SourceMode::kAverageRemote,
                                         {"livingroom", "bedroom"}));
  selector.update_board_temp(2000, kSampleNow);
  selector.record_remote_success(readings, kSampleNow);

  ResolvedTemperature resolved = selector.resolve(kSampleNow);
  EXPECT_TRUE(resolved.valid);
  EXPECT_EQ(2110, resolved.temp_centi);
  EXPECT_EQ(120, resolved.age_sec);
  EXPECT_TRUE(resolved.freshness == Freshness::kFresh);
  EXPECT_STREQ("평균(거실, 안방) · 최근 2분 전", resolved.source_label.c_str());
}

THERMOSTAT_TEST_MAIN()
