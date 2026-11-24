#pragma once

#include <cstdint>

namespace thermostat::pins {

// Waveshare ESP32-S3-Touch-LCD-4.3B onboard I2C bus and control pins.
constexpr int kI2cSda = 8;
constexpr int kI2cScl = 9;
constexpr int kTouchInt = 4;
constexpr uint32_t kSerialBaud = 115200;

}  // namespace thermostat::pins
