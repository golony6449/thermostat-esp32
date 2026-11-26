#include "clock_rtc.h"

#include <cstddef>
#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#include <driver/i2c.h>
#include <sys/time.h>

#include <ctime>
#endif

namespace thermostat::net {
namespace {

constexpr int64_t kSecondsPerDay = 86400;

// Howard Hinnant's days_from_civil: days since 1970-01-01 for a proleptic
// Gregorian date. Works for any year >= 1.
int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned shifted_month =
      static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
  const unsigned day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

// Inverse of days_from_civil (Howard Hinnant's civil_from_days).
void civil_from_days(int64_t days, int* out_year, unsigned* out_month,
                     unsigned* out_day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
       day_of_era / 146096) /
      365;
  const int year =
      static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned month_prime = (5 * day_of_year + 2) / 153;
  *out_day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const int month_offset = month_prime < 10 ? 3 : -9;
  *out_month =
      static_cast<unsigned>(static_cast<int>(month_prime) + month_offset);
  *out_year = year + (*out_month <= 2 ? 1 : 0);
}

bool is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int year, int month) {
  static constexpr int kDaysPerMonth[12] = {31, 28, 31, 30, 31, 30,
                                            31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kDaysPerMonth[month - 1];
}

}  // namespace

uint8_t bcd_to_dec(uint8_t bcd) {
  return static_cast<uint8_t>(((bcd >> 4) * 10U) + (bcd & 0x0F));
}

uint8_t dec_to_bcd(uint8_t dec) {
  return static_cast<uint8_t>(((dec / 10U) << 4) | (dec % 10U));
}

bool utc_datetime_to_epoch(const UtcDateTime& dt, int64_t* out_epoch_sec) {
  if (out_epoch_sec == nullptr) {
    return false;
  }
  // The PCF85063A stores a two-digit year and the firmware only deals with
  // current dates, so keep the supported range explicit.
  if (dt.year < 1970 || dt.year > 2099) {
    return false;
  }
  if (dt.month < 1 || dt.month > 12) {
    return false;
  }
  if (dt.day < 1 || dt.day > days_in_month(dt.year, dt.month)) {
    return false;
  }
  if (dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 ||
      dt.second < 0 || dt.second > 59) {
    return false;
  }

  const int64_t days = days_from_civil(dt.year, static_cast<unsigned>(dt.month),
                                       static_cast<unsigned>(dt.day));
  *out_epoch_sec = days * kSecondsPerDay + dt.hour * 3600 + dt.minute * 60 +
                   dt.second;
  return true;
}

bool epoch_to_utc_datetime(int64_t epoch_sec, UtcDateTime* out) {
  if (out == nullptr) {
    return false;
  }
  int64_t days = epoch_sec / kSecondsPerDay;
  int64_t remainder = epoch_sec % kSecondsPerDay;
  if (remainder < 0) {
    remainder += kSecondsPerDay;
    --days;
  }

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, &year, &month, &day);
  if (year < 1970 || year > 2099) {
    return false;
  }

  out->year = year;
  out->month = static_cast<int>(month);
  out->day = static_cast<int>(day);
  out->hour = static_cast<int>(remainder / 3600);
  out->minute = static_cast<int>((remainder % 3600) / 60);
  out->second = static_cast<int>(remainder % 60);
  // 1970-01-01 was a Thursday, so Sunday-based weekday = (days + 4) % 7.
  int weekday = static_cast<int>((days + 4) % 7);
  if (weekday < 0) {
    weekday += 7;
  }
  out->weekday = weekday;
  return true;
}

#ifdef ARDUINO
namespace {

// The SHT40-style digits check: reject nibbles above 9 before trusting a BCD
// register, so a bus glitch cannot silently produce a bogus time.
bool bcd_digits_valid(uint8_t value) {
  return (value & 0x0F) <= 9 && ((value >> 4) & 0x0F) <= 9;
}

// Anything below 2023-11-14 means SNTP has not delivered a real time yet.
constexpr int64_t kSaneEpochSec = 1700000000;

// Bounded transfer timeout: a missing RTC must not hang the boot sequence.
TickType_t i2c_timeout_ticks() { return pdMS_TO_TICKS(50); }

// One register-address byte plus the payload. Two transactions used by the
// firmware are 1 byte (Control_1) and 7 bytes (time block), so 16 is ample.
constexpr size_t kMaxRegisterWriteLen = 16;

// Probes a 7-bit address with a start + address + stop transaction.
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

bool ClockRtc::begin(i2c_port_t port, uint8_t addr) {
  port_ = port;
  addr_ = addr;
  present_ = false;

  if (!i2c_address_acks(port_, addr_)) {
    return false;
  }

  // Force 24-hour mode and clear the STOP bit so the oscillator runs.
  uint8_t control1 = 0;
  if (!read_registers(kPcf85063aRegControl1, &control1, 1)) {
    return false;
  }
  const uint8_t desired = static_cast<uint8_t>(
      control1 &
      ~(kPcf85063aControl1StopBit | kPcf85063aControl1TwelveHourBit));
  if (desired != control1 &&
      !write_registers(kPcf85063aRegControl1, &desired, 1)) {
    return false;
  }

  present_ = true;
  return true;
}

bool ClockRtc::read_registers(uint8_t reg, uint8_t* data, size_t len) {
  if (data == nullptr || len == 0 || len > 0xFF) {
    return false;
  }
  const esp_err_t err = i2c_master_write_read_device(
      port_, addr_, &reg, 1, data, len, i2c_timeout_ticks());
  return err == ESP_OK;
}

bool ClockRtc::write_registers(uint8_t reg, const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0 || len > kMaxRegisterWriteLen) {
    return false;
  }
  uint8_t buffer[kMaxRegisterWriteLen + 1];
  buffer[0] = reg;
  for (size_t i = 0; i < len; ++i) {
    buffer[i + 1] = data[i];
  }
  const esp_err_t err = i2c_master_write_to_device(
      port_, addr_, buffer, len + 1, i2c_timeout_ticks());
  return err == ESP_OK;
}

