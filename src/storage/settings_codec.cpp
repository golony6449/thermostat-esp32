#include "settings_codec.h"

#include <cstring>

namespace thermostat::storage {
namespace {

constexpr size_t kHeaderSize = 4 /*magic*/ + 2 /*schema_version*/ +
                               2 /*payload_len*/;
constexpr size_t kCrcSize = 4;

// One zone record, in this exact order:
//   mode(1) target(2) repeat(1) freeze(2) hysteresis(2) min_on(2)
//   min_off(2) keep_alive_interval(2) keep_alive_off(2) strategy(1)
constexpr size_t kZoneControlSize = 17;
constexpr size_t kZoneSettingsSize =
    kZoneControlSize + 1 /*sensor_source*/ + 1 /*selected_remote_count*/ +
    kMaxSelectedRemoteSensors * kMaxSensorIdLen + 2 /*sensor_offset*/;
constexpr size_t kPayloadSize =
    1 /*active_zones*/ + core::kMaxZones * kZoneSettingsSize + kMaxUrlLen;
static_assert(kPayloadSize <= 0xFFFFu, "payload must fit into payload_len");

template <typename T>
T clamp_value(T value, T low, T high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

void append_u16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void append_i16(std::vector<uint8_t>* out, int16_t value) {
  append_u16(out, static_cast<uint16_t>(value));
}

void append_u32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

uint16_t read_u16(const uint8_t* data) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                               (static_cast<uint16_t>(data[1]) << 8));
}

int16_t read_i16(const uint8_t* data) {
  return static_cast<int16_t>(read_u16(data));
}

uint32_t read_u32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

// Fixed-width, zero-padded string field so the encoding stays deterministic.
void append_fixed_string(std::vector<uint8_t>* out, const char* text,
                         size_t capacity) {
  size_t length = 0;
  while (length < capacity && text[length] != '\0') {
    ++length;
  }
  out->insert(out->end(), text, text + length);
  out->insert(out->end(), capacity - length, static_cast<uint8_t>(0));
}

// Reads a fixed-width field and always terminates it, whatever the stored
// bytes contained.
void read_fixed_string(const uint8_t* data, size_t capacity, char* out) {
  std::memcpy(out, data, capacity);
  out[capacity - 1] = '\0';
}

}  // namespace

DeviceSettings default_device_settings() { return DeviceSettings{}; }

void sanitize_settings(DeviceSettings* settings) {
  if (settings == nullptr) {
    return;
  }

  settings->active_zones = clamp_value<uint8_t>(
      settings->active_zones, 1, static_cast<uint8_t>(core::kMaxZones));

  for (size_t i = 0; i < core::kMaxZones; ++i) {
    ZonePersistedSettings& zone = settings->zones[i];
    core::ZoneConfig& control = zone.control;

    if (static_cast<uint8_t>(control.mode) >
        static_cast<uint8_t>(core::Mode::kRepeat)) {
      control.mode = core::Mode::kOff;
    }
    control.target_temp_centi = clamp_value<int16_t>(
        control.target_temp_centi, kMinTargetTempCenti, kMaxTargetTempCenti);
    control.repeat_on_minutes = clamp_value<uint8_t>(
        control.repeat_on_minutes, 0, kMaxRepeatOnMinutes);
    control.freeze_protect_centi =
        clamp_value<int16_t>(control.freeze_protect_centi,
                             kMinFreezeProtectCenti, kMaxFreezeProtectCenti);
    control.hysteresis_centi = clamp_value<int16_t>(
        control.hysteresis_centi, kMinHysteresisCenti, kMaxHysteresisCenti);
    control.min_on_sec =
        clamp_value<uint16_t>(control.min_on_sec, 0, kMaxMinOnOffSec);
    control.min_off_sec =
        clamp_value<uint16_t>(control.min_off_sec, 0, kMaxMinOnOffSec);
    control.keep_alive_interval_sec = clamp_value<uint16_t>(
        control.keep_alive_interval_sec, kMinKeepAliveIntervalSec,
        kMaxKeepAliveIntervalSec);
    control.keep_alive_off_sec = clamp_value<uint16_t>(
        control.keep_alive_off_sec, kMinKeepAliveOffSec, kMaxKeepAliveOffSec);
    if (control.strategy != core::StrategyKind::kHysteresis) {
      control.strategy = core::StrategyKind::kHysteresis;
    }

    if (static_cast<uint8_t>(zone.sensor_source) >
        static_cast<uint8_t>(SensorSourceSetting::kAverageRemote)) {
      zone.sensor_source = SensorSourceSetting::kBoardOnly;
    }
    zone.selected_remote_count = clamp_value<uint8_t>(
        zone.selected_remote_count, 0,
        static_cast<uint8_t>(kMaxSelectedRemoteSensors));
    zone.sensor_offset_centi = clamp_value<int16_t>(
        zone.sensor_offset_centi, kMinSensorOffsetCenti, kMaxSensorOffsetCenti);

    for (size_t id = 0; id < kMaxSelectedRemoteSensors; ++id) {
      zone.selected_remote_ids[id][kMaxSensorIdLen - 1] = '\0';
    }
  }

  settings->homeserver_url[kMaxUrlLen - 1] = '\0';
}

