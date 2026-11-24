#pragma once

#include <cstddef>
#include <cstdint>

#include "thermostat_sensor_hw.h"

#ifdef ARDUINO
#include <Wire.h>
#endif

namespace thermostat::sensor {

// Sensirion CRC-8: polynomial 0x31 (x^8 + x^5 + x^4 + 1), initial value 0xFF,
// no reflection and no final XOR. Used for both the temperature and the
// relative-humidity word of an SHT40 measurement frame.
uint8_t sht40_crc8(const uint8_t* data, size_t len);

// Validate both CRC words of a 6-byte SHT40 measurement frame and convert the
// raw words to centi-degC and centi-%RH (0.01 units, matching the project-wide
// temperature unit). Returns false when a CRC does not match.
bool sht40_decode_frame(const uint8_t frame[kSht40FrameSize],
                        int16_t* out_temp_centi, uint16_t* out_rh_centi);

#ifdef ARDUINO

// Minimal SHT40 driver on top of Wire (no external library).
//
// A missing sensor must never crash or block the firmware: `begin()` returns
// false when the address is not acknowledged and `read()` returns false on
// NACK, short reads or CRC errors.
class Sht40Driver {
 public:
  bool begin(TwoWire& wire, uint8_t addr = kSht40Address);
  bool read(int16_t* out_temp_centi, uint16_t* out_rh_centi);

  bool is_present() const { return present_; }
  uint8_t address() const { return addr_; }

 private:
  TwoWire* wire_ = nullptr;
  uint8_t addr_ = kSht40Address;
  bool present_ = false;
};

#endif  // ARDUINO

}  // namespace thermostat::sensor
