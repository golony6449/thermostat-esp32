#pragma once

#include <cstdint>

// Sensor module entry header: SHT40 hardware constants for the Waveshare
// ESP32-S3-Touch-LCD-4.3B external sensor wiring.
//
// This header stays free of Arduino/Wire dependencies so host-side unit tests
// can include it directly. The Arduino-only driver class lives in
// `sht40_driver.h` behind `#ifdef ARDUINO`.

namespace thermostat::sensor {

// I2C 7-bit address of the Sensirion SHT40 (datasheet 4.4).
constexpr uint8_t kSht40Address = 0x44;

// High-precision measurement command (datasheet 4.5): 10 ms conversion time,
// followed by 6 response bytes (T_msb, T_lsb, T_crc, RH_msb, RH_lsb, RH_crc).
constexpr uint8_t kSht40CmdMeasureHighPrecision = 0xFD;
constexpr uint16_t kSht40MeasureDelayMs = 10;
constexpr uint8_t kSht40FrameSize = 6;

// I2C bus clock used by the project (the SHT40 supports fast mode).
constexpr uint32_t kSensorI2cClockHz = 400000;

}  // namespace thermostat::sensor
