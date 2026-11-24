#pragma once

// Configuration and state types for the thermostat core control logic.
//
// Everything here is plain data and pure C++17: no Arduino, ESP-IDF or
// standard-library platform pieces. Temperatures are centi-degC (0.01 degC,
// e.g. 2150 = 21.50 degC), matching Matter. Times are either monotonic
// milliseconds since boot (`now_ms`, injected by the caller) or absolute
// Unix UTC seconds (`epoch_sec`).

#include <cstddef>
#include <cstdint>

namespace thermostat::core {

// Maximum number of heating zones the control layer can hold. The first
// product revision runs a single zone, but the array layout keeps per-room
// actuators (one Matter endpoint each) open without reshaping the API.
constexpr size_t kMaxZones = 4;

// Sentinel for "no valid temperature" (INT16_MIN).
constexpr int16_t kInvalidTempCenti = -32768;

// Operating modes, mirroring the three modes of a Korean room thermostat.
enum class Mode : uint8_t {
  kOff = 0,
  kTemperature = 1,
  kRepeat = 2,
};

// Control strategy selector. Only hysteresis exists today; keeping the
// selector in the config makes a later time-proportional strategy a swap
// instead of a config-model change.
enum class StrategyKind : uint8_t {
  kHysteresis = 0,
};

// Why the controller requested heat (or kept holding it off) on the latest
// step. Reported to logs and, later, to the UI.
enum class HeatReason : uint8_t {
  kNone = 0,
  kTemperatureBelowTarget,
  kRepeatCycleOn,
  kFreezeProtection,
  kMinOnHold,
  kKeepAlivePulseOff,
  kMinOffHold,
  kSensorFaultOff,
};

// Per-zone settings. All fields are safe by default: off mode, 21.0 degC
// target, 10.0 degC freeze protection and 5 minute minimum run/idle times.
struct ZoneConfig {
  Mode mode = Mode::kOff;
  int16_t target_temp_centi = 2100;      // 21.0 degC, range 500..3000
  uint8_t repeat_on_minutes = 15;        // on-time inside a 60 min cycle, 0..60
  int16_t freeze_protect_centi = 1000;   // 10.0 degC, range 500..1500
  int16_t hysteresis_centi = 50;         // +/-0.5 degC
  uint16_t min_on_sec = 300;             // 5 min
  uint16_t min_off_sec = 300;            // 5 min
  uint16_t keep_alive_interval_sec = 3000;  // pulse after 50 min of heating
  uint16_t keep_alive_off_sec = 10;         // pulse width, 10 s
  StrategyKind strategy = StrategyKind::kHysteresis;
};

// Per-step input from the sensor layer and the clock.
struct ZoneInput {
  uint64_t now_ms = 0;
  int16_t current_temp_centi = kInvalidTempCenti;
  bool sensor_valid = false;
};

// Per-step result. `logical_heating` is the user-facing "heating" state;
// `matter_heating` is what gets published as Matter ThermostatRunningState
// and is briefly false during a keep-alive pulse.
struct ZoneOutput {
  bool logical_heating = false;
  bool matter_heating = false;
  bool freeze_protect_active = false;
  bool sensor_fault_warning = false;
  bool keep_alive_pulsing = false;
  HeatReason reason = HeatReason::kNone;
};

}  // namespace thermostat::core
