// LINK: src/sensor/sht40_driver.cpp src/net/clock_rtc.cpp
#include <cstdint>
#include <cstring>

#include "net/clock_rtc.h"
#include "sensor/sht40_driver.h"
#include "test_harness.h"

namespace {

// SHT40 high-precision frame: raw_t = 0x6666 (25.00 degC) and
// raw_rh = 0x8000 (56.50 %RH), with both Sensirion CRC-8 bytes.
constexpr uint8_t kValidFrame[6] = {0x66, 0x66, 0x93, 0x80, 0x00, 0xA2};

void make_frame(uint16_t raw_temp, uint16_t raw_rh, uint8_t out[6]) {
  out[0] = static_cast<uint8_t>(raw_temp >> 8);
  out[1] = static_cast<uint8_t>(raw_temp & 0xFF);
  out[2] = thermostat::sensor::sht40_crc8(out, 2);
  out[3] = static_cast<uint8_t>(raw_rh >> 8);
  out[4] = static_cast<uint8_t>(raw_rh & 0xFF);
  out[5] = thermostat::sensor::sht40_crc8(out + 3, 2);
}

}  // namespace

TEST_CASE(sht40_crc8_matches_sensirion_test_vector) {
  const uint8_t data[2] = {0xBE, 0xEF};
  EXPECT_EQ(0x92,
            static_cast<int>(thermostat::sensor::sht40_crc8(data, 2)));
}

TEST_CASE(sht40_decode_frame_returns_temperature_and_humidity) {
  int16_t temp_centi = 0;
  uint16_t rh_centi = 0;
  EXPECT_TRUE(thermostat::sensor::sht40_decode_frame(kValidFrame, &temp_centi,
                                                     &rh_centi));
  EXPECT_EQ(2500, static_cast<int>(temp_centi));
  EXPECT_EQ(5650, static_cast<int>(rh_centi));
}

TEST_CASE(sht40_decode_frame_maps_the_raw_range_ends) {
  int16_t temp_centi = 0;
  uint16_t rh_centi = 0;
  uint8_t frame[6];

  make_frame(0x0000, 0x0000, frame);
  EXPECT_TRUE(
      thermostat::sensor::sht40_decode_frame(frame, &temp_centi, &rh_centi));
  EXPECT_EQ(-4500, static_cast<int>(temp_centi));
  EXPECT_EQ(0, static_cast<int>(rh_centi));  // -6 %RH clamped to 0.

  make_frame(0xFFFF, 0xFFFF, frame);
  EXPECT_TRUE(
      thermostat::sensor::sht40_decode_frame(frame, &temp_centi, &rh_centi));
  EXPECT_EQ(13000, static_cast<int>(temp_centi));
  EXPECT_EQ(10000, static_cast<int>(rh_centi));  // 119 %RH clamped to 100.
}

TEST_CASE(sht40_decode_frame_rejects_bad_crc) {
  int16_t temp_centi = 0;
  uint16_t rh_centi = 0;

  uint8_t bad_temp_crc[6];
  std::memcpy(bad_temp_crc, kValidFrame, sizeof(bad_temp_crc));
  bad_temp_crc[1] ^= 0x01;
  EXPECT_FALSE(thermostat::sensor::sht40_decode_frame(
      bad_temp_crc, &temp_centi, &rh_centi));

  uint8_t bad_rh_crc[6];
  std::memcpy(bad_rh_crc, kValidFrame, sizeof(bad_rh_crc));
  bad_rh_crc[4] ^= 0x01;
  EXPECT_FALSE(
      thermostat::sensor::sht40_decode_frame(bad_rh_crc, &temp_centi, &rh_centi));
}

TEST_CASE(sht40_decode_frame_rejects_null_pointers) {
  int16_t temp_centi = 0;
  uint16_t rh_centi = 0;
  EXPECT_FALSE(thermostat::sensor::sht40_decode_frame(kValidFrame, nullptr,
                                                      &rh_centi));
  EXPECT_FALSE(thermostat::sensor::sht40_decode_frame(kValidFrame, &temp_centi,
                                                      nullptr));
  EXPECT_FALSE(thermostat::sensor::sht40_decode_frame(nullptr, &temp_centi,
                                                      &rh_centi));
}

TEST_CASE(bcd_to_dec_handles_boundaries) {
  EXPECT_EQ(0, static_cast<int>(thermostat::net::bcd_to_dec(0x00)));
  EXPECT_EQ(9, static_cast<int>(thermostat::net::bcd_to_dec(0x09)));
  EXPECT_EQ(10, static_cast<int>(thermostat::net::bcd_to_dec(0x10)));
  EXPECT_EQ(59, static_cast<int>(thermostat::net::bcd_to_dec(0x59)));
  EXPECT_EQ(99, static_cast<int>(thermostat::net::bcd_to_dec(0x99)));
}

