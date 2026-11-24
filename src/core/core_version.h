#pragma once

namespace thermostat::core {

// Firmware core version, reported in boot logs and the UI.
const char* version_string();

// Default minimum indoor temperature protection: 10.0 degC.
// Temperatures are expressed in centi-degC (0.01 degC), matching Matter.
constexpr int kDefaultMinTempCenti = 1000;

// Clamps a heating setpoint to [min_centi, max_centi].
int clamp_setpoint_centi(int setpoint_centi, int min_centi = 500,
                         int max_centi = 3000);

}  // namespace thermostat::core
