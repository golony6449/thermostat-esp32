// LINK: src/core/*.cpp src/sensor/sensor_logic.cpp src/storage/settings_codec.cpp src/app/thermostat_app.cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/thermostat_app.h"
#include "test_harness.h"

namespace {

using thermostat::app::SyncActions;
using thermostat::app::ThermostatApp;
using thermostat::core::Mode;
using thermostat::sensor::Freshness;
using thermostat::storage::DeviceSettings;
using thermostat::storage::SensorSourceSetting;
using thermostat::ui::SourceSeverity;
using thermostat::ui::UiMode;

// SCRUM-239 sample response time: 2026-11-24T21:15:00+09:00 == 12:15 UTC.
constexpr int64_t kSampleEpochSec = 1795522500;

DeviceSettings base_settings() {
  DeviceSettings settings = thermostat::storage::default_device_settings();
  settings.active_zones = 1;
  settings.zones[0].control.mode = Mode::kTemperature;
  settings.zones[0].control.target_temp_centi = 2100;
  settings.zones[0].control.hysteresis_centi = 50;
  settings.zones[0].control.min_on_sec = 300;
  settings.zones[0].control.min_off_sec = 300;
  return settings;
}

void expect_no_actions(const SyncActions& actions) {
  EXPECT_FALSE(actions.settings_dirty);
  EXPECT_FALSE(actions.push_matter_mode);
  EXPECT_FALSE(actions.push_matter_setpoint);
  EXPECT_FALSE(actions.push_matter_temp);
  EXPECT_FALSE(actions.push_matter_heating);
  EXPECT_FALSE(actions.ui_dirty);
}

void select_single_remote(DeviceSettings* settings) {
  settings->zones[0].sensor_source = SensorSourceSetting::kSingleRemote;
  settings->zones[0].selected_remote_count = 1;
  std::snprintf(settings->zones[0].selected_remote_ids[0],
                thermostat::storage::kMaxSensorIdLen, "%s", "livingroom");
}

}  // namespace

TEST_CASE(init_applies_restored_settings_and_initial_actions) {
  DeviceSettings settings = base_settings();
  settings.zones[0].control.target_temp_centi = 2200;
  settings.zones[0].control.repeat_on_minutes = 20;
  settings.zones[0].control.freeze_protect_centi = 900;
  settings.zones[0].sensor_offset_centi = -20;

  ThermostatApp app;
  app.init(settings, 1000);

  EXPECT_EQ(2200, app.settings().zones[0].control.target_temp_centi);
  EXPECT_EQ(20, app.settings().zones[0].control.repeat_on_minutes);
  EXPECT_EQ(900, app.settings().zones[0].control.freeze_protect_centi);
  EXPECT_EQ(-20, app.settings().zones[0].sensor_offset_centi);
  EXPECT_TRUE(app.is_matter_heat_mode());

  const SyncActions actions = app.take_actions();
  EXPECT_FALSE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_TRUE(actions.push_matter_mode);
  EXPECT_TRUE(actions.matter_heat_mode);
  EXPECT_TRUE(actions.push_matter_setpoint);
  EXPECT_EQ(2200, actions.matter_setpoint_centi);
  EXPECT_FALSE(actions.push_matter_temp);
  EXPECT_FALSE(actions.push_matter_heating);

  const thermostat::ui::UiViewModel view = app.build_view_model(0);
  EXPECT_EQ(2200, view.target_temp_centi);
  EXPECT_EQ(static_cast<int>(UiMode::kTemperature),
            static_cast<int>(view.mode));
  EXPECT_FALSE(view.temp_valid);
  EXPECT_FALSE(view.heating_active);
  EXPECT_STREQ("--:--", view.time_text);

  // The initial actions are one-shot.
  expect_no_actions(app.take_actions());
}

