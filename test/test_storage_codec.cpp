// LINK: src/storage/settings_codec.cpp
#include "storage/settings_codec.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "test_harness.h"

namespace {

using thermostat::core::Mode;
using thermostat::core::StrategyKind;
using thermostat::storage::DecodeStatus;
using thermostat::storage::DeferredWriteGuard;
using thermostat::storage::DeviceSettings;
using thermostat::storage::SensorSourceSetting;
using thermostat::storage::deserialize_settings;
using thermostat::storage::serialize_settings;

constexpr size_t kHeaderSize = 12;  // magic(4) + version(2) + length(2) + crc(4)

bool same_string(const char* lhs, const char* rhs) {
  return std::strcmp(lhs, rhs) == 0;
}

void set_string(char* dest, size_t capacity, const char* text) {
  std::memset(dest, 0, capacity);
  const size_t length = std::strlen(text);
  const size_t copy_len = length < capacity - 1 ? length : capacity - 1;
  std::memcpy(dest, text, copy_len);
}

bool same_settings(const DeviceSettings& lhs, const DeviceSettings& rhs) {
  if (lhs.active_zones != rhs.active_zones) {
    return false;
  }
  if (!same_string(lhs.homeserver_url, rhs.homeserver_url)) {
    return false;
  }

  for (size_t i = 0; i < thermostat::core::kMaxZones; ++i) {
    const auto& a = lhs.zones[i];
    const auto& b = rhs.zones[i];
    if (a.control.mode != b.control.mode ||
        a.control.target_temp_centi != b.control.target_temp_centi ||
        a.control.repeat_on_minutes != b.control.repeat_on_minutes ||
        a.control.freeze_protect_centi != b.control.freeze_protect_centi ||
        a.control.hysteresis_centi != b.control.hysteresis_centi ||
        a.control.min_on_sec != b.control.min_on_sec ||
        a.control.min_off_sec != b.control.min_off_sec ||
        a.control.keep_alive_interval_sec != b.control.keep_alive_interval_sec ||
        a.control.keep_alive_off_sec != b.control.keep_alive_off_sec ||
        a.control.strategy != b.control.strategy) {
      return false;
    }
    if (a.sensor_source != b.sensor_source ||
        a.selected_remote_count != b.selected_remote_count ||
        a.sensor_offset_centi != b.sensor_offset_centi) {
      return false;
    }
    for (size_t s = 0; s < thermostat::storage::kMaxSelectedRemoteSensors;
         ++s) {
      if (!same_string(a.selected_remote_ids[s], b.selected_remote_ids[s])) {
        return false;
      }
    }
  }
  return true;
}

DeviceSettings make_custom_settings() {
  DeviceSettings settings = thermostat::storage::default_device_settings();
  settings.active_zones = 3;

  auto& zone0 = settings.zones[0];
  zone0.control.mode = Mode::kTemperature;
  zone0.control.target_temp_centi = 2350;
  zone0.control.repeat_on_minutes = 42;
  zone0.control.freeze_protect_centi = 1200;
  zone0.control.hysteresis_centi = 75;
  zone0.control.min_on_sec = 120;
  zone0.control.min_off_sec = 240;
  zone0.control.keep_alive_interval_sec = 2400;
  zone0.control.keep_alive_off_sec = 5;
  zone0.control.strategy = StrategyKind::kHysteresis;
  zone0.sensor_source = SensorSourceSetting::kAverageRemote;
  zone0.selected_remote_count = 2;
  set_string(zone0.selected_remote_ids[0], thermostat::storage::kMaxSensorIdLen,
             "livingroom");
  set_string(zone0.selected_remote_ids[1], thermostat::storage::kMaxSensorIdLen,
             "bedroom");
  zone0.sensor_offset_centi = -125;

  auto& zone1 = settings.zones[1];
  zone1.control.mode = Mode::kRepeat;
  zone1.control.target_temp_centi = 1800;
  zone1.control.repeat_on_minutes = 60;
  zone1.control.freeze_protect_centi = 700;
  zone1.control.hysteresis_centi = 30;
  zone1.control.min_on_sec = 0;
  zone1.control.min_off_sec = 600;
  zone1.control.keep_alive_interval_sec = 60;
  zone1.control.keep_alive_off_sec = 1;
  zone1.sensor_source = SensorSourceSetting::kSingleRemote;
  zone1.selected_remote_count = 1;
  set_string(zone1.selected_remote_ids[0], thermostat::storage::kMaxSensorIdLen,
             "kitchen");
  zone1.sensor_offset_centi = 300;

  auto& zone2 = settings.zones[2];
  zone2.control.mode = Mode::kOff;
  zone2.control.target_temp_centi = 500;
  zone2.control.freeze_protect_centi = 1500;
  zone2.sensor_source = SensorSourceSetting::kBoardOnly;
  zone2.sensor_offset_centi = -500;

  set_string(settings.homeserver_url, thermostat::storage::kMaxUrlLen,
             "http://192.168.0.10:8080");
  return settings;
}

DecodeStatus decode(const std::vector<uint8_t>& blob, DeviceSettings* out) {
  return deserialize_settings(blob.data(), blob.size(), out);
}

}  // namespace

