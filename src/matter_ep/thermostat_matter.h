#pragma once

// Matter Thermostat endpoint for the DIY SmartThings thermostat.
//
// One heating-only Thermostat endpoint per zone (CLAUDE.md 8절). The firmware
// decides whether heating is needed and publishes the decision through the
// standard ThermostatRunningState attribute (bit 0, Heat). A SmartThings
// routine watches that attribute and switches the certified smart plug.
//
// MatterThermostat does not expose a ThermostatRunningState setter, so this
// wrapper creates/updates the attribute through the esp_matter cluster API.
// An auxiliary MatterContactSensor endpoint (contact = heating ON) provides a
// fallback condition in case the hub does not surface ThermostatRunningState.

#include <Arduino.h>
#include <Matter.h>

#include <cstdint>
#include <functional>

namespace thermostat::matter {

// Heating setpoint limits in centi-degC. These match the MatterThermostat
// defaults (kDefaultMinHeatSetpointLimit / kDefaultMaxHeatSetpointLimit).
constexpr int16_t kMinHeatSetpointCenti = 700;   // 7.00 C
constexpr int16_t kMaxHeatSetpointCenti = 3000;  // 30.00 C
constexpr int16_t kInvalidTempCenti = INT16_MIN;

struct MatterThermostatCallbacks {
  // Called when a Matter controller changes the thermostat mode.
  // true = Heat, false = Off. Return false to reject the change.
  std::function<bool(bool heat_mode)> on_mode_change;
  // Called when a Matter controller changes OccupiedHeatingSetpoint.
  // Return false to reject the change.
  std::function<bool(int16_t setpoint_centi)> on_setpoint_change;
};

class ThermostatEndpoint {
 public:
  // Creates the heating-only Thermostat endpoint and, when
  // enable_aux_contact is true, the auxiliary contact sensor endpoint.
  // Call before Matter.begin().
  bool begin(const MatterThermostatCallbacks& callbacks, bool enable_aux_contact = true);

  // Call after Matter.begin(): applies the initial temperature, setpoint,
  // mode and the idle RunningState / open contact sensor. Setters that touch
  // the Matter stack become effective only after this call.
  void after_matter_begin(int16_t initial_temp_centi, int16_t initial_setpoint_centi,
                          bool heat_mode);

  // LocalTemperature in centi-degC. Returns false for kInvalidTempCenti.
  bool set_local_temperature_centi(int16_t temp_centi);

  // OccupiedHeatingSetpoint in centi-degC, clamped to 7.00..30.00 C.
  bool set_heating_setpoint_centi(int16_t setpoint_centi);

  // SystemMode: true = Heat, false = Off.
  bool set_heat_mode(bool heat_mode);

  // Publishes the firmware heating decision: ThermostatRunningState bit 0 and
  // the auxiliary contact sensor (contact = heating ON).
  bool set_heating_active(bool heating);

  bool is_heating_active() const { return heating_active_; }
  bool is_running_state_supported() const { return running_state_supported_; }
  bool is_aux_contact_enabled() const { return aux_contact_enabled_; }
  uint16_t endpoint_id() const;

 private:
  MatterThermostat thermostat_;
  MatterContactSensor aux_heat_sensor_;
  MatterThermostatCallbacks callbacks_;
  bool aux_contact_enabled_ = false;
  bool running_state_supported_ = false;
  bool matter_begun_ = false;
  bool heating_active_ = false;
};

}  // namespace thermostat::matter
