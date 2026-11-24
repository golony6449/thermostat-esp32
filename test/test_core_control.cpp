// LINK: src/core/*.cpp
// Host-side unit tests for the pure C++ thermostat control state machine.
// Covers all three modes, hysteresis, minimum on/off times, freeze protection,
// the 50 minute keep-alive pulse and sensor-fault safety.

#include "test_harness.h"
#include "thermostat_core.h"

namespace {

using thermostat::core::HeatReason;
using thermostat::core::Mode;
using thermostat::core::ZoneConfig;
using thermostat::core::ZoneController;
using thermostat::core::ZoneInput;
using thermostat::core::ZoneOutput;

constexpr uint64_t kSec = 1000ULL;
constexpr uint64_t kMin = 60ULL * kSec;

ZoneInput InputAt(uint64_t now_ms, int16_t temp_centi, bool valid = true) {
  ZoneInput input;
  input.now_ms = now_ms;
  input.current_temp_centi = temp_centi;
  input.sensor_valid = valid;
  return input;
}

ZoneConfig TempModeConfig() {
  ZoneConfig config;
  config.mode = Mode::kTemperature;
  config.target_temp_centi = 2100;  // 21.0 degC
  config.hysteresis_centi = 50;     // +/-0.5 degC
  config.min_on_sec = 0;
  config.min_off_sec = 0;
  return config;
}

ZoneConfig RepeatConfig(uint8_t on_minutes) {
  ZoneConfig config;
  config.mode = Mode::kRepeat;
  config.repeat_on_minutes = on_minutes;
  config.min_on_sec = 0;
  config.min_off_sec = 0;
  return config;
}

bool ReasonIs(HeatReason expected, const ZoneOutput& output) {
  return output.reason == expected;
}

class AlwaysHeatStrategy final : public thermostat::core::ITempStrategy {
 public:
  bool decide_heating(bool /*currently_heating*/,
                      int16_t /*current_temp_centi*/,
                      const ZoneConfig& /*cfg*/) const override {
    return true;
  }
};

}  // namespace

// 1. Off mode stays off, but freeze protection forces heat and releases only
//    after the hysteresis band.
TEST_CASE(off_mode_holds_and_freeze_protection_latches) {
  ZoneConfig config;
  config.mode = Mode::kOff;
  config.freeze_protect_centi = 1000;  // 10.0 degC
  config.hysteresis_centi = 50;
  config.min_on_sec = 300;
  config.min_off_sec = 300;
  ZoneController controller(config);

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_FALSE(out.freeze_protect_active);
  EXPECT_TRUE(ReasonIs(HeatReason::kNone, out));

  // Below the freeze threshold: heat on immediately even though the minimum
  // off-time (300 s) has not elapsed since the first step.
  out = controller.step(InputAt(60 * kSec, 900));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(out.matter_heating);
  EXPECT_TRUE(out.freeze_protect_active);
  EXPECT_TRUE(ReasonIs(HeatReason::kFreezeProtection, out));

  // 10.4 degC is above the threshold but below threshold + hysteresis: latched.
  out = controller.step(InputAt(120 * kSec, 1040));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(out.freeze_protect_active);
  EXPECT_TRUE(ReasonIs(HeatReason::kFreezeProtection, out));

  // 10.5 degC releases the latch; off mode stops immediately even inside the
  // 5 minute minimum on-time.
  out = controller.step(InputAt(180 * kSec, 1050));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_FALSE(out.freeze_protect_active);
  EXPECT_TRUE(ReasonIs(HeatReason::kNone, out));
}

// 2. Temperature mode hysteresis boundaries (target 21.0 degC, +/-0.5 degC).
TEST_CASE(temperature_mode_hysteresis_boundaries) {
  ZoneController controller(TempModeConfig());

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);  // below 20.5 degC
  EXPECT_TRUE(ReasonIs(HeatReason::kTemperatureBelowTarget, out));

  out = controller.step(InputAt(kMin, 2050));
  EXPECT_TRUE(out.logical_heating);  // exactly target - hysteresis: on

  out = controller.step(InputAt(2 * kMin, 2100));
  EXPECT_TRUE(out.logical_heating);  // inside the band: hold previous

  out = controller.step(InputAt(3 * kMin, 2150));
  EXPECT_FALSE(out.logical_heating);  // exactly target + hysteresis: off

  out = controller.step(InputAt(4 * kMin, 2100));
  EXPECT_FALSE(out.logical_heating);  // inside the band: hold previous
}