// --- Blob format ----------------------------------------------------------

TEST_CASE(serialized_blob_uses_documented_little_endian_header) {
  const std::vector<uint8_t> blob =
      serialize_settings(thermostat::storage::default_device_settings());

  EXPECT_TRUE(blob.size() > kHeaderSize);
  // The magic constant is stored little-endian, so the on-flash byte order is
  // reversed relative to the "THST" mnemonic.
  const uint32_t magic = static_cast<uint32_t>(blob[0]) |
                         (static_cast<uint32_t>(blob[1]) << 8) |
                         (static_cast<uint32_t>(blob[2]) << 16) |
                         (static_cast<uint32_t>(blob[3]) << 24);
  EXPECT_EQ(thermostat::storage::kSettingsMagic, magic);

  const int version = static_cast<int>(blob[4]) |
                      (static_cast<int>(blob[5]) << 8);
  EXPECT_EQ(static_cast<int>(thermostat::storage::kSettingsSchemaVersion),
            version);

  const size_t payload_len = static_cast<size_t>(blob[6]) |
                             (static_cast<size_t>(blob[7]) << 8);
  // Header (8) + payload + CRC (4).
  EXPECT_EQ(blob.size() - kHeaderSize, payload_len);
}

TEST_CASE(crc32_matches_ieee_reference) {
  const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(0xCBF43926u, thermostat::storage::compute_crc32(digits,
                                                            sizeof(digits)));
  EXPECT_EQ(0u, thermostat::storage::compute_crc32(digits, 0));
}

TEST_CASE(serialization_is_deterministic) {
  const DeviceSettings settings = make_custom_settings();
  const std::vector<uint8_t> first = serialize_settings(settings);
  const std::vector<uint8_t> second = serialize_settings(settings);
  EXPECT_TRUE(first == second);
}

// --- Round trips ----------------------------------------------------------

TEST_CASE(default_settings_round_trip) {
  const DeviceSettings original =
      thermostat::storage::default_device_settings();
  const std::vector<uint8_t> blob = serialize_settings(original);

  // Pre-fill the destination to prove a successful decode overwrites it.
  DeviceSettings restored = make_custom_settings();
  EXPECT_TRUE(decode(blob, &restored) == DecodeStatus::kOk);
  EXPECT_TRUE(same_settings(original, restored));

  EXPECT_EQ(1, static_cast<int>(restored.active_zones));
  EXPECT_EQ(static_cast<int>(Mode::kOff),
            static_cast<int>(restored.zones[0].control.mode));
  EXPECT_EQ(2100, restored.zones[0].control.target_temp_centi);
  EXPECT_EQ(15, static_cast<int>(restored.zones[0].control.repeat_on_minutes));
  EXPECT_EQ(1000, restored.zones[0].control.freeze_protect_centi);
  EXPECT_EQ(50, restored.zones[0].control.hysteresis_centi);
  EXPECT_EQ(300, restored.zones[0].control.min_on_sec);
  EXPECT_EQ(300, restored.zones[0].control.min_off_sec);
  EXPECT_STREQ("", restored.homeserver_url);
}

