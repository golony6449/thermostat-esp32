#include "nvs_store.h"

#ifdef ARDUINO

namespace thermostat::storage {
namespace {

constexpr const char* kDefaultNamespace = "thermostat";
constexpr const char* kDefaultKey = "cfg_blob";

DeviceSettings sanitized_copy(const DeviceSettings& settings) {
  DeviceSettings safe = settings;
  sanitize_settings(&safe);
  return safe;
}

}  // namespace

bool NvsSettingsStore::begin(const char* ns, const char* key) {
  namespace_ = (ns != nullptr && ns[0] != '\0') ? ns : kDefaultNamespace;
  key_ = (key != nullptr && key[0] != '\0') ? key : kDefaultKey;
  begun_ = prefs_.begin(namespace_.c_str(), false);
  return begun_;
}

DecodeStatus NvsSettingsStore::load(DeviceSettings* out) {
  if (out == nullptr) {
    return DecodeStatus::kEmptyOrShort;
  }
  *out = default_device_settings();
  if (!begun_ && !begin()) {
    return DecodeStatus::kEmptyOrShort;
  }

  const size_t stored_len = prefs_.getBytesLength(key_.c_str());
  if (stored_len == 0) {
    return DecodeStatus::kEmptyOrShort;
  }

  std::vector<uint8_t> blob(stored_len);
  const size_t read_len = prefs_.getBytes(key_.c_str(), blob.data(), stored_len);
  if (read_len != stored_len) {
    // A partial read cannot be trusted; keep the safe defaults.
    return DecodeStatus::kEmptyOrShort;
  }

  return deserialize_settings(blob.data(), blob.size(), out);
}

void NvsSettingsStore::request_save(const DeviceSettings& settings,
                                    uint64_t now_ms) {
  pending_blob_ = serialize_settings(sanitized_copy(settings));
  guard_.mark_dirty(now_ms, pending_blob_);
}

bool NvsSettingsStore::tick(uint64_t now_ms) {
  if (!begun_ || !guard_.should_flush(now_ms)) {
    return false;
  }
  if (!write_blob(pending_blob_)) {
    return false;
  }
  guard_.mark_flushed(now_ms, pending_blob_);
  return true;
}

bool NvsSettingsStore::save_now(const DeviceSettings& settings,
                                uint64_t now_ms) {
  if (!begun_ && !begin()) {
    return false;
  }
  const std::vector<uint8_t> blob =
      serialize_settings(sanitized_copy(settings));
  if (!write_blob(blob)) {
    return false;
  }
  guard_.mark_flushed(now_ms, blob);
  return true;
}

bool NvsSettingsStore::erase() {
  if (!begun_ && !begin()) {
    return false;
  }
  prefs_.remove(key_.c_str());
  const bool empty = prefs_.getBytesLength(key_.c_str()) == 0;
  pending_blob_.clear();
  guard_.reset();
  return empty;
}

bool NvsSettingsStore::is_dirty() const { return guard_.is_dirty(); }

bool NvsSettingsStore::write_blob(const std::vector<uint8_t>& blob) {
  if (!begun_ || blob.empty()) {
    return false;
  }
  const size_t written =
      prefs_.putBytes(key_.c_str(), blob.data(), blob.size());
  return written == blob.size();
}

}  // namespace thermostat::storage

#endif  // ARDUINO