// 3. Minimum on/off times prevent flicker; explicit off mode bypasses them.
TEST_CASE(minimum_on_and_off_times_prevent_flicker) {
  ZoneConfig config = TempModeConfig();
  config.min_on_sec = 300;
  config.min_off_sec = 300;
  ZoneController controller(config);

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);

  // Warm enough to stop, but only 60 s of the 5 minute minimum on-time passed.
  out = controller.step(InputAt(60 * kSec, 2200));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(out.matter_heating);
  EXPECT_TRUE(ReasonIs(HeatReason::kMinOnHold, out));

  out = controller.step(InputAt(300 * kSec, 2200));
  EXPECT_FALSE(out.logical_heating);

  // Cold enough to start, but the 5 minute minimum off-time is running.
  out = controller.step(InputAt(360 * kSec, 2000));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_TRUE(ReasonIs(HeatReason::kMinOffHold, out));

  out = controller.step(InputAt(600 * kSec, 2000));
  EXPECT_TRUE(out.logical_heating);

  // Switching to off mode drops heat immediately, ignoring the minimum on-time.
  config.mode = Mode::kOff;
  controller.set_config(config);
  out = controller.step(InputAt(610 * kSec, 2000));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
}

// 4. Repeat mode: 15 of 60 minutes on, and the N=0 / N=60 extremes.
TEST_CASE(repeat_mode_cycles_fifteen_minutes_on) {
  ZoneController controller(RepeatConfig(15));

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(ReasonIs(HeatReason::kRepeatCycleOn, out));

  out = controller.step(InputAt(15 * kMin - 1, 2000));
  EXPECT_TRUE(out.logical_heating);

  out = controller.step(InputAt(15 * kMin, 2000));
  EXPECT_FALSE(out.logical_heating);

  out = controller.step(InputAt(30 * kMin, 2000));
  EXPECT_FALSE(out.logical_heating);

  out = controller.step(InputAt(60 * kMin, 2000));
  EXPECT_TRUE(out.logical_heating);  // next cycle starts

  ZoneController always_off(RepeatConfig(0));
  EXPECT_FALSE(always_off.step(InputAt(0, 2000)).logical_heating);
  EXPECT_FALSE(always_off.step(InputAt(30 * kMin, 2000)).logical_heating);

  ZoneController always_on(RepeatConfig(60));
  EXPECT_TRUE(always_on.step(InputAt(0, 2000)).logical_heating);
  EXPECT_TRUE(always_on.step(InputAt(59 * kMin, 2000)).logical_heating);
}

// The repeat cycle is anchored at the first step so a controller that starts
// later still begins with its on-window.
TEST_CASE(repeat_cycle_starts_at_first_step) {
  ZoneController controller(RepeatConfig(15));
  const uint64_t start = 1234 * kSec;

  EXPECT_TRUE(controller.step(InputAt(start, 2000)).logical_heating);
  EXPECT_TRUE(
      controller.step(InputAt(start + 15 * kMin - 1, 2000)).logical_heating);
  EXPECT_FALSE(
      controller.step(InputAt(start + 15 * kMin, 2000)).logical_heating);
}

// 5. Repeat N=60: 50 minutes of continuous heating triggers the keep-alive
//    pulse (10 s off) so the 70 minute dead-man routine never fires.
TEST_CASE(repeat_sixty_minutes_gets_keep_alive_pulses) {
  ZoneController controller(RepeatConfig(60));

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(out.matter_heating);
  EXPECT_FALSE(out.keep_alive_pulsing);

  out = controller.step(InputAt(50 * kMin - 1, 2000));
  EXPECT_TRUE(out.matter_heating);
  EXPECT_FALSE(out.keep_alive_pulsing);

  out = controller.step(InputAt(50 * kMin, 2000));
  EXPECT_TRUE(out.logical_heating);  // UI still shows heating
  EXPECT_FALSE(out.matter_heating);  // Matter briefly reports idle
  EXPECT_TRUE(out.keep_alive_pulsing);
  EXPECT_TRUE(ReasonIs(HeatReason::kKeepAlivePulseOff, out));

  out = controller.step(InputAt(50 * kMin + 10 * kSec - 1, 2000));
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.keep_alive_pulsing);

  out = controller.step(InputAt(50 * kMin + 10 * kSec, 2000));
  EXPECT_TRUE(out.matter_heating);
  EXPECT_FALSE(out.keep_alive_pulsing);
  EXPECT_TRUE(ReasonIs(HeatReason::kRepeatCycleOn, out));

  // The next pulse comes 50 minutes after the previous pulse ended.
  out = controller.step(InputAt(100 * kMin + 10 * kSec, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.keep_alive_pulsing);
}

