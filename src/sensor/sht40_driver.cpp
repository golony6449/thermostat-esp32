#include "sht40_driver.h"

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#include <driver/i2c.h>
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
namespace {

// Bounded transfer timeout: a missing or wedged sensor must not hang boot.
TickType_t i2c_timeout_ticks() { return pdMS_TO_TICKS(50); }

// Probes a 7-bit address with a start + address + stop transaction. The
// hardware reports a NACK (absent device) as an error, so this never blocks
// longer than the timeout above.
bool i2c_address_acks(i2c_port_t port, uint8_t addr) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) {
    return false;
  }
  i2c_master_start(cmd);
  i2c_master_write_byte(
      cmd, static_cast<uint8_t>((addr << 1) | I2C_MASTER_WRITE), true);
  i2c_master_stop(cmd);
  const esp_err_t err = i2c_master_cmd_begin(port, cmd, i2c_timeout_ticks());
  i2c_cmd_link_delete(cmd);
  return err == ESP_OK;
}

}  // namespace

bool Sht40Driver::begin(i2c_port_t port, uint8_t addr) {
  port_ = port;
  addr_ = addr;
  present_ = i2c_address_acks(port_, addr_);
  return present_;
}

bool Sht40Driver::read(int16_t* out_temp_centi, uint16_t* out_rh_centi) {
  if (out_temp_centi == nullptr || out_rh_centi == nullptr) {
    return false;
  }
  if (!present_) {
    return false;
  }

  // High-precision measurement: command, conversion delay, 6-byte frame.
  const uint8_t command = kSht40CmdMeasureHighPrecision;
  esp_err_t err = i2c_master_write_to_device(port_, addr_, &command, 1,
                                             i2c_timeout_ticks());
  if (err != ESP_OK) {
    return false;
  }
  delay(kSht40MeasureDelayMs);

  uint8_t frame[kSht40FrameSize] = {};
  err = i2c_master_read_from_device(port_, addr_, frame, kSht40FrameSize,
                                    i2c_timeout_ticks());
  if (err != ESP_OK) {
    return false;
  }
  return sht40_decode_frame(frame, out_temp_centi, out_rh_centi);
}

#endif  // ARDUINO

}  // namespace thermostat::sensor