uint32_t compute_crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> serialize_settings(const DeviceSettings& settings) {
  std::vector<uint8_t> payload;
  payload.reserve(kPayloadSize);

  payload.push_back(settings.active_zones);
  for (size_t i = 0; i < core::kMaxZones; ++i) {
    const ZonePersistedSettings& zone = settings.zones[i];
    const core::ZoneConfig& control = zone.control;

    payload.push_back(static_cast<uint8_t>(control.mode));
    append_i16(&payload, control.target_temp_centi);
    payload.push_back(control.repeat_on_minutes);
    append_i16(&payload, control.freeze_protect_centi);
    append_i16(&payload, control.hysteresis_centi);
    append_u16(&payload, control.min_on_sec);
    append_u16(&payload, control.min_off_sec);
    append_u16(&payload, control.keep_alive_interval_sec);
    append_u16(&payload, control.keep_alive_off_sec);
    payload.push_back(static_cast<uint8_t>(control.strategy));

    payload.push_back(static_cast<uint8_t>(zone.sensor_source));
    payload.push_back(zone.selected_remote_count);
    for (size_t id = 0; id < kMaxSelectedRemoteSensors; ++id) {
      append_fixed_string(&payload, zone.selected_remote_ids[id],
                          kMaxSensorIdLen);
    }
    append_i16(&payload, zone.sensor_offset_centi);
  }
  append_fixed_string(&payload, settings.homeserver_url, kMaxUrlLen);

  std::vector<uint8_t> blob;
  blob.reserve(kHeaderSize + payload.size() + kCrcSize);
  append_u32(&blob, kSettingsMagic);
  append_u16(&blob, kSettingsSchemaVersion);
  append_u16(&blob, static_cast<uint16_t>(payload.size()));
  blob.insert(blob.end(), payload.begin(), payload.end());
  append_u32(&blob, compute_crc32(blob.data(), blob.size()));
  return blob;
}