// 6. Temperature mode: keep-alive pulses after 50 minutes and again after
//    100 minutes of continuous demand.
TEST_CASE(temperature_mode_gets_repeated_keep_alive_pulses) {
  ZoneConfig config = TempModeConfig();
  config.target_temp_centi = 2500;
  ZoneController controller(config);

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);

  out = controller.step(InputAt(50 * kMin, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.keep_alive_pulsing);

  out = controller.step(InputAt(50 * kMin + 10 * kSec, 2000));
  EXPECT_TRUE(out.matter_heating);
  EXPECT_FALSE(out.keep_alive_pulsing);
  EXPECT_TRUE(ReasonIs(HeatReason::kTemperatureBelowTarget, out));

  out = controller.step(InputAt(100 * kMin + 10 * kSec, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.keep_alive_pulsing);
}

// Freeze-protection heating also counts as continuous heating, so it pulses too.
TEST_CASE(freeze_protection_heating_also_gets_keep_alive_pulses) {
  ZoneConfig config;
  config.mode = Mode::kOff;
  config.freeze_protect_centi = 1000;
  ZoneController controller(config);

  ZoneOutput out = controller.step(InputAt(0, 900));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(out.freeze_protect_active);

  out = controller.step(InputAt(50 * kMin, 900));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.keep_alive_pulsing);

  out = controller.step(InputAt(50 * kMin + 10 * kSec, 900));
  EXPECT_TRUE(out.matter_heating);
  EXPECT_FALSE(out.keep_alive_pulsing);
  EXPECT_TRUE(out.freeze_protect_active);
}

// The keep-alive timer restarts whenever heating stops.
TEST_CASE(keep_alive_timer_resets_when_heating_stops) {
  ZoneController controller(TempModeConfig());

  EXPECT_TRUE(controller.step(InputAt(0, 2000)).logical_heating);
  EXPECT_FALSE(controller.step(InputAt(30 * kMin, 2200)).logical_heating);
  EXPECT_TRUE(controller.step(InputAt(31 * kMin, 2000)).logical_heating);

  ZoneOutput out = controller.step(InputAt(81 * kMin - 1, 2000));
  EXPECT_TRUE(out.matter_heating);
  EXPECT_FALSE(out.keep_alive_pulsing);

  out = controller.step(InputAt(81 * kMin, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.keep_alive_pulsing);
}

// 7. Sensor faults: temperature/off modes fail safe immediately; repeat keeps
//    its cycle but warns.
TEST_CASE(sensor_fault_stops_heat_immediately_in_temperature_mode) {
  ZoneConfig config = TempModeConfig();
  config.min_on_sec = 300;
  ZoneController controller(config);

  ZoneOutput out = controller.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.sensor_fault_warning);

  // Safety beats the 5 minute minimum on-time.
  out = controller.step(InputAt(60 * kSec, 2000, false));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_FALSE(out.matter_heating);
  EXPECT_TRUE(out.sensor_fault_warning);
  EXPECT_TRUE(ReasonIs(HeatReason::kSensorFaultOff, out));

  // Recovery: a valid cold reading heats again.
  out = controller.step(InputAt(120 * kSec, 2000, true));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_FALSE(out.sensor_fault_warning);
}

TEST_CASE(invalid_temperature_value_is_treated_as_sensor_fault) {
  ZoneController controller(TempModeConfig());

  ZoneOutput out =
      controller.step(InputAt(0, thermostat::core::kInvalidTempCenti, true));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_TRUE(out.sensor_fault_warning);
  EXPECT_TRUE(ReasonIs(HeatReason::kSensorFaultOff, out));
}

TEST_CASE(sensor_fault_in_off_mode_reports_fault_without_freezing_heat) {
  ZoneConfig config;
  config.mode = Mode::kOff;
  config.freeze_protect_centi = 1500;
  ZoneController controller(config);

  ZoneOutput out = controller.step(InputAt(0, 900, false));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_TRUE(out.sensor_fault_warning);
  EXPECT_FALSE(out.freeze_protect_active);
  EXPECT_TRUE(ReasonIs(HeatReason::kSensorFaultOff, out));
}

