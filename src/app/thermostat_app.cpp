#include "thermostat_app.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace thermostat::app {
namespace {

// Anything below 2023-11-14 means SNTP has not delivered a real time yet.
constexpr int64_t kSaneEpochSec = 1700000000;
constexpr int64_t kKstOffsetSec = 9 * 3600;
constexpr int64_t kSecondsPerDay = 24 * 3600;

// Matter is only updated when the temperature moved by 0.1 degC since the
// last publish; the 0.01 degC resolution would otherwise flood the hub.
constexpr int kMatterTempPublishDeltaCenti = 10;

core::Mode mode_from_ui(ui::UiMode mode) {
  switch (mode) {
    case ui::UiMode::kOff:
      return core::Mode::kOff;
    case ui::UiMode::kTemperature:
      return core::Mode::kTemperature;
    case ui::UiMode::kRepeat:
      return core::Mode::kRepeat;
  }
  return core::Mode::kOff;
}

ui::UiMode mode_to_ui(core::Mode mode) {
  switch (mode) {
    case core::Mode::kOff:
      return ui::UiMode::kOff;
    case core::Mode::kTemperature:
      return ui::UiMode::kTemperature;
    case core::Mode::kRepeat:
      return ui::UiMode::kRepeat;
  }
  return ui::UiMode::kOff;
}

sensor::SourceMode source_mode_from_setting(storage::SensorSourceSetting value) {
  switch (value) {
    case storage::SensorSourceSetting::kBoardOnly:
      return sensor::SourceMode::kBoardOnly;
    case storage::SensorSourceSetting::kSingleRemote:
      return sensor::SourceMode::kSingleRemote;
    case storage::SensorSourceSetting::kAverageRemote:
      return sensor::SourceMode::kAverageRemote;
  }
  return sensor::SourceMode::kBoardOnly;
}

bool is_heat_mode(core::Mode mode) { return mode != core::Mode::kOff; }

// Copies a UTF-8 string into a fixed buffer, always null-terminating. Avoids
// snprintf format-truncation warnings for dynamic strings.
void copy_text(char* dest, size_t capacity, const std::string& text) {
  if (dest == nullptr || capacity == 0) {
    return;
  }
  const size_t length =
      text.size() < capacity - 1 ? text.size() : capacity - 1;
  std::memcpy(dest, text.data(), length);
  dest[length] = '\0';
}

bool view_model_equal(const ui::UiViewModel& a, const ui::UiViewModel& b) {
  return a.temp_valid == b.temp_valid &&
         a.current_temp_centi == b.current_temp_centi &&
         a.target_temp_centi == b.target_temp_centi && a.mode == b.mode &&
         a.heating_active == b.heating_active &&
         a.repeat_on_minutes == b.repeat_on_minutes &&
         a.freeze_protect_centi == b.freeze_protect_centi &&
         a.source_severity == b.source_severity &&
         std::strcmp(a.source_text, b.source_text) == 0 &&
         std::strcmp(a.time_text, b.time_text) == 0;
}

}  // namespace

void ThermostatApp::init(const storage::DeviceSettings& settings,
                         uint64_t now_ms) {
  // Reserved for future time-based UI hints; the controller starts its own
  // timers on the first step().
  static_cast<void>(now_ms);

  settings_ = settings;
  storage::sanitize_settings(&settings_);

  controller_.set_active_zones(settings_.active_zones);
  apply_zone_control();

  const core::Mode mode = settings_.zones[0].control.mode;
  last_heat_mode_ =
      mode == core::Mode::kRepeat ? core::Mode::kRepeat : core::Mode::kTemperature;

  apply_sensor_config();

  last_output_ = core::ZoneOutput{};
  for (size_t i = 0; i < core::kMaxZones; ++i) {
    zone_outputs_[i] = core::ZoneOutput{};
  }
  last_temp_ = sensor::ResolvedTemperature{};
  last_view_model_ = ui::UiViewModel{};

  simulated_temp_active_ = false;
  simulated_temp_centi_ = core::kInvalidTempCenti;

  // The firmware needs the Matter endpoint synchronized right after boot.
  settings_dirty_ = false;
  push_matter_mode_ = true;
  matter_heat_mode_ = is_matter_heat_mode();
  push_matter_setpoint_ = true;
  matter_setpoint_centi_ = settings_.zones[0].control.target_temp_centi;
  push_matter_temp_ = false;
  matter_temp_centi_ = core::kInvalidTempCenti;
  push_matter_heating_ = false;
  matter_heating_ = false;
  has_published_matter_temp_ = false;
  last_published_matter_temp_ = core::kInvalidTempCenti;
  has_published_matter_heating_ = false;
  last_published_matter_heating_ = false;
  ui_dirty_ = true;
}