TEST_CASE(ui_changes_produce_matter_and_nvs_actions) {
  ThermostatApp app;
  app.init(base_settings(), 0);
  app.take_actions();

  // Target temperature: panel -> app -> Matter setpoint + NVS.
  app.on_ui_target_temp(2300);
  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_TRUE(actions.push_matter_setpoint);
  EXPECT_EQ(2300, actions.matter_setpoint_centi);
  EXPECT_FALSE(actions.push_matter_mode);
  EXPECT_EQ(2300, app.settings().zones[0].control.target_temp_centi);

  // Re-entering the same value changes nothing.
  app.on_ui_target_temp(2300);
  expect_no_actions(app.take_actions());

  // Temperature -> repeat stays inside Matter Heat, so no SystemMode push.
  app.on_ui_mode(UiMode::kRepeat);
  actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_FALSE(actions.push_matter_mode);
  EXPECT_EQ(static_cast<int>(Mode::kRepeat),
            static_cast<int>(app.settings().zones[0].control.mode));

  app.on_ui_mode(UiMode::kRepeat);
  expect_no_actions(app.take_actions());

  app.on_ui_repeat_minutes(30);
  actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_EQ(30, app.settings().zones[0].control.repeat_on_minutes);
  EXPECT_FALSE(actions.push_matter_setpoint);

  app.on_ui_repeat_minutes(30);
  expect_no_actions(app.take_actions());

  app.on_ui_freeze_protect(1200);
  actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_EQ(1200, app.settings().zones[0].control.freeze_protect_centi);

  // Out-of-range values are clamped by sanitize_settings.
  app.on_ui_target_temp(9999);
  EXPECT_EQ(3000, app.settings().zones[0].control.target_temp_centi);
  app.take_actions();
  app.on_ui_target_temp(3000);
  expect_no_actions(app.take_actions());

  // Off is a Matter SystemMode change and must be pushed.
  app.on_ui_mode(UiMode::kOff);
  actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_mode);
  EXPECT_FALSE(actions.matter_heat_mode);
  EXPECT_EQ(static_cast<int>(Mode::kOff),
            static_cast<int>(app.settings().zones[0].control.mode));
}

TEST_CASE(matter_changes_update_ui_without_echo) {
  DeviceSettings settings = base_settings();
  settings.zones[0].control.mode = Mode::kOff;

  ThermostatApp app;
  app.init(settings, 0);
  app.take_actions();

  app.on_matter_setpoint(2250);
  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_FALSE(actions.push_matter_setpoint);
  EXPECT_FALSE(actions.push_matter_mode);
  EXPECT_FALSE(actions.push_matter_temp);
  EXPECT_FALSE(actions.push_matter_heating);
  EXPECT_EQ(2250, app.settings().zones[0].control.target_temp_centi);

  app.on_matter_mode(true);
  actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_FALSE(actions.push_matter_mode);
  EXPECT_EQ(static_cast<int>(Mode::kTemperature),
            static_cast<int>(app.settings().zones[0].control.mode));
  EXPECT_EQ(static_cast<int>(UiMode::kTemperature),
            static_cast<int>(app.build_view_model(0).mode));
  EXPECT_TRUE(app.is_matter_heat_mode());
}

TEST_CASE(repeat_mode_survives_matter_heat_and_off_round_trip) {
  DeviceSettings settings = base_settings();
  settings.zones[0].control.mode = Mode::kRepeat;
  settings.zones[0].control.repeat_on_minutes = 20;

  ThermostatApp app;
  app.init(settings, 0);
  app.take_actions();

  // Matter Heat while already in repeat mode keeps repeat (no clobbering).
  app.on_matter_mode(true);
  expect_no_actions(app.take_actions());
  EXPECT_EQ(static_cast<int>(Mode::kRepeat),
            static_cast<int>(app.settings().zones[0].control.mode));

  // Matter Off, then Heat again restores the last heat mode (repeat).
  app.on_matter_mode(false);
  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_EQ(static_cast<int>(Mode::kOff),
            static_cast<int>(app.settings().zones[0].control.mode));

  app.on_matter_mode(true);
  actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_EQ(static_cast<int>(Mode::kRepeat),
            static_cast<int>(app.settings().zones[0].control.mode));
  EXPECT_EQ(20, app.settings().zones[0].control.repeat_on_minutes);
}