TEST_CASE(custom_settings_round_trip) {
  const DeviceSettings original = make_custom_settings();
  const std::vector<uint8_t> blob = serialize_settings(original);

  DeviceSettings restored;
  EXPECT_TRUE(decode(blob, &restored) == DecodeStatus::kOk);
  EXPECT_TRUE(same_settings(original, restored));

  // Spot-check each documented field family across zones.
  EXPECT_EQ(3, static_cast<int>(restored.active_zones));
  EXPECT_EQ(static_cast<int>(Mode::kTemperature),
            static_cast<int>(restored.zones[0].control.mode));
  EXPECT_EQ(2350, restored.zones[0].control.target_temp_centi);
  EXPECT_EQ(42, static_cast<int>(restored.zones[0].control.repeat_on_minutes));
  EXPECT_EQ(1200, restored.zones[0].control.freeze_protect_centi);
  EXPECT_EQ(75, restored.zones[0].control.hysteresis_centi);
  EXPECT_EQ(120, restored.zones[0].control.min_on_sec);
  EXPECT_EQ(240, restored.zones[0].control.min_off_sec);
  EXPECT_EQ(2400, restored.zones[0].control.keep_alive_interval_sec);
  EXPECT_EQ(5, restored.zones[0].control.keep_alive_off_sec);
  EXPECT_EQ(static_cast<int>(StrategyKind::kHysteresis),
            static_cast<int>(restored.zones[0].control.strategy));
  EXPECT_EQ(static_cast<int>(SensorSourceSetting::kAverageRemote),
            static_cast<int>(restored.zones[0].sensor_source));
  EXPECT_EQ(2, static_cast<int>(restored.zones[0].selected_remote_count));
  EXPECT_STREQ("livingroom", restored.zones[0].selected_remote_ids[0]);
  EXPECT_STREQ("bedroom", restored.zones[0].selected_remote_ids[1]);
  EXPECT_EQ(-125, restored.zones[0].sensor_offset_centi);
  EXPECT_EQ(static_cast<int>(Mode::kRepeat),
            static_cast<int>(restored.zones[1].control.mode));
  EXPECT_STREQ("kitchen", restored.zones[1].selected_remote_ids[0]);
  EXPECT_EQ(300, restored.zones[1].sensor_offset_centi);
  EXPECT_STREQ("http://192.168.0.10:8080", restored.homeserver_url);
}

// --- Corrupt input --------------------------------------------------------

TEST_CASE(decode_failures_restore_safe_defaults) {
  const std::vector<uint8_t> valid = serialize_settings(make_custom_settings());
  const DeviceSettings defaults =
      thermostat::storage::default_device_settings();

  DeviceSettings out = make_custom_settings();
  EXPECT_TRUE(deserialize_settings(nullptr, 0, &out) ==
              DecodeStatus::kEmptyOrShort);
  EXPECT_TRUE(same_settings(defaults, out));

  const uint8_t short_bytes[] = {0x54, 0x48, 0x53, 0x54, 0x01, 0x00};
  out = make_custom_settings();
  EXPECT_TRUE(deserialize_settings(short_bytes, sizeof(short_bytes), &out) ==
              DecodeStatus::kEmptyOrShort);
  EXPECT_TRUE(same_settings(defaults, out));

  std::vector<uint8_t> bad_magic = valid;
  bad_magic[0] = static_cast<uint8_t>(bad_magic[0] ^ 0xFFu);
  out = make_custom_settings();
  EXPECT_TRUE(decode(bad_magic, &out) == DecodeStatus::kBadMagic);
  EXPECT_TRUE(same_settings(defaults, out));

  std::vector<uint8_t> future = valid;
  future[4] = 2;  // schema_version = 2
  future[5] = 0;
  out = make_custom_settings();
  EXPECT_TRUE(decode(future, &out) == DecodeStatus::kUnsupportedVersion);
  EXPECT_TRUE(same_settings(defaults, out));

  // The payload is declared as 725 bytes; a truncated buffer no longer
  // matches the declared length.
  std::vector<uint8_t> truncated(valid.begin(), valid.end() - 1);
  out = make_custom_settings();
  EXPECT_TRUE(decode(truncated, &out) == DecodeStatus::kBadLength);
  EXPECT_TRUE(same_settings(defaults, out));

  std::vector<uint8_t> bad_length = valid;
  bad_length[6] = static_cast<uint8_t>(bad_length[6] - 1);
  out = make_custom_settings();
  EXPECT_TRUE(decode(bad_length, &out) == DecodeStatus::kBadLength);
  EXPECT_TRUE(same_settings(defaults, out));

  // A single flipped bit inside the payload must be caught by the CRC.
  std::vector<uint8_t> corrupted = valid;
  corrupted[kHeaderSize + 8] =
      static_cast<uint8_t>(corrupted[kHeaderSize + 8] ^ 0x01u);
  out = make_custom_settings();
  EXPECT_TRUE(decode(corrupted, &out) == DecodeStatus::kBadCrc);
  EXPECT_TRUE(same_settings(defaults, out));

  // A null output pointer must not crash.
  EXPECT_TRUE(deserialize_settings(valid.data(), valid.size(), nullptr) ==
              DecodeStatus::kEmptyOrShort);
}