void ThermostatApp::apply_zone_control() {
  controller_.zone(0).set_config(settings_.zones[0].control);
}

bool ThermostatApp::commit_target_temp(int16_t new_target_centi) {
  storage::DeviceSettings candidate = settings_;
  candidate.zones[0].control.target_temp_centi = new_target_centi;
  storage::sanitize_settings(&candidate);

  const int16_t target = candidate.zones[0].control.target_temp_centi;
  if (target == settings_.zones[0].control.target_temp_centi) {
    return false;
  }
  settings_.zones[0].control.target_temp_centi = target;
  apply_zone_control();
  settings_dirty_ = true;
  ui_dirty_ = true;
  return true;
}

void ThermostatApp::on_ui_target_temp(int16_t new_target_centi) {
  if (!commit_target_temp(new_target_centi)) {
    return;
  }
  push_matter_setpoint_ = true;
  matter_setpoint_centi_ = settings_.zones[0].control.target_temp_centi;
}

void ThermostatApp::on_ui_mode(ui::UiMode new_mode) {
  const core::Mode mode = mode_from_ui(new_mode);
  const core::Mode current = settings_.zones[0].control.mode;
  if (mode == current) {
    return;
  }

  const bool was_heat = is_heat_mode(current);
  settings_.zones[0].control.mode = mode;
  if (is_heat_mode(mode)) {
    last_heat_mode_ = mode;
  }
  apply_zone_control();
  settings_dirty_ = true;
  ui_dirty_ = true;

  // Only the Matter SystemMode (Off/Heat) needs a push; Temperature <-> Repeat
  // stays inside Heat and is a firmware-only setting.
  if (was_heat != is_heat_mode(mode)) {
    push_matter_mode_ = true;
    matter_heat_mode_ = is_heat_mode(mode);
  }
}

void ThermostatApp::on_ui_repeat_minutes(uint8_t new_minutes) {
  storage::DeviceSettings candidate = settings_;
  candidate.zones[0].control.repeat_on_minutes = new_minutes;
  storage::sanitize_settings(&candidate);

  const uint8_t minutes = candidate.zones[0].control.repeat_on_minutes;
  if (minutes == settings_.zones[0].control.repeat_on_minutes) {
    return;
  }
  settings_.zones[0].control.repeat_on_minutes = minutes;
  apply_zone_control();
  settings_dirty_ = true;
  ui_dirty_ = true;
}

void ThermostatApp::on_ui_freeze_protect(int16_t new_freeze_centi) {
  storage::DeviceSettings candidate = settings_;
  candidate.zones[0].control.freeze_protect_centi = new_freeze_centi;
  storage::sanitize_settings(&candidate);

  const int16_t freeze = candidate.zones[0].control.freeze_protect_centi;
  if (freeze == settings_.zones[0].control.freeze_protect_centi) {
    return;
  }
  settings_.zones[0].control.freeze_protect_centi = freeze;
  apply_zone_control();
  settings_dirty_ = true;
  ui_dirty_ = true;
}

void ThermostatApp::on_matter_mode(bool heat_mode) {
  const core::Mode current = settings_.zones[0].control.mode;
  core::Mode mode = current;
  if (!heat_mode) {
    mode = core::Mode::kOff;
  } else if (!is_heat_mode(current)) {
    // Restore the last heat mode so a controller switching Heat on does not
    // discard repeat mode.
    mode = last_heat_mode_;
  }

  if (mode == current) {
    return;
  }
  settings_.zones[0].control.mode = mode;
  apply_zone_control();
  settings_dirty_ = true;
  ui_dirty_ = true;
  // No push_matter_mode on purpose: the change came from Matter.
}

void ThermostatApp::on_matter_setpoint(int16_t setpoint_centi) {
  // No push_matter_setpoint on purpose: the change came from Matter.
  commit_target_temp(setpoint_centi);
}

void ThermostatApp::on_board_temp_sample(int16_t raw_centi,
                                         int64_t now_epoch_sec) {
  sensor_selector_.update_board_temp(raw_centi, now_epoch_sec);
}

