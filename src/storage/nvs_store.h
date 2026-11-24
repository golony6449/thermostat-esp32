#pragma once

// ESP32 NVS adapter for DeviceSettings.
//
// The class only exists in Arduino builds: host tests link settings_codec.cpp
// alone, so Preferences.h never reaches the host toolchain. The adapter keeps
// the debounce/wear-leveling policy in DeferredWriteGuard (pure C++) and only
// performs the actual flash write here.

#include "settings_codec.h"

#ifdef ARDUINO

#include <Preferences.h>

#include <string>
#include <vector>

namespace thermostat::storage {

class NvsSettingsStore {
 public:
  // Opens the NVS namespace read-write. Defaults match the device contract:
  // namespace "thermostat", key "cfg_blob".
  bool begin(const char* ns = "thermostat", const char* key = "cfg_blob");

  // Reads and verifies the stored blob. A missing or corrupt entry leaves
  // `*out` at safe defaults and returns the reason
  // (kEmptyOrShort when nothing is stored).
  DecodeStatus load(DeviceSettings* out);

  // Sanitizes and packs the settings, then schedules a debounced write.
  void request_save(const DeviceSettings& settings, uint64_t now_ms);

  // Performs the pending write when the debounce/cooldown allow it. Returns
  // true only when NVS was actually written.
  bool tick(uint64_t now_ms);

  // Writes immediately (explicit "save" command, or before shutting down).
  bool save_now(const DeviceSettings& settings, uint64_t now_ms);

  // Removes the stored blob. Returns true when no blob remains afterwards.
  bool erase();

  bool is_dirty() const;

 private:
  bool write_blob(const std::vector<uint8_t>& blob);

  Preferences prefs_;
  std::string namespace_;
  std::string key_;
  bool begun_ = false;
  DeferredWriteGuard guard_;
  std::vector<uint8_t> pending_blob_;
};

}  // namespace thermostat::storage

#endif  // ARDUINO
