#include "sht40_driver.h"

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace thermostat::sensor {

uint8_t sht40_crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  if (data == nullptr) {
    return crc;
  }
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if ((crc & 0x80) != 0) {
        crc = static_cast<uint8_t>((crc << 1) ^ 0x31);
      } else {
        crc = static_cast<uint8_t>(crc << 1);
      }
    }
  }
  return crc;
}

bool sht40_decode_frame(const uint8_t frame[kSht40FrameSize],
                        int16_t* out_temp_centi, uint16_t* out_rh_centi) {
  if (frame == nullptr || out_temp_centi == nullptr || out_rh_centi == nullptr) {
    return false;
  }
  // The SHT40 sends two 16-bit words, each followed by its own CRC byte.
  if (sht40_crc8(frame, 2) != frame[2] ||
      sht40_crc8(frame + 3, 2) != frame[5]) {
    return false;
  }

  const uint16_t raw_temp =
      static_cast<uint16_t>((static_cast<uint16_t>(frame[0]) << 8) | frame[1]);
  const uint16_t raw_rh =
      static_cast<uint16_t>((static_cast<uint16_t>(frame[3]) << 8) | frame[4]);

  // T [degC] = -45 + 175 * raw_temp / 65535, scaled to 0.01 degC.
  // +32767/65535 implements rounding to the nearest centi-degree.
  const int32_t temp_centi =
      -4500 + static_cast<int32_t>((17500LL * raw_temp + 32767) / 65535);

  // RH [%] = -6 + 125 * raw_rh / 65535, scaled to 0.01 %RH and clamped to the
  // physically meaningful 0..100 % range from the datasheet.
  int32_t rh_centi =
      -600 + static_cast<int32_t>((12500LL * raw_rh + 32767) / 65535);
  if (rh_centi < 0) {
    rh_centi = 0;
  } else if (rh_centi > 10000) {
    rh_centi = 10000;
  }

  *out_temp_centi = static_cast<int16_t>(temp_centi);
  *out_rh_centi = static_cast<uint16_t>(rh_centi);
  return true;
}

#ifdef ARDUINO

bool Sht40Driver::begin(TwoWire& wire, uint8_t addr) {
  wire_ = &wire;
  addr_ = addr;
  present_ = false;

  // Bound every transfer so a missing or wedged sensor cannot hang the boot
  // sequence (the ESP32 Wire default timeout is much longer).
  (void)wire_->setTimeOut(50);
  wire_->beginTransmission(addr_);
  present_ = (wire_->endTransmission() == 0);
  return present_;
}

bool Sht40Driver::read(int16_t* out_temp_centi, uint16_t* out_rh_centi) {
  if (out_temp_centi == nullptr || out_rh_centi == nullptr) {
    return false;
  }
  if (wire_ == nullptr || !present_) {
    return false;
  }

  wire_->beginTransmission(addr_);
  wire_->write(kSht40CmdMeasureHighPrecision);
  if (wire_->endTransmission() != 0) {
    return false;
  }
  delay(kSht40MeasureDelayMs);

  const size_t received = wire_->requestFrom(
      static_cast<uint16_t>(addr_), static_cast<uint8_t>(kSht40FrameSize), true);
  if (received != static_cast<size_t>(kSht40FrameSize)) {
    return false;
  }

  uint8_t frame[kSht40FrameSize];
  for (size_t i = 0; i < static_cast<size_t>(kSht40FrameSize); ++i) {
    if (wire_->available() <= 0) {
      return false;
    }
    frame[i] = static_cast<uint8_t>(wire_->read());
  }
  return sht40_decode_frame(frame, out_temp_centi, out_rh_centi);
}

#endif  // ARDUINO

}  // namespace thermostat::sensor
