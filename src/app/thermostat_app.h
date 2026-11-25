#pragma once

// Pure C++17 integration coordinator for the DIY SmartThings thermostat.
//
// ThermostatApp ties the platform-independent pieces together:
//
//   sensor input (board / home-server / simulation)
//     -> thermostat::core control state machine
//     -> Matter attributes, NVS persistence and the LVGL UI view model.
//
// It deliberately has no Arduino, LVGL, Matter or Preferences dependency. The
// firmware forwards callbacks from the Matter task and the LVGL task into the
// on_* methods (which only touch app state) and applies the SyncActions
// returned by take_actions() from the single loop() task. Host tests compile
// this module with `g++ -std=c++17 -Wall -Wextra -Werror`.
//
// Echo-loop rules (see CLAUDE.md 8):
//   * UI-originated changes set push_matter_* so the app reflects the panel.
//   * Matter-originated changes never set push_matter_*; the app is already
//     the source of truth for its own attributes.
//   * Matter Heat restores the last heat mode, so switching the app to Heat
//     does not clobber repeat mode.

#include <cstdint>
#include <string>

#include "core/thermostat_controller.h"
#include "core/thermostat_types.h"
#include "sensor/sensor_logic.h"
#include "storage/settings_codec.h"
#include "ui/waveshare43b/ui_view_model.h"

namespace thermostat::app {

// Work collected for the firmware after one loop iteration. Every flag is
// one-shot: take_actions() returns the flags and clears them.
struct SyncActions {
  bool settings_dirty = false;

  bool push_matter_mode = false;
  bool matter_heat_mode = false;

  bool push_matter_setpoint = false;
  int16_t matter_setpoint_centi = 2100;

  bool push_matter_temp = false;
  int16_t matter_temp_centi = thermostat::core::kInvalidTempCenti;

  bool push_matter_heating = false;
  bool matter_heating = false;

  bool ui_dirty = false;
};

class ThermostatApp {
 public:
  ThermostatApp() = default;

  // Copies and sanitizes `settings`, applies zone 0 to the controller and the
  // sensor selector, and queues the initial Matter/UI synchronization.
  // `now_ms` is reserved for future time-based UI hints; the controller starts
  // its own timers on the first step().
  void init(const thermostat::storage::DeviceSettings& settings,
            uint64_t now_ms);

  // --- UI (LVGL task) inputs ------------------------------------------------

  void on_ui_target_temp(int16_t new_target_centi);
  void on_ui_mode(thermostat::ui::UiMode new_mode);
  void on_ui_repeat_minutes(uint8_t new_minutes);
  void on_ui_freeze_protect(int16_t new_freeze_centi);

  // --- Matter (CHIP task) inputs -------------------------------------------
  // These never schedule a push back to Matter (echo-loop prevention).

  void on_matter_mode(bool heat_mode);
  void on_matter_setpoint(int16_t setpoint_centi);

  // --- Sensor inputs --------------------------------------------------------

  void on_board_temp_sample(int16_t raw_centi, int64_t now_epoch_sec);

  // Parses the SCRUM-239 home-server response. Returns true on a structurally
  // valid response; on failure the selector's consecutive-failure counter is
  // incremented so the automatic board fallback can engage.
  bool on_remote_sensors_json(const std::string& json,
                              int64_t now_epoch_sec);
  void on_remote_fetch_failed();

  // --- Configuration --------------------------------------------------------

  // Sanitizes and stores the base URL; marks the settings dirty on change.
  void set_homeserver_url(const char* url);

  // Debug override used on the bench when no SHT40 / home server is attached:
  // the simulated value replaces the resolved sensor temperature.
  void set_simulated_temp(int16_t temp_centi);
  void clear_simulated_temp();
  bool has_simulated_temp() const { return simulated_temp_active_; }

  // --- Control step ---------------------------------------------------------

  // Resolves the temperature, steps the control state machine and records the
  // Matter/UI changes that result. Call regularly (about once per second).
  void step(uint64_t now_ms, int64_t now_epoch_sec);

  // --- Results and accessors ------------------------------------------------

  thermostat::ui::UiViewModel build_view_model(int64_t now_epoch_sec) const;

  // Returns the queued work and clears it.
  SyncActions take_actions();

  const thermostat::storage::DeviceSettings& settings() const {
    return settings_;
  }
  const thermostat::core::ZoneOutput& last_output() const {
    return last_output_;
  }
  const thermostat::sensor::ResolvedTemperature& last_temp() const {
    return last_temp_;
  }
  bool is_matter_heat_mode() const {
    return settings_.zones[0].control.mode != thermostat::core::Mode::kOff;
  }

 private:
  void apply_zone_control();
  bool commit_target_temp(int16_t new_target_centi);

  thermostat::storage::DeviceSettings settings_{};
  thermostat::core::MultiZoneController controller_{1};
  thermostat::sensor::SensorSelector sensor_selector_{};

  // Heat mode restored when the app switches Matter back to Heat after Off.
  thermostat::core::Mode last_heat_mode_ =
      thermostat::core::Mode::kTemperature;

  thermostat::core::ZoneOutput last_output_{};
  thermostat::core::ZoneOutput zone_outputs_[thermostat::core::kMaxZones]{};
  thermostat::sensor::ResolvedTemperature last_temp_{};
  thermostat::ui::UiViewModel last_view_model_{};

  bool simulated_temp_active_ = false;
  int16_t simulated_temp_centi_ = thermostat::core::kInvalidTempCenti;

  // Queued actions (consumed by take_actions()).
  bool settings_dirty_ = false;
  bool push_matter_mode_ = false;
  bool matter_heat_mode_ = false;
  bool push_matter_setpoint_ = false;
  int16_t matter_setpoint_centi_ = 2100;
  bool push_matter_temp_ = false;
  int16_t matter_temp_centi_ = thermostat::core::kInvalidTempCenti;
  bool push_matter_heating_ = false;
  bool matter_heating_ = false;
  bool ui_dirty_ = false;

  // Value last published to Matter, used for the 0.1 degC update threshold.
  bool has_published_matter_temp_ = false;
  int16_t last_published_matter_temp_ = thermostat::core::kInvalidTempCenti;
  bool has_published_matter_heating_ = false;
  bool last_published_matter_heating_ = false;
};

}  // namespace thermostat::app