TEST_CASE(dec_to_bcd_handles_boundaries) {
  EXPECT_EQ(0x00, static_cast<int>(thermostat::net::dec_to_bcd(0)));
  EXPECT_EQ(0x09, static_cast<int>(thermostat::net::dec_to_bcd(9)));
  EXPECT_EQ(0x10, static_cast<int>(thermostat::net::dec_to_bcd(10)));
  EXPECT_EQ(0x59, static_cast<int>(thermostat::net::dec_to_bcd(59)));
  EXPECT_EQ(0x99, static_cast<int>(thermostat::net::dec_to_bcd(99)));
}

TEST_CASE(bcd_round_trip_covers_all_two_digit_values) {
  for (int value = 0; value <= 99; ++value) {
    const uint8_t bcd =
        thermostat::net::dec_to_bcd(static_cast<uint8_t>(value));
    EXPECT_EQ(value, static_cast<int>(thermostat::net::bcd_to_dec(bcd)));
  }
}

TEST_CASE(utc_datetime_to_epoch_matches_reference_instants) {
  thermostat::net::UtcDateTime dt;
  int64_t epoch_sec = 0;

  dt.year = 2000;
  dt.month = 1;
  dt.day = 1;
  dt.hour = 0;
  dt.minute = 0;
  dt.second = 0;
  EXPECT_TRUE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));
  EXPECT_EQ(946684800LL, epoch_sec);

  dt.year = 2026;
  dt.month = 11;
  dt.day = 24;
  dt.hour = 12;
  dt.minute = 15;
  dt.second = 0;
  EXPECT_TRUE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));
  EXPECT_EQ(1795522500LL, epoch_sec);
}

TEST_CASE(epoch_to_utc_datetime_round_trips_and_derives_weekday) {
  thermostat::net::UtcDateTime dt;
  EXPECT_TRUE(thermostat::net::epoch_to_utc_datetime(0, &dt));
  EXPECT_EQ(1970, dt.year);
  EXPECT_EQ(1, dt.month);
  EXPECT_EQ(1, dt.day);
  EXPECT_EQ(4, dt.weekday);  // Thursday, 0 = Sunday.

  EXPECT_TRUE(thermostat::net::epoch_to_utc_datetime(946684800LL, &dt));
  EXPECT_EQ(2000, dt.year);
  EXPECT_EQ(1, dt.month);
  EXPECT_EQ(1, dt.day);
  EXPECT_EQ(6, dt.weekday);  // Saturday.

  EXPECT_TRUE(thermostat::net::epoch_to_utc_datetime(1795522500LL, &dt));
  EXPECT_EQ(2026, dt.year);
  EXPECT_EQ(11, dt.month);
  EXPECT_EQ(24, dt.day);
  EXPECT_EQ(12, dt.hour);
  EXPECT_EQ(15, dt.minute);
  EXPECT_EQ(0, dt.second);
  EXPECT_EQ(2, dt.weekday);  // Tuesday.

  int64_t epoch_sec = 0;
  EXPECT_TRUE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));
  EXPECT_EQ(1795522500LL, epoch_sec);
}

TEST_CASE(utc_datetime_to_epoch_accepts_leap_day) {
  thermostat::net::UtcDateTime dt;
  dt.year = 2028;
  dt.month = 2;
  dt.day = 29;
  dt.hour = 6;
  dt.minute = 30;
  dt.second = 15;
  int64_t epoch_sec = 0;
  EXPECT_TRUE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));
  EXPECT_EQ(1835418615LL, epoch_sec);
}

TEST_CASE(utc_datetime_to_epoch_rejects_invalid_fields) {
  thermostat::net::UtcDateTime dt;
  int64_t epoch_sec = 0;

  dt.year = 1969;
  EXPECT_FALSE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));

  dt.year = 2026;
  dt.month = 13;
  EXPECT_FALSE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));

  dt.month = 2;
  dt.day = 30;
  EXPECT_FALSE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));

  dt.month = 11;
  dt.day = 24;
  dt.hour = 24;
  EXPECT_FALSE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));

  dt.hour = 12;
  dt.minute = 60;
  EXPECT_FALSE(thermostat::net::utc_datetime_to_epoch(dt, &epoch_sec));

  EXPECT_FALSE(thermostat::net::utc_datetime_to_epoch(dt, nullptr));
  EXPECT_FALSE(thermostat::net::epoch_to_utc_datetime(0, nullptr));
}

THERMOSTAT_TEST_MAIN()