TEST_CASE(sensor_temperature_drives_matter_heating_and_threshold) {
  ThermostatApp app;
  app.init(base_settings(), 0);
  app.take_actions();

  app.on_board_temp_sample(1900, kSampleEpochSec);
  app.step(1000, kSampleEpochSec);
  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_temp);
  EXPECT_EQ(1900, actions.matter_temp_centi);
  EXPECT_TRUE(actions.push_matter_heating);
  EXPECT_TRUE(actions.matter_heating);
  EXPECT_TRUE(actions.ui_dirty);
  EXPECT_TRUE(app.last_output().logical_heating);
  EXPECT_TRUE(app.build_view_model(kSampleEpochSec).heating_active);

  // 0.05 degC raw step moves the moving-average value by 0.03 degC only, so
  // Matter is not spammed with a sub-0.1 degC update.
  app.on_board_temp_sample(1905, kSampleEpochSec + 5);
  app.step(6000, kSampleEpochSec + 5);
  actions = app.take_actions();
  EXPECT_FALSE(actions.push_matter_temp);
  EXPECT_FALSE(actions.push_matter_heating);

  // A larger change is published again.
  app.on_board_temp_sample(2040, kSampleEpochSec + 10);
  app.step(11000, kSampleEpochSec + 10);
  actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_temp);
  EXPECT_EQ(1948, actions.matter_temp_centi);  // (1900 + 1905 + 2040) / 3
  EXPECT_FALSE(actions.push_matter_heating);
}

TEST_CASE(remote_sensor_json_used_and_failures_fall_back_to_board) {
  DeviceSettings settings = base_settings();
  select_single_remote(&settings);

  ThermostatApp app;
  app.init(settings, 0);
  app.take_actions();

  constexpr int64_t kNow = kSampleEpochSec + 60;
  app.on_board_temp_sample(1800, kNow);

  // A structurally invalid response counts as one failure.
  EXPECT_FALSE(app.on_remote_sensors_json("{\"unexpected\":true}", kNow));

  const std::string json =
      "{\"sensors\":[{\"id\":\"livingroom\",\"name\":\"거실\","
      "\"temperature\":21.4,\"updated_at\":\"2026-11-24T21:15:00+09:00\"}]}";
  EXPECT_TRUE(app.on_remote_sensors_json(json, kNow));
  app.step(1000, kNow);

  const thermostat::sensor::ResolvedTemperature& remote = app.last_temp();
  EXPECT_TRUE(remote.valid);
  EXPECT_FALSE(remote.used_fallback_board);
  EXPECT_EQ(2140, remote.temp_centi);
  EXPECT_EQ(static_cast<int>(Freshness::kFresh),
            static_cast<int>(remote.freshness));
  EXPECT_STREQ("거실 센서 · 1분 전", remote.source_label.c_str());

  thermostat::ui::UiViewModel view = app.build_view_model(kNow);
  EXPECT_EQ(static_cast<int>(SourceSeverity::kNormal),
            static_cast<int>(view.source_severity));
  EXPECT_STREQ("거실 센서 · 1분 전", view.source_text);

  // Three consecutive fetch failures switch back to the board sensor.
  app.on_remote_fetch_failed();
  app.on_remote_fetch_failed();
  app.on_remote_fetch_failed();
  app.step(2000, kNow + 5);

  const thermostat::sensor::ResolvedTemperature& fallback = app.last_temp();
  EXPECT_TRUE(fallback.valid);
  EXPECT_TRUE(fallback.used_fallback_board);
  EXPECT_EQ(1800, fallback.temp_centi);
  EXPECT_EQ(static_cast<int>(Freshness::kStaleOrFallback),
            static_cast<int>(fallback.freshness));

  view = app.build_view_model(kNow + 5);
  EXPECT_EQ(static_cast<int>(SourceSeverity::kError),
            static_cast<int>(view.source_severity));
  EXPECT_STREQ("보드 센서 · 원격 센서 응답 없음", view.source_text);
}

