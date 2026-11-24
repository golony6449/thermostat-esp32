#pragma once

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO
#include <Wire.h>
#endif

namespace thermostat::net {

// PCF85063A registers used by the clock adapter (NXP datasheet, register map).
// The time block is 0x04 (seconds) .. 0x0A (years) in BCD, with 0x07 = days and
// 0x08 = weekday in between.
constexpr uint8_t kPcf85063aAddress = 0x51;
constexpr uint8_t kPcf85063aRegControl1 = 0x00;
constexpr uint8_t kPcf85063aRegSeconds = 0x04;
constexpr uint8_t kPcf85063aRegYears = 0x0A;
constexpr uint8_t kPcf85063aOscillatorStoppedBit = 0x80;
constexpr uint8_t kPcf85063aControl1StopBit = 0x20;
constexpr uint8_t kPcf85063aControl1TwelveHourBit = 0x02;

// UTC calendar fields used when reading/writing the PCF85063A registers.
// `weekday` follows the chip convention: 0 = Sunday ... 6 = Saturday and is
// ignored when converting back to epoch seconds (it is derived from the date).
struct UtcDateTime {
  int year = 2000;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int weekday = 6;  // 2000-01-01 was a Saturday.
};

// Plain BCD helpers for the PCF85063A time registers. `dec` must be 0..99.
uint8_t bcd_to_dec(uint8_t bcd);
uint8_t dec_to_bcd(uint8_t dec);

// Proleptic Gregorian <-> Unix UTC conversion (Howard Hinnant's civil calendar
// formulas). Implemented with integer math only so the result never depends on
// the host's local timezone or on mktime().
bool utc_datetime_to_epoch(const UtcDateTime& dt, int64_t* out_epoch_sec);
bool epoch_to_utc_datetime(int64_t epoch_sec, UtcDateTime* out);

#ifdef ARDUINO

// NTP synchronization with battery-backed PCF85063A RTC.
//
// The RTC keeps UTC across reboots, so the firmware can restore wall-clock time
// before Wi-Fi is up. All methods degrade gracefully when the chip is missing.
class ClockRtc {
 public:
  bool begin(TwoWire& wire, uint8_t addr = kPcf85063aAddress);
  bool read_rtc_epoch(int64_t* out_epoch_sec);
  bool write_rtc_epoch(int64_t epoch_sec);
  bool sync_ntp_and_save(const char* ntp_server = "pool.ntp.org",
                         uint32_t timeout_ms = 5000);
  bool restore_system_time_from_rtc();
  int64_t now_epoch_sec() const;

  bool is_present() const { return present_; }
  uint8_t address() const { return addr_; }

 private:
  bool read_registers(uint8_t reg, uint8_t* data, size_t len);
  bool write_registers(uint8_t reg, const uint8_t* data, size_t len);

  TwoWire* wire_ = nullptr;
  uint8_t addr_ = kPcf85063aAddress;
  bool present_ = false;
};

#endif  // ARDUINO

}  // namespace thermostat::net