bool ThermostatApp::on_remote_sensors_json(const std::string& json,
                                           int64_t now_epoch_sec) {
  std::vector<sensor::RemoteSensorReading> readings;
  if (!sensor::parse_homeserver_sensors_json(json, &readings)) {
    sensor_selector_.record_remote_failure();
    return false;
  }
  sensor_selector_.record_remote_success(readings, now_epoch_sec);
  return true;
}

void ThermostatApp::on_remote_fetch_failed() {
  sensor_selector_.record_remote_failure();
}

void ThermostatApp::set_homeserver_url(const char* url) {
  const std::string text = url == nullptr ? std::string() : std::string(url);
  storage::DeviceSettings candidate = settings_;
  copy_text(candidate.homeserver_url, sizeof(candidate.homeserver_url), text);
  storage::sanitize_settings(&candidate);

  if (std::strcmp(candidate.homeserver_url, settings_.homeserver_url) == 0) {
    return;
  }
  std::memcpy(settings_.homeserver_url, candidate.homeserver_url,
              sizeof(settings_.homeserver_url));
  settings_dirty_ = true;
}

void ThermostatApp::apply_sensor_config() {
  const storage::ZonePersistedSettings& zone = settings_.zones[0];
  sensor::SensorSelectionConfig sensor_config;
  sensor_config.mode = source_mode_from_setting(zone.sensor_source);
  sensor_config.board_offset_centi = zone.sensor_offset_centi;
  for (uint8_t i = 0; i < zone.selected_remote_count; ++i) {
    if (zone.selected_remote_ids[i][0] != '\0') {
      sensor_config.selected_remote_ids.emplace_back(
          zone.selected_remote_ids[i]);
    }
  }
  sensor_selector_.set_config(sensor_config);
}

bool ThermostatApp::set_sensor_source(storage::SensorSourceSetting source,
                                      const char* const* remote_ids,
                                      uint8_t remote_count) {
  storage::DeviceSettings candidate = settings_;
  storage::ZonePersistedSettings& zone = candidate.zones[0];
  zone.sensor_source = source;
  zone.selected_remote_count = 0;
  std::memset(zone.selected_remote_ids, 0, sizeof(zone.selected_remote_ids));
  if (source != storage::SensorSourceSetting::kBoardOnly) {
    if (remote_ids == nullptr || remote_count == 0 ||
        remote_count > storage::kMaxSelectedRemoteSensors) {
      return false;
    }
    for (uint8_t i = 0; i < remote_count; ++i) {
      if (remote_ids[i] == nullptr || remote_ids[i][0] == '\0' ||
          std::strlen(remote_ids[i]) >= storage::kMaxSensorIdLen) {
        return false;
      }
      copy_text(zone.selected_remote_ids[i], storage::kMaxSensorIdLen,
                std::string(remote_ids[i]));
    }
    zone.selected_remote_count = remote_count;
  }
  storage::sanitize_settings(&candidate);

  const storage::ZonePersistedSettings& before = settings_.zones[0];
  if (before.sensor_source == zone.sensor_source &&
      before.selected_remote_count == zone.selected_remote_count &&
      std::memcmp(before.selected_remote_ids, zone.selected_remote_ids,
                  sizeof(zone.selected_remote_ids)) == 0) {
    return true;
  }
  settings_.zones[0] = zone;
  apply_sensor_config();
  settings_dirty_ = true;
  return true;
}

void ThermostatApp::set_simulated_temp(int16_t temp_centi) {
  simulated_temp_centi_ = temp_centi;
  simulated_temp_active_ = temp_centi != core::kInvalidTempCenti;
}

void ThermostatApp::clear_simulated_temp() {
  simulated_temp_active_ = false;
  simulated_temp_centi_ = core::kInvalidTempCenti;
}