TEST_CASE(keep_alive_pulse_splits_matter_from_ui) {
  DeviceSettings settings = base_settings();
  settings.zones[0].control.target_temp_centi = 2500;
  settings.zones[0].control.keep_alive_interval_sec = 60;
  settings.zones[0].control.keep_alive_off_sec = 10;

  ThermostatApp app;
  app.init(settings, 0);
  app.take_actions();

  app.on_board_temp_sample(1800, kSampleEpochSec);
  app.step(0, kSampleEpochSec);
  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_heating);
  EXPECT_TRUE(actions.matter_heating);

  // 60 s of continuous heating: Matter briefly sees "not heating" so the
  // SmartThings 70-minute dead-man routine resets, but the UI keeps showing
  // the heating badge.
  app.step(60000, kSampleEpochSec + 60);
  actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_heating);
  EXPECT_FALSE(actions.matter_heating);
  EXPECT_TRUE(app.last_output().logical_heating);
  EXPECT_FALSE(app.last_output().matter_heating);
  EXPECT_TRUE(app.last_output().keep_alive_pulsing);
  EXPECT_TRUE(app.build_view_model(kSampleEpochSec + 60).heating_active);

  // Still inside the 10 s pulse window: nothing new to publish.
  app.step(65000, kSampleEpochSec + 65);
  actions = app.take_actions();
  EXPECT_FALSE(actions.push_matter_heating);
  EXPECT_TRUE(app.last_output().keep_alive_pulsing);
  EXPECT_TRUE(app.build_view_model(kSampleEpochSec + 65).heating_active);

  // Pulse over: Matter sees heating again, the UI never changed.
  app.step(70000, kSampleEpochSec + 70);
  actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_heating);
  EXPECT_TRUE(actions.matter_heating);
  EXPECT_TRUE(app.last_output().logical_heating);
  EXPECT_TRUE(app.build_view_model(kSampleEpochSec + 70).heating_active);
}

TEST_CASE(simulated_temperature_overrides_then_returns_to_sensor_path) {
  DeviceSettings settings = base_settings();
  settings.zones[0].control.target_temp_centi = 2000;
  settings.zones[0].control.min_on_sec = 0;
  settings.zones[0].control.min_off_sec = 0;

  ThermostatApp app;
  app.init(settings, 0);
  app.take_actions();

  app.on_board_temp_sample(2500, kSampleEpochSec);
  app.step(0, kSampleEpochSec);
  app.take_actions();
  EXPECT_EQ(2500, app.last_temp().temp_centi);
  EXPECT_FALSE(app.last_output().logical_heating);

  app.set_simulated_temp(1850);
  EXPECT_TRUE(app.has_simulated_temp());
  app.step(1000, kSampleEpochSec + 1);
  EXPECT_TRUE(app.last_temp().valid);
  EXPECT_EQ(1850, app.last_temp().temp_centi);
  EXPECT_EQ(static_cast<int>(Freshness::kFresh),
            static_cast<int>(app.last_temp().freshness));
  EXPECT_STREQ("시뮬레이션 · 방금", app.last_temp().source_label.c_str());
  EXPECT_TRUE(app.last_output().logical_heating);

  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_temp);
  EXPECT_EQ(1850, actions.matter_temp_centi);
  EXPECT_TRUE(actions.push_matter_heating);
  EXPECT_TRUE(actions.matter_heating);

  app.clear_simulated_temp();
  EXPECT_FALSE(app.has_simulated_temp());
  app.step(2000, kSampleEpochSec + 2);
  EXPECT_EQ(2500, app.last_temp().temp_centi);
  EXPECT_FALSE(app.last_output().logical_heating);

  actions = app.take_actions();
  EXPECT_TRUE(actions.push_matter_temp);
  EXPECT_TRUE(actions.push_matter_heating);
  EXPECT_FALSE(actions.matter_heating);
}

TEST_CASE(kst_time_text_formats_hour_and_minute) {
  ThermostatApp app;
  app.init(base_settings(), 0);

  const thermostat::ui::UiViewModel unsynced = app.build_view_model(0);
  EXPECT_STREQ("--:--", unsynced.time_text);

  // 2026-11-24T12:15:00Z == 21:15 KST.
  const thermostat::ui::UiViewModel kst = app.build_view_model(kSampleEpochSec);
  EXPECT_STREQ("21:15", kst.time_text);

  // 2023-11-14T22:13:20Z == 07:13 KST the next day.
  const thermostat::ui::UiViewModel midnight =
      app.build_view_model(1700000000);
  EXPECT_STREQ("07:13", midnight.time_text);
}