bool ClockRtc::read_rtc_epoch(int64_t* out_epoch_sec) {
  if (out_epoch_sec == nullptr || !present_) {
    return false;
  }

  // 0x04 seconds, 0x05 minutes, 0x06 hours, 0x07 days, 0x08 weekday,
  // 0x09 months, 0x0A years.
  uint8_t raw[7] = {0};
  if (!read_registers(kPcf85063aRegSeconds, raw, sizeof(raw))) {
    return false;
  }
  // The OS (oscillator stopped) flag means the stored time is not trustworthy
  // (for example the backup battery was removed).
  if ((raw[0] & kPcf85063aOscillatorStoppedBit) != 0) {
    return false;
  }

  const uint8_t seconds_bcd = static_cast<uint8_t>(raw[0] & 0x7F);
  const uint8_t minutes_bcd = static_cast<uint8_t>(raw[1] & 0x7F);
  const uint8_t hours_bcd = static_cast<uint8_t>(raw[2] & 0x3F);
  const uint8_t days_bcd = static_cast<uint8_t>(raw[3] & 0x3F);
  const uint8_t months_bcd = static_cast<uint8_t>(raw[5] & 0x1F);
  const uint8_t years_bcd = raw[6];
  if (!bcd_digits_valid(seconds_bcd) || !bcd_digits_valid(minutes_bcd) ||
      !bcd_digits_valid(hours_bcd) || !bcd_digits_valid(days_bcd) ||
      !bcd_digits_valid(months_bcd) || !bcd_digits_valid(years_bcd)) {
    return false;
  }

  UtcDateTime dt;
  dt.year = 2000 + bcd_to_dec(years_bcd);
  dt.month = bcd_to_dec(months_bcd);
  dt.day = bcd_to_dec(days_bcd);
  dt.hour = bcd_to_dec(hours_bcd);
  dt.minute = bcd_to_dec(minutes_bcd);
  dt.second = bcd_to_dec(seconds_bcd);
  return utc_datetime_to_epoch(dt, out_epoch_sec);
}

bool ClockRtc::write_rtc_epoch(int64_t epoch_sec) {
  if (!present_) {
    return false;
  }
  UtcDateTime dt;
  if (!epoch_to_utc_datetime(epoch_sec, &dt)) {
    return false;
  }

  // Writing seconds with bit 7 clear also resets the OS flag.
  uint8_t regs[7];
  regs[0] = dec_to_bcd(static_cast<uint8_t>(dt.second));
  regs[1] = dec_to_bcd(static_cast<uint8_t>(dt.minute));
  regs[2] = dec_to_bcd(static_cast<uint8_t>(dt.hour));
  regs[3] = dec_to_bcd(static_cast<uint8_t>(dt.day));
  regs[4] = static_cast<uint8_t>(dt.weekday % 7);
  regs[5] = dec_to_bcd(static_cast<uint8_t>(dt.month));
  regs[6] = dec_to_bcd(static_cast<uint8_t>(dt.year % 100));
  return write_registers(kPcf85063aRegSeconds, regs, sizeof(regs));
}

bool ClockRtc::sync_ntp_and_save(const char* ntp_server, uint32_t timeout_ms) {
  if (ntp_server == nullptr) {
    return false;
  }
  // Korean standard time (UTC+9) for log formatting; the epoch itself stays
  // UTC and the RTC is always written in UTC.
  configTime(9L * 3600L, 0, ntp_server);

  const uint32_t started_ms = millis();
  do {
    const int64_t epoch = static_cast<int64_t>(time(nullptr));
    if (epoch >= kSaneEpochSec) {
      return write_rtc_epoch(epoch);
    }
    delay(250);
  } while (millis() - started_ms < timeout_ms);
  return false;
}

bool ClockRtc::set_system_epoch(int64_t epoch_sec) {
  struct timeval tv = {};
  tv.tv_sec = static_cast<time_t>(epoch_sec);
  tv.tv_usec = 0;
  return settimeofday(&tv, nullptr) == 0;
}

bool ClockRtc::restore_system_time_from_rtc() {
  int64_t epoch_sec = 0;
  if (!read_rtc_epoch(&epoch_sec)) {
    return false;
  }
  return set_system_epoch(epoch_sec);
}

int64_t ClockRtc::now_epoch_sec() const {
  return static_cast<int64_t>(time(nullptr));
}

#endif  // ARDUINO

}  // namespace thermostat::net