void ThermostatApp::step(uint64_t now_ms, int64_t now_epoch_sec) {
  if (simulated_temp_active_) {
    last_temp_ = sensor::ResolvedTemperature{};
    last_temp_.valid = true;
    last_temp_.temp_centi = simulated_temp_centi_;
    last_temp_.used_fallback_board = false;
    last_temp_.freshness = sensor::Freshness::kFresh;
    last_temp_.age_sec = 0;
    last_temp_.source_label = "시뮬레이션 · 방금";
  } else {
    last_temp_ = sensor_selector_.resolve(now_epoch_sec);
  }

  core::ZoneInput inputs[core::kMaxZones];
  core::ZoneOutput outputs[core::kMaxZones];
  const size_t active_zones = settings_.active_zones;
  const int16_t temp_centi =
      last_temp_.valid ? last_temp_.temp_centi : core::kInvalidTempCenti;
  for (size_t i = 0; i < active_zones; ++i) {
    inputs[i].now_ms = now_ms;
    inputs[i].current_temp_centi = temp_centi;
    inputs[i].sensor_valid = last_temp_.valid;
  }
  controller_.step_all(inputs, outputs, active_zones);
  for (size_t i = 0; i < active_zones; ++i) {
    zone_outputs_[i] = outputs[i];
  }
  last_output_ = zone_outputs_[0];

  // LocalTemperature: publish the first valid value and then only changes of
  // at least 0.1 degC.
  if (last_temp_.valid) {
    const bool first_publish = !has_published_matter_temp_;
    const int delta = static_cast<int>(last_temp_.temp_centi) -
                      static_cast<int>(last_published_matter_temp_);
    if (first_publish || delta >= kMatterTempPublishDeltaCenti ||
        delta <= -kMatterTempPublishDeltaCenti) {
      push_matter_temp_ = true;
      matter_temp_centi_ = last_temp_.temp_centi;
      has_published_matter_temp_ = true;
      last_published_matter_temp_ = last_temp_.temp_centi;
    }
  }

  // ThermostatRunningState: publish every logical change, including the brief
  // false of a keep-alive pulse (that is what resets the SmartThings 70-minute
  // routine timer).
  if (!has_published_matter_heating_ ||
      last_published_matter_heating_ != last_output_.matter_heating) {
    push_matter_heating_ = true;
    matter_heating_ = last_output_.matter_heating;
    has_published_matter_heating_ = true;
    last_published_matter_heating_ = last_output_.matter_heating;
  }

  const ui::UiViewModel view = build_view_model(now_epoch_sec);
  if (!view_model_equal(view, last_view_model_)) {
    last_view_model_ = view;
    ui_dirty_ = true;
  }
}

ui::UiViewModel ThermostatApp::build_view_model(int64_t now_epoch_sec) const {
  ui::UiViewModel view;
  view.temp_valid = last_temp_.valid;
  view.current_temp_centi =
      last_temp_.valid ? last_temp_.temp_centi : core::kInvalidTempCenti;
  view.target_temp_centi = settings_.zones[0].control.target_temp_centi;
  view.mode = mode_to_ui(settings_.zones[0].control.mode);
  // Keep showing "heating" during a keep-alive pulse even though Matter is
  // briefly told otherwise.
  view.heating_active = last_output_.logical_heating;
  view.repeat_on_minutes = settings_.zones[0].control.repeat_on_minutes;
  view.freeze_protect_centi = settings_.zones[0].control.freeze_protect_centi;

  switch (last_temp_.freshness) {
    case sensor::Freshness::kFresh:
      view.source_severity = ui::SourceSeverity::kNormal;
      break;
    case sensor::Freshness::kWarn:
      view.source_severity = ui::SourceSeverity::kWarning;
      break;
    case sensor::Freshness::kStaleOrFallback:
    case sensor::Freshness::kFault:
      view.source_severity = ui::SourceSeverity::kError;
      break;
  }
  copy_text(view.source_text, sizeof(view.source_text),
            last_temp_.source_label);

  if (now_epoch_sec >= kSaneEpochSec) {
    const int64_t kst_seconds =
        (now_epoch_sec + kKstOffsetSec) % kSecondsPerDay;
    const int hour = static_cast<int>(kst_seconds / 3600);
    const int minute = static_cast<int>((kst_seconds % 3600) / 60);
    std::snprintf(view.time_text, sizeof(view.time_text), "%02d:%02d", hour,
                  minute);
  } else {
    copy_text(view.time_text, sizeof(view.time_text), "--:--");
  }
  return view;
}

SyncActions ThermostatApp::take_actions() {
  SyncActions actions;
  actions.settings_dirty = settings_dirty_;
  actions.push_matter_mode = push_matter_mode_;
  actions.matter_heat_mode = matter_heat_mode_;
  actions.push_matter_setpoint = push_matter_setpoint_;
  actions.matter_setpoint_centi = matter_setpoint_centi_;
  actions.push_matter_temp = push_matter_temp_;
  actions.matter_temp_centi = matter_temp_centi_;
  actions.push_matter_heating = push_matter_heating_;
  actions.matter_heating = matter_heating_;
  actions.ui_dirty = ui_dirty_;

  settings_dirty_ = false;
  push_matter_mode_ = false;
  push_matter_setpoint_ = false;
  push_matter_temp_ = false;
  push_matter_heating_ = false;
  ui_dirty_ = false;
  return actions;
}

}  // namespace thermostat::app