TEST_CASE(homeserver_url_update_marks_settings_dirty) {
  ThermostatApp app;
  app.init(base_settings(), 0);
  app.take_actions();

  app.set_homeserver_url("http://192.168.0.10");
  SyncActions actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_FALSE(actions.ui_dirty);
  EXPECT_STREQ("http://192.168.0.10", app.settings().homeserver_url);

  app.set_homeserver_url("http://192.168.0.10");
  expect_no_actions(app.take_actions());

  // Long URLs are truncated to the persisted field size (kMaxUrlLen - 1).
  std::string long_url = "http://192.168.0.10/";
  long_url.append(200, 'x');
  app.set_homeserver_url(long_url.c_str());
  actions = app.take_actions();
  EXPECT_TRUE(actions.settings_dirty);
  EXPECT_EQ(static_cast<size_t>(thermostat::storage::kMaxUrlLen - 1),
            std::strlen(app.settings().homeserver_url));
}

TEST_CASE(sensor_source_update_switches_to_remote_and_marks_dirty) {
  ThermostatApp app;
  app.init(base_settings(), 0);
  app.take_actions();

  constexpr int64_t kNow = 1795700497 + 60;  // real sample updated_at + 1 min
  app.on_board_temp_sample(1800, kNow);
  const std::string json =
      "{\"sensors\":[{\"id\":\"livingroom\",\"name\":\"거실\","
      "\"temperature\":21.0,\"updated_at\":\"2026-11-26T22:41:37+09:00\"}]}";
  EXPECT_TRUE(app.on_remote_sensors_json(json, kNow));

  // Board-only default ignores the remote reading.
  app.step(1000, kNow);
  EXPECT_EQ(1800, app.last_temp().temp_centi);
  app.take_actions();

  const char* ids[] = {"livingroom"};
  EXPECT_TRUE(app.set_sensor_source(SensorSourceSetting::kSingleRemote, ids, 1));
  EXPECT_TRUE(app.take_actions().settings_dirty);
  EXPECT_EQ(static_cast<int>(SensorSourceSetting::kSingleRemote),
            static_cast<int>(app.settings().zones[0].sensor_source));
  EXPECT_EQ(1, app.settings().zones[0].selected_remote_count);
  EXPECT_STREQ("livingroom", app.settings().zones[0].selected_remote_ids[0]);

  app.step(2000, kNow);
  EXPECT_TRUE(app.last_temp().valid);
  EXPECT_EQ(2100, app.last_temp().temp_centi);
  EXPECT_STREQ("거실 센서 · 1분 전", app.last_temp().source_label.c_str());

  // Same selection again is a no-op.
  EXPECT_TRUE(app.set_sensor_source(SensorSourceSetting::kSingleRemote, ids, 1));
  EXPECT_FALSE(app.take_actions().settings_dirty);

  // Invalid remote selections change nothing.
  EXPECT_FALSE(app.set_sensor_source(SensorSourceSetting::kAverageRemote,
                                     nullptr, 0));
  const std::string long_id(thermostat::storage::kMaxSensorIdLen, 'x');
  const char* long_ids[] = {long_id.c_str()};
  EXPECT_FALSE(app.set_sensor_source(SensorSourceSetting::kSingleRemote,
                                     long_ids, 1));
  EXPECT_FALSE(app.take_actions().settings_dirty);
  EXPECT_STREQ("livingroom", app.settings().zones[0].selected_remote_ids[0]);

  // Back to the board sensor clears the remote list.
  EXPECT_TRUE(app.set_sensor_source(SensorSourceSetting::kBoardOnly, nullptr, 0));
  EXPECT_TRUE(app.take_actions().settings_dirty);
  EXPECT_EQ(0, app.settings().zones[0].selected_remote_count);
  app.step(3000, kNow);
  EXPECT_EQ(1800, app.last_temp().temp_centi);
}

THERMOSTAT_TEST_MAIN()
