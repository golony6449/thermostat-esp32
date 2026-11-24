#pragma once

// Persistent thermostat settings and their deterministic binary codec.
//
// This header and settings_codec.cpp are pure C++17: no Arduino.h,
// Preferences.h or ESP-IDF dependency is allowed here so the codec can be
// exercised by tools/test_host.py with g++ -std=c++17 -Wall -Wextra -Werror.
//
// The on-flash format is little-endian and packs every field explicitly; it
// never relies on struct padding or alignment:
//
//   magic           uint32  ("THST" = 0x54485354)
//   schema_version  uint16
//   payload_len     uint16
//   payload         payload_len bytes
//   crc32           uint32  (IEEE 802.3, over magic..payload)
//
// All multi-byte values are little-endian. The CRC covers every byte before
// the CRC field itself.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/thermostat_types.h"

namespace thermostat::storage {

constexpr uint32_t kSettingsMagic = 0x54485354u;  // "THST"
constexpr uint16_t kSettingsSchemaVersion = 1;

constexpr size_t kMaxSensorIdLen = 32;
constexpr size_t kMaxSelectedRemoteSensors = 4;
constexpr size_t kMaxUrlLen = 128;

// Sanitize ranges. They mirror the limits documented in CLAUDE.md (target
// 5..30 degC, freeze protection 5..15 degC) and keep any value written by a
// future/buggy firmware from driving the heater outside safe bounds.
constexpr int16_t kMinTargetTempCenti = 500;
constexpr int16_t kMaxTargetTempCenti = 3000;
constexpr uint8_t kMaxRepeatOnMinutes = 60;
constexpr int16_t kMinFreezeProtectCenti = 500;
constexpr int16_t kMaxFreezeProtectCenti = 1500;
constexpr int16_t kMinHysteresisCenti = 0;
constexpr int16_t kMaxHysteresisCenti = 500;
constexpr uint16_t kMaxMinOnOffSec = 3600;
constexpr uint16_t kMinKeepAliveIntervalSec = 60;
constexpr uint16_t kMaxKeepAliveIntervalSec = 7200;
constexpr uint16_t kMinKeepAliveOffSec = 1;
constexpr uint16_t kMaxKeepAliveOffSec = 300;
constexpr int16_t kMinSensorOffsetCenti = -500;
constexpr int16_t kMaxSensorOffsetCenti = 500;

// Which temperature source a zone uses. Mirrors the sensor layer's selection
// modes without depending on it.
enum class SensorSourceSetting : uint8_t {
  kBoardOnly = 0,
  kSingleRemote = 1,
  kAverageRemote = 2,
};

// Everything that must survive a reboot for one zone.
struct ZonePersistedSettings {
  thermostat::core::ZoneConfig control;
  SensorSourceSetting sensor_source = SensorSourceSetting::kBoardOnly;
  uint8_t selected_remote_count = 0;
  char selected_remote_ids[kMaxSelectedRemoteSensors][kMaxSensorIdLen] = {};
  int16_t sensor_offset_centi = 0;  // range -500..500
};

// Everything that must survive a reboot for the device. The home-server API
// key is deliberately not part of this model: only non-secret configuration
// is stored on flash.
struct DeviceSettings {
  uint8_t active_zones = 1;  // range 1..thermostat::core::kMaxZones
  ZonePersistedSettings zones[thermostat::core::kMaxZones];
  char homeserver_url[kMaxUrlLen] = "";
};

// Safe factory defaults: one active zone, off mode, 21.0 degC target,
// 10.0 degC freeze protection, 5 minute minimum run/idle times.
DeviceSettings default_device_settings();

// Clamps every numeric field into its documented range, maps unknown enum
// values to their safe default and guarantees null-terminated strings.
void sanitize_settings(DeviceSettings* settings);

// Standard IEEE 802.3 CRC-32 (reflected polynomial 0xEDB88320).
uint32_t compute_crc32(const uint8_t* data, size_t len);

// Packs the settings exactly as described above. The caller's values are
// encoded as-is; call sanitize_settings() first when the input is untrusted.
std::vector<uint8_t> serialize_settings(const DeviceSettings& settings);

enum class DecodeStatus : uint8_t {
  kOk = 0,
  kEmptyOrShort,
  kBadMagic,
  kUnsupportedVersion,
  kBadLength,
  kBadCrc,
};

// Decodes and verifies a settings blob. On any failure `*out` is reset to
// default_device_settings() so callers can boot with safe values without a
// second step. On success the decoded values are sanitized before being
// returned.
DecodeStatus deserialize_settings(const uint8_t* data, size_t len,
                                  DeviceSettings* out);

// Delays flash writes so a burst of UI changes results in one NVS write.
//
// Protects two things:
//   * debounce_ms: wait for the value to settle before writing.
//   * min_write_interval_ms: never write more often than this, so flash wear
//     stays bounded even when the user keeps adjusting the setpoint.
//
// A candidate blob identical to the last blob actually written is discarded
// immediately, which avoids a flash write when the user changes a value and
// then returns it to the saved value.
class DeferredWriteGuard {
 public:
  explicit DeferredWriteGuard(uint64_t debounce_ms = 5000,
                              uint64_t min_write_interval_ms = 30000);

  // Records a new candidate blob. Re-marking the same pending content does
  // not restart the debounce window, because nothing changed.
  void mark_dirty(uint64_t now_ms, const std::vector<uint8_t>& candidate_blob);

  // True when a write is due. `now_ms` must be a monotonic millisecond clock.
  bool should_flush(uint64_t now_ms) const;

  // Records that the pending blob was successfully written.
  void mark_flushed(uint64_t now_ms, const std::vector<uint8_t>& written_blob);

  bool is_dirty() const;

  // Forgets all history (used after the store is erased).
  void reset();

 private:
  uint64_t debounce_ms_;
  uint64_t min_write_interval_ms_;
  bool dirty_ = false;
  bool has_written_ = false;
  uint64_t last_dirty_ms_ = 0;
  uint64_t last_write_ms_ = 0;
  std::vector<uint8_t> pending_blob_;
  std::vector<uint8_t> last_written_blob_;
};

}  // namespace thermostat::storage