DecodeStatus deserialize_settings(const uint8_t* data, size_t len,
                                  DeviceSettings* out) {
  if (out == nullptr) {
    return DecodeStatus::kEmptyOrShort;
  }
  *out = default_device_settings();

  if (data == nullptr || len < kHeaderSize + kCrcSize) {
    return DecodeStatus::kEmptyOrShort;
  }
  if (read_u32(data) != kSettingsMagic) {
    return DecodeStatus::kBadMagic;
  }
  if (read_u16(data + 4) != kSettingsSchemaVersion) {
    return DecodeStatus::kUnsupportedVersion;
  }
  const size_t payload_len = read_u16(data + 6);
  if (payload_len != kPayloadSize) {
    return DecodeStatus::kBadLength;
  }
  if (len != kHeaderSize + payload_len + kCrcSize) {
    return DecodeStatus::kBadLength;
  }
  if (read_u32(data + len - kCrcSize) != compute_crc32(data, len - kCrcSize)) {
    return DecodeStatus::kBadCrc;
  }

  const uint8_t* payload = data + kHeaderSize;
  DeviceSettings decoded = default_device_settings();
  decoded.active_zones = payload[0];

  size_t offset = 1;
  for (size_t i = 0; i < core::kMaxZones; ++i) {
    const uint8_t* zone = payload + offset;
    ZonePersistedSettings& target = decoded.zones[i];

    target.control.mode = static_cast<core::Mode>(zone[0]);
    target.control.target_temp_centi = read_i16(zone + 1);
    target.control.repeat_on_minutes = zone[3];
    target.control.freeze_protect_centi = read_i16(zone + 4);
    target.control.hysteresis_centi = read_i16(zone + 6);
    target.control.min_on_sec = read_u16(zone + 8);
    target.control.min_off_sec = read_u16(zone + 10);
    target.control.keep_alive_interval_sec = read_u16(zone + 12);
    target.control.keep_alive_off_sec = read_u16(zone + 14);
    target.control.strategy = static_cast<core::StrategyKind>(zone[16]);

    target.sensor_source = static_cast<SensorSourceSetting>(zone[17]);
    target.selected_remote_count = zone[18];
    for (size_t id = 0; id < kMaxSelectedRemoteSensors; ++id) {
      read_fixed_string(zone + 19 + id * kMaxSensorIdLen, kMaxSensorIdLen,
                        target.selected_remote_ids[id]);
    }
    target.sensor_offset_centi = read_i16(zone + 147);

    offset += kZoneSettingsSize;
  }
  read_fixed_string(payload + offset, kMaxUrlLen, decoded.homeserver_url);

  sanitize_settings(&decoded);
  *out = decoded;
  return DecodeStatus::kOk;
}

DeferredWriteGuard::DeferredWriteGuard(uint64_t debounce_ms,
                                       uint64_t min_write_interval_ms)
    : debounce_ms_(debounce_ms),
      min_write_interval_ms_(min_write_interval_ms) {}

void DeferredWriteGuard::mark_dirty(
    uint64_t now_ms, const std::vector<uint8_t>& candidate_blob) {
  // The user changed a value and then changed it back to what is already on
  // flash: nothing to write.
  if (has_written_ && candidate_blob == last_written_blob_) {
    dirty_ = false;
    pending_blob_.clear();
    return;
  }
  // Re-marking the exact pending content is not a new change, so keep the
  // original debounce deadline instead of postponing the write forever.
  if (dirty_ && candidate_blob == pending_blob_) {
    return;
  }
  dirty_ = true;
  last_dirty_ms_ = now_ms;
  pending_blob_ = candidate_blob;
}

bool DeferredWriteGuard::should_flush(uint64_t now_ms) const {
  if (!dirty_) {
    return false;
  }
  if (now_ms - last_dirty_ms_ < debounce_ms_) {
    return false;
  }
  if (has_written_ && now_ms - last_write_ms_ < min_write_interval_ms_) {
    return false;
  }
  return true;
}

void DeferredWriteGuard::mark_flushed(
    uint64_t now_ms, const std::vector<uint8_t>& written_blob) {
  dirty_ = false;
  has_written_ = true;
  last_write_ms_ = now_ms;
  last_written_blob_ = written_blob;
  pending_blob_.clear();
}

bool DeferredWriteGuard::is_dirty() const { return dirty_; }

void DeferredWriteGuard::reset() {
  dirty_ = false;
  has_written_ = false;
  last_dirty_ms_ = 0;
  last_write_ms_ = 0;
  pending_blob_.clear();
  last_written_blob_.clear();
}

}  // namespace thermostat::storage