// --- Sanitize -------------------------------------------------------------

TEST_CASE(sanitize_settings_clamps_high_values) {
  DeviceSettings settings =
      thermostat::storage::default_device_settings();
  settings.active_zones = 200;

  auto& zone = settings.zones[0];
  zone.control.mode = static_cast<Mode>(99);
  zone.control.target_temp_centi = 9000;
  zone.control.repeat_on_minutes = 200;
  zone.control.freeze_protect_centi = 9000;
  zone.control.hysteresis_centi = 900;
  zone.control.min_on_sec = 60000;
  zone.control.min_off_sec = 60000;
  zone.control.keep_alive_interval_sec = 60000;
  zone.control.keep_alive_off_sec = 0;
  zone.control.strategy = static_cast<StrategyKind>(7);
  zone.sensor_source = static_cast<SensorSourceSetting>(7);
  zone.selected_remote_count = 200;
  zone.sensor_offset_centi = 9000;

  // Unterminated strings must be forced to terminate.
  std::memset(zone.selected_remote_ids[0], 'y',
              thermostat::storage::kMaxSensorIdLen);
  std::memset(settings.homeserver_url, 'x',
              thermostat::storage::kMaxUrlLen);

  thermostat::storage::sanitize_settings(&settings);

  EXPECT_EQ(4, static_cast<int>(settings.active_zones));
  EXPECT_EQ(static_cast<int>(Mode::kOff),
            static_cast<int>(zone.control.mode));
  EXPECT_EQ(3000, zone.control.target_temp_centi);
  EXPECT_EQ(60, static_cast<int>(zone.control.repeat_on_minutes));
  EXPECT_EQ(1500, zone.control.freeze_protect_centi);
  EXPECT_EQ(500, zone.control.hysteresis_centi);
  EXPECT_EQ(3600, static_cast<int>(zone.control.min_on_sec));
  EXPECT_EQ(3600, static_cast<int>(zone.control.min_off_sec));
  EXPECT_EQ(7200, static_cast<int>(zone.control.keep_alive_interval_sec));
  EXPECT_EQ(1, static_cast<int>(zone.control.keep_alive_off_sec));
  EXPECT_EQ(static_cast<int>(StrategyKind::kHysteresis),
            static_cast<int>(zone.control.strategy));
  EXPECT_EQ(static_cast<int>(SensorSourceSetting::kBoardOnly),
            static_cast<int>(zone.sensor_source));
  EXPECT_EQ(4, static_cast<int>(zone.selected_remote_count));
  EXPECT_EQ(500, zone.sensor_offset_centi);
  EXPECT_EQ('\0', zone.selected_remote_ids[0]
                      [thermostat::storage::kMaxSensorIdLen - 1]);
  EXPECT_EQ('\0',
            settings.homeserver_url[thermostat::storage::kMaxUrlLen - 1]);
}

