#pragma once

// Firmware entry points implemented in thermostat_firmware.cpp. The sketch
// only forwards setup()/loop() so arduino-cli does not preprocess the heavy
// Matter/LVGL/display headers on every build (SCRUM-330). Keep this header
// free of includes for the same reason.
namespace thermostat::firmware {

void setup();
void loop();

}  // namespace thermostat::firmware
