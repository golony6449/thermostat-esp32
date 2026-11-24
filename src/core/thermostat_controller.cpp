#include "thermostat_controller.h"

namespace thermostat::core {
namespace {

constexpr uint64_t kMinuteMs = 60ULL * 1000ULL;
constexpr uint64_t kRepeatCycleMs = 60ULL * kMinuteMs;

// Monotonic clock subtraction that also survives a caller clock reset.
uint64_t elapsed_ms(uint64_t now_ms, uint64_t since_ms) {
  return now_ms >= since_ms ? now_ms - since_ms : 0;
}

int16_t clamp_centi(int value, int min_value, int max_value) {
  if (value < min_value) {
    return static_cast<int16_t>(min_value);
  }
  if (value > max_value) {
    return static_cast<int16_t>(max_value);
  }
  return static_cast<int16_t>(value);
}

ZoneConfig clamped_config(const ZoneConfig& input) {
  ZoneConfig config = input;
  config.target_temp_centi = clamp_centi(config.target_temp_centi, 500, 3000);
  if (config.repeat_on_minutes > 60) {
    config.repeat_on_minutes = 60;
  }
  config.freeze_protect_centi =
      clamp_centi(config.freeze_protect_centi, 500, 1500);
  if (config.hysteresis_centi < 10) {
    config.hysteresis_centi = 10;
  }
  return config;
}

}  // namespace

bool HysteresisStrategy::decide_heating(bool currently_heating,
                                        int16_t current_temp_centi,
                                        const ZoneConfig& cfg) const {
  const int32_t low = static_cast<int32_t>(cfg.target_temp_centi) -
                      static_cast<int32_t>(cfg.hysteresis_centi);
  const int32_t high = static_cast<int32_t>(cfg.target_temp_centi) +
                       static_cast<int32_t>(cfg.hysteresis_centi);
  if (current_temp_centi <= low) {
    return true;
  }
  if (current_temp_centi >= high) {
    return false;
  }
  return currently_heating;
}

ZoneController::ZoneController(const ZoneConfig& config)
    : config_(clamped_config(config)) {}

void ZoneController::set_config(const ZoneConfig& config) {
  config_ = clamped_config(config);
}

void ZoneController::set_strategy(const ITempStrategy* strategy) {
  strategy_ = strategy;
}

bool ZoneController::repeat_desired(uint64_t now_ms) const {
  if (config_.repeat_on_minutes == 0) {
    return false;
  }
  const uint64_t on_ms =
      static_cast<uint64_t>(config_.repeat_on_minutes) * kMinuteMs;
  const uint64_t in_cycle =
      elapsed_ms(now_ms, repeat_cycle_anchor_ms_) % kRepeatCycleMs;
  return in_cycle < on_ms;
}

ZoneOutput ZoneController::step(const ZoneInput& input) {
  // The repeat cycle starts when the controller sees its first step. Doing it
  // before the mode decision keeps the first step and later steps consistent.
  if (!initialized_) {
    repeat_cycle_anchor_ms_ = input.now_ms;
  }

  ZoneOutput out;

  const bool sensor_fault =
      !input.sensor_valid || input.current_temp_centi == kInvalidTempCenti;
  out.sensor_fault_warning = sensor_fault;

  bool desired_heating = false;
  HeatReason desired_reason = HeatReason::kNone;
  bool bypass_min_hold = false;  // safety transitions skip min on/off waits
  bool freeze_active = false;

  if (sensor_fault) {
    // Without a temperature the freeze decision cannot be made at all.
    freeze_protect_active_ = false;

    if (config_.mode == Mode::kRepeat) {
      // Repeat mode is time based; keep cycling and only warn on screen.
      desired_heating = repeat_desired(input.now_ms);
      desired_reason =
          desired_heating ? HeatReason::kRepeatCycleOn : HeatReason::kNone;
    } else {
      // Temperature and off modes: fail safe, even inside minimum on-time.
      desired_heating = false;
      desired_reason = HeatReason::kSensorFaultOff;
      bypass_min_hold = true;
    }
  } else {
    // Freeze protection latch: enters below freeze_protect_centi, releases at
    // freeze_protect_centi + hysteresis_centi to avoid boundary chatter.
    if (freeze_protect_active_) {
      const int32_t release_centi =
          static_cast<int32_t>(config_.freeze_protect_centi) +
          static_cast<int32_t>(config_.hysteresis_centi);
      if (input.current_temp_centi >= release_centi) {
        freeze_protect_active_ = false;
      }
    } else if (input.current_temp_centi < config_.freeze_protect_centi) {
      freeze_protect_active_ = true;
      bypass_min_hold = true;  // freeze entry ignores minimum off-time
    }
    freeze_active = freeze_protect_active_;

    if (freeze_active) {
      desired_heating = true;
      desired_reason = HeatReason::kFreezeProtection;
    } else {
      switch (config_.mode) {
        case Mode::kOff:
          desired_heating = false;
          desired_reason = HeatReason::kNone;
          bypass_min_hold = true;  // explicit off mode stops heat immediately
          break;
        case Mode::kTemperature: {
          const ITempStrategy& strategy =
              strategy_ != nullptr ? *strategy_ : default_strategy_;
          desired_heating = strategy.decide_heating(
              logical_heating_, input.current_temp_centi, config_);
          desired_reason = desired_heating ? HeatReason::kTemperatureBelowTarget
                                           : HeatReason::kNone;
          break;
        }
        case Mode::kRepeat:
          desired_heating = repeat_desired(input.now_ms);
          desired_reason =
              desired_heating ? HeatReason::kRepeatCycleOn : HeatReason::kNone;
          break;
      }
    }
  }

  if (!initialized_) {
    initialized_ = true;
    logical_heating_ = desired_heating;
    last_transition_ms_ = input.now_ms;
  } else if (desired_heating != logical_heating_) {
    const bool turning_off = logical_heating_;
    const uint64_t held_ms =
        turning_off ? static_cast<uint64_t>(config_.min_on_sec) * 1000ULL
                    : static_cast<uint64_t>(config_.min_off_sec) * 1000ULL;
    const bool blocked =
        !bypass_min_hold &&
        elapsed_ms(input.now_ms, last_transition_ms_) < held_ms;
    if (blocked) {
      desired_reason =
          turning_off ? HeatReason::kMinOnHold : HeatReason::kMinOffHold;
    } else {
      logical_heating_ = desired_heating;
      last_transition_ms_ = input.now_ms;
    }
  }

  out.logical_heating = logical_heating_;
  out.matter_heating = logical_heating_;
  out.freeze_protect_active = freeze_active;
  out.keep_alive_pulsing = false;
  out.reason = desired_reason;

  if (!logical_heating_) {
    keep_alive_anchor_valid_ = false;
    keep_alive_pulse_active_ = false;
    keep_alive_anchor_ms_ = 0;
    keep_alive_pulse_start_ms_ = 0;
    return out;
  }

  if (!keep_alive_anchor_valid_) {
    keep_alive_anchor_valid_ = true;
    keep_alive_anchor_ms_ = input.now_ms;
    keep_alive_pulse_active_ = false;
  }

  const uint64_t off_pulse_ms =
      static_cast<uint64_t>(config_.keep_alive_off_sec) * 1000ULL;

  if (keep_alive_pulse_active_) {
    if (elapsed_ms(input.now_ms, keep_alive_pulse_start_ms_) >= off_pulse_ms) {
      // Pulse finished: resume publishing heat and restart the 50 min count.
      keep_alive_pulse_active_ = false;
      keep_alive_anchor_ms_ = input.now_ms;
      out.keep_alive_pulsing = false;
      out.matter_heating = true;
    } else {
      out.keep_alive_pulsing = true;
      out.matter_heating = false;
      out.reason = HeatReason::kKeepAlivePulseOff;
    }
  } else {
    const uint64_t interval_ms =
        static_cast<uint64_t>(config_.keep_alive_interval_sec) * 1000ULL;
    if (elapsed_ms(input.now_ms, keep_alive_anchor_ms_) >= interval_ms) {
      keep_alive_pulse_active_ = true;
      keep_alive_pulse_start_ms_ = input.now_ms;
      out.keep_alive_pulsing = true;
      out.matter_heating = false;
      out.reason = HeatReason::kKeepAlivePulseOff;
    }
  }

  return out;
}

MultiZoneController::MultiZoneController(size_t active_zones)
    : active_zones_(active_zones > kMaxZones ? kMaxZones : active_zones) {}

bool MultiZoneController::set_active_zones(size_t count) {
  if (count > kMaxZones) {
    return false;
  }
  active_zones_ = count;
  return true;
}

size_t MultiZoneController::clamp_index(size_t idx) {
  return idx < kMaxZones ? idx : kMaxZones - 1;
}

ZoneController& MultiZoneController::zone(size_t idx) {
  return zones_[clamp_index(idx)];
}

const ZoneController& MultiZoneController::zone(size_t idx) const {
  return zones_[clamp_index(idx)];
}

void MultiZoneController::step_all(const ZoneInput* inputs,
                                   ZoneOutput* outputs, size_t count) {
  if (inputs == nullptr || outputs == nullptr || count == 0) {
    return;
  }
  if (count > kMaxZones) {
    count = kMaxZones;
  }
  if (count > active_zones_) {
    count = active_zones_;
  }
  for (size_t i = 0; i < count; ++i) {
    outputs_[i] = zones_[i].step(inputs[i]);
    outputs[i] = outputs_[i];
  }
}

const ZoneOutput& MultiZoneController::output(size_t idx) const {
  return outputs_[clamp_index(idx)];
}

}  // namespace thermostat::core