TEST_CASE(sanitize_settings_clamps_low_values) {
  DeviceSettings settings =
      thermostat::storage::default_device_settings();
  settings.active_zones = 0;

  auto& zone = settings.zones[1];
  zone.control.target_temp_centi = -100;
  zone.control.freeze_protect_centi = 100;
  zone.control.hysteresis_centi = -20;
  zone.sensor_offset_centi = -9000;
  zone.control.keep_alive_interval_sec = 0;

  thermostat::storage::sanitize_settings(&settings);

  EXPECT_EQ(1, static_cast<int>(settings.active_zones));
  EXPECT_EQ(500, zone.control.target_temp_centi);
  EXPECT_EQ(500, zone.control.freeze_protect_centi);
  EXPECT_EQ(0, zone.control.hysteresis_centi);
  EXPECT_EQ(-500, zone.sensor_offset_centi);
  EXPECT_EQ(60, static_cast<int>(zone.control.keep_alive_interval_sec));
}

// --- DeferredWriteGuard ---------------------------------------------------

TEST_CASE(deferred_write_waits_for_debounce) {
  DeferredWriteGuard guard(5000, 30000);
  const std::vector<uint8_t> blob = {1, 2, 3};

  EXPECT_FALSE(guard.is_dirty());
  guard.mark_dirty(1000, blob);
  EXPECT_TRUE(guard.is_dirty());
  EXPECT_FALSE(guard.should_flush(1000));
  EXPECT_FALSE(guard.should_flush(5999));
  EXPECT_TRUE(guard.should_flush(6000));

  guard.mark_flushed(6000, blob);
  EXPECT_FALSE(guard.is_dirty());
  EXPECT_FALSE(guard.should_flush(6000));
}

TEST_CASE(deferred_write_restarts_debounce_on_each_change) {
  DeferredWriteGuard guard(5000, 30000);
  const std::vector<uint8_t> first = {1, 2, 3};
  const std::vector<uint8_t> second = {1, 2, 4};

  guard.mark_dirty(1000, first);
  guard.mark_dirty(3000, second);
  EXPECT_FALSE(guard.should_flush(5999));
  EXPECT_FALSE(guard.should_flush(7999));
  EXPECT_TRUE(guard.should_flush(8000));

  // Re-marking the same pending content is not a change and must not push the
  // deadline out again.
  guard.mark_dirty(8000, second);
  EXPECT_TRUE(guard.should_flush(8000));
}

TEST_CASE(deferred_write_skips_unchanged_and_reverted_blobs) {
  DeferredWriteGuard guard(1000, 5000);
  const std::vector<uint8_t> saved = {1, 2, 3};
  const std::vector<uint8_t> changed = {1, 2, 4};

  guard.mark_dirty(0, saved);
  EXPECT_TRUE(guard.should_flush(1000));
  guard.mark_flushed(1000, saved);

  // Same content as the last write: no dirty state, no flash write.
  guard.mark_dirty(2000, saved);
  EXPECT_FALSE(guard.is_dirty());
  EXPECT_FALSE(guard.should_flush(100000));

  // User changed the value...
  guard.mark_dirty(3000, changed);
  EXPECT_TRUE(guard.is_dirty());
  // ...then changed it back to the saved value: the write is dropped.
  guard.mark_dirty(3500, saved);
  EXPECT_FALSE(guard.is_dirty());
  EXPECT_FALSE(guard.should_flush(100000));
}

TEST_CASE(deferred_write_enforces_min_write_interval) {
  DeferredWriteGuard guard(1000, 10000);
  const std::vector<uint8_t> first = {1};
  const std::vector<uint8_t> second = {2};

  // The first write only needs the debounce delay, not the cooldown.
  guard.mark_dirty(0, first);
  EXPECT_FALSE(guard.should_flush(999));
  EXPECT_TRUE(guard.should_flush(1000));
  guard.mark_flushed(1000, first);

  // The next change has settled at 3000, but the cooldown lasts until 11000.
  guard.mark_dirty(2000, second);
  EXPECT_FALSE(guard.should_flush(3000));
  EXPECT_FALSE(guard.should_flush(10999));
  EXPECT_TRUE(guard.should_flush(11000));
}

THERMOSTAT_TEST_MAIN()
