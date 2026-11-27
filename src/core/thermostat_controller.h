#pragma once

// Pure C++17 heating control state machine.
//
// The controller never reads the clock or a sensor itself: every step takes a
// `ZoneInput` with the caller-provided monotonic `now_ms` and the current
// temperature. That keeps the whole decision path testable on a PC host and
// free of Arduino / ESP-IDF dependencies.
//
// Safety ordering implemented by `ZoneController::step`:
//   1. Sensor fault: temperature/off modes switch heat off immediately
//      (safety beats minimum on-time); repeat mode keeps its cycle running but
//      raises `sensor_fault_warning`.
//   2. Freeze protection: overrides every mode; entry ignores minimum off-time,
//      release waits until `freeze_protect_centi + hysteresis_centi`.
//   3. Mode decision: off / hysteresis / 60 minute repeat cycle.
//   4. Minimum on/off time gating (bypassed by freeze entry, sensor fault and
//      the explicit off mode).
//   5. Keep-alive pulse: after `keep_alive_interval_sec` of continuous logical
//      heating, `matter_heating` goes false for `keep_alive_off_sec` so the
//      SmartThings "plug on for 70 minutes" dead-man routine resets. Applies to
//      temperature, repeat (e.g. N=60) and freeze-protection heating alike.

#include <cstddef>
#include <cstdint>

#include "thermostat_types.h"

namespace thermostat::core {

// Swappable temperature-mode decision strategy (time-proportional control is
// planned as a later implementation).
class ITempStrategy {
 public:
  virtual ~ITempStrategy() = default;

  // Returns the desired heating state given the temperature and config.
  virtual bool decide_heating(bool currently_heating,
                              int16_t current_temp_centi,
                              const ZoneConfig& cfg) const = 0;
};

// Default strategy: on at or below target - hysteresis, off at or above
// target + hysteresis, hold the previous state in between.
class HysteresisStrategy final : public ITempStrategy {
 public:
  bool decide_heating(bool currently_heating,
                      int16_t current_temp_centi,
                      const ZoneConfig& cfg) const override;
};

// One zone's controller (one heating actuator / one Matter thermostat
// endpoint).
class ZoneController {
 public:
  explicit ZoneController(const ZoneConfig& config = {});

  // Stores the config after clamping out-of-range values:
  // target 500..3000, repeat 0..60, freeze 500..1500, hysteresis >= 10.
  void set_config(const ZoneConfig& config);
  const ZoneConfig& config() const { return config_; }

  // nullptr restores the built-in HysteresisStrategy.
  void set_strategy(const ITempStrategy* strategy);

  ZoneOutput step(const ZoneInput& input);

 private:
  bool repeat_desired(uint64_t now_ms) const;

  ZoneConfig config_{};
  HysteresisStrategy default_strategy_{};
  const ITempStrategy* strategy_ = nullptr;

  bool initialized_ = false;
  bool logical_heating_ = false;
  bool freeze_protect_active_ = false;
  // Set by set_config when mode, target or repeat minutes changed; the next
  // step ignores the minimum on/off time once.
  bool settings_changed_ = false;
  uint64_t last_transition_ms_ = 0;
  uint64_t repeat_cycle_anchor_ms_ = 0;

  bool keep_alive_anchor_valid_ = false;
  bool keep_alive_pulse_active_ = false;
  uint64_t keep_alive_anchor_ms_ = 0;
  uint64_t keep_alive_pulse_start_ms_ = 0;
};

// Fixed-capacity group of zone controllers. `active_zones` is the number of
// zones currently in use; `step_all` steps at most that many zones.
class MultiZoneController {
 public:
  explicit MultiZoneController(size_t active_zones = 1);

  // Returns false (leaving the count unchanged) when count > kMaxZones.
  bool set_active_zones(size_t count);
  size_t active_zones() const { return active_zones_; }

  // Out-of-range indices are clamped to the last zone.
  ZoneController& zone(size_t idx);
  const ZoneController& zone(size_t idx) const;

  // Steps the first min(count, active_zones()) zones. Null pointers or
  // count == 0 are a no-op, so a partially configured system never crashes.
  void step_all(const ZoneInput* inputs, ZoneOutput* outputs, size_t count);

  // Most recent output of a zone (out-of-range indices clamp like zone()).
  const ZoneOutput& output(size_t idx) const;

 private:
  static size_t clamp_index(size_t idx);

  ZoneController zones_[kMaxZones];
  ZoneOutput outputs_[kMaxZones];
  size_t active_zones_ = 1;
};

}  // namespace thermostat::core