TEST_CASE(sensor_fault_keeps_repeat_cycle_running_with_warning) {
  ZoneController always_heating(RepeatConfig(60));

  ZoneOutput out = always_heating.step(InputAt(0, 2000));
  EXPECT_TRUE(out.logical_heating);

  out = always_heating.step(
      InputAt(10 * kSec, thermostat::core::kInvalidTempCenti, false));
  EXPECT_TRUE(out.logical_heating);
  EXPECT_TRUE(out.matter_heating);
  EXPECT_TRUE(out.sensor_fault_warning);
  EXPECT_FALSE(out.freeze_protect_active);
  EXPECT_TRUE(ReasonIs(HeatReason::kRepeatCycleOn, out));

  // In the off window the cycle stays off while the warning is still raised.
  ZoneController cycling(RepeatConfig(15));
  EXPECT_TRUE(cycling.step(InputAt(0, 2000)).logical_heating);
  out = cycling.step(InputAt(20 * kMin, 2000, false));
  EXPECT_FALSE(out.logical_heating);
  EXPECT_TRUE(out.sensor_fault_warning);
}

// 8. Custom strategy injection and independent multi-zone stepping.
TEST_CASE(custom_strategy_can_be_injected_and_removed) {
  ZoneController controller(TempModeConfig());
  AlwaysHeatStrategy always_heat;

  ZoneOutput out = controller.step(InputAt(0, 3000));
  EXPECT_FALSE(out.logical_heating);  // hysteresis: 30.0 degC is far above 21.5

  controller.set_strategy(&always_heat);
  out = controller.step(InputAt(kMin, 3000));
  EXPECT_TRUE(out.logical_heating);

  controller.set_strategy(nullptr);
  out = controller.step(InputAt(2 * kMin, 3000));
  EXPECT_FALSE(out.logical_heating);
}

TEST_CASE(multi_zone_controller_steps_zones_independently) {
  thermostat::core::MultiZoneController multi;
  EXPECT_EQ(static_cast<size_t>(1), multi.active_zones());
  EXPECT_TRUE(multi.set_active_zones(2));
  EXPECT_EQ(static_cast<size_t>(2), multi.active_zones());
  EXPECT_FALSE(multi.set_active_zones(thermostat::core::kMaxZones + 1));
  EXPECT_EQ(static_cast<size_t>(2), multi.active_zones());

  ZoneConfig cool_zone = TempModeConfig();  // target 21.0 degC
  ZoneConfig warm_zone = TempModeConfig();
  warm_zone.target_temp_centi = 2200;  // target 22.0 degC
  multi.zone(0).set_config(cool_zone);
  multi.zone(1).set_config(warm_zone);

  const ZoneInput inputs[2] = {InputAt(0, 2300), InputAt(0, 2000)};
  ZoneOutput outputs[2];
  multi.step_all(inputs, outputs, 2);

  EXPECT_FALSE(outputs[0].logical_heating);  // 23.0 >= 21.5
  EXPECT_TRUE(outputs[1].logical_heating);   // 20.0 <= 21.5
  EXPECT_FALSE(multi.output(0).logical_heating);
  EXPECT_TRUE(multi.output(1).logical_heating);

  const ZoneInput next[2] = {InputAt(kMin, 2000), InputAt(kMin, 2300)};
  multi.step_all(next, outputs, 2);
  EXPECT_TRUE(outputs[0].logical_heating);
  EXPECT_FALSE(outputs[1].logical_heating);

  // Null buffers are a safe no-op, not a crash.
  multi.step_all(nullptr, outputs, 2);
  multi.step_all(next, nullptr, 2);
}

// Config values outside the supported ranges are clamped on set.
TEST_CASE(config_values_are_clamped_to_safe_ranges) {
  ZoneConfig config;
  config.target_temp_centi = 100;
  config.repeat_on_minutes = 200;
  config.freeze_protect_centi = 4800;
  config.hysteresis_centi = 0;
  ZoneController controller(config);

  EXPECT_EQ(500, controller.config().target_temp_centi);
  EXPECT_EQ(60, static_cast<int>(controller.config().repeat_on_minutes));
  EXPECT_EQ(1500, controller.config().freeze_protect_centi);
  EXPECT_EQ(10, controller.config().hysteresis_centi);

  ZoneConfig upper;
  upper.target_temp_centi = 9999;
  upper.freeze_protect_centi = 100;
  controller.set_config(upper);
  EXPECT_EQ(3000, controller.config().target_temp_centi);
  EXPECT_EQ(500, controller.config().freeze_protect_centi);
}

THERMOSTAT_TEST_MAIN()
