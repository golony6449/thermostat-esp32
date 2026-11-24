#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

// Include the library entry header directly (no "storage/" prefix): Arduino
// CLI only detects thermostat_storage as used, and therefore compiles and
// links its .cpp files, when the include matches a header inside the library.
#include "thermostat_storage.h"

namespace {

using thermostat::core::Mode;
using thermostat::storage::DecodeStatus;
using thermostat::storage::DeviceSettings;
using thermostat::storage::NvsSettingsStore;

constexpr uint32_t kSerialBaud = 115200;
constexpr unsigned kMaxCommandLen = 96;

NvsSettingsStore g_store;
DeviceSettings g_settings;
String g_line;

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::kOff:
      return "off";
    case Mode::kTemperature:
      return "temp";
    case Mode::kRepeat:
      return "repeat";
  }
  return "off";
}

const char* decode_status_name(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::kOk:
      return "kOk";
    case DecodeStatus::kEmptyOrShort:
      return "kEmptyOrShort";
    case DecodeStatus::kBadMagic:
      return "kBadMagic";
    case DecodeStatus::kUnsupportedVersion:
      return "kUnsupportedVersion";
    case DecodeStatus::kBadLength:
      return "kBadLength";
    case DecodeStatus::kBadCrc:
      return "kBadCrc";
  }
  return "unknown";
}

void print_settings(const char* label) {
  const auto& control = g_settings.zones[0].control;
  Serial.printf(
      "[04_storage] %s target=%.2fC mode=%s repeat_min=%u freeze=%.2fC "
      "homeserver=%s\n",
      label, static_cast<double>(control.target_temp_centi) / 100.0,
      mode_name(control.mode),
      static_cast<unsigned>(control.repeat_on_minutes),
      static_cast<double>(control.freeze_protect_centi) / 100.0,
      g_settings.homeserver_url[0] != '\0' ? g_settings.homeserver_url
                                          : "(none)");
}

bool parse_mode(const char* text, Mode* out) {
  if (std::strcmp(text, "off") == 0) {
    *out = Mode::kOff;
    return true;
  }
  if (std::strcmp(text, "temp") == 0) {
    *out = Mode::kTemperature;
    return true;
  }
  if (std::strcmp(text, "repeat") == 0) {
    *out = Mode::kRepeat;
    return true;
  }
  return false;
}

void handle_set(const String& line) {
  float temp_c = 0.0f;
  char mode_text[16] = {};
  unsigned repeat_min = 0;
  const int matched =
      std::sscanf(line.c_str(), "set %f %15s %u", &temp_c, mode_text, &repeat_min);
  Mode mode = Mode::kOff;
  if (matched != 3 || !parse_mode(mode_text, &mode)) {
    Serial.println(
        "[04_storage] usage: set <temp_c> <off|temp|repeat> <repeat_min>");
    return;
  }

  // Clamp before narrowing so an absurd input cannot wrap into a bogus value;
  // sanitize_settings() then applies the documented ranges.
  if (temp_c < 0.0f) {
    temp_c = 0.0f;
  }
  if (temp_c > 100.0f) {
    temp_c = 100.0f;
  }
  if (repeat_min > 255u) {
    repeat_min = 255u;
  }

  auto& control = g_settings.zones[0].control;
  control.target_temp_centi =
      static_cast<int16_t>(std::lroundf(temp_c * 100.0f));
  control.mode = mode;
  control.repeat_on_minutes = static_cast<uint8_t>(repeat_min);

  thermostat::storage::sanitize_settings(&g_settings);
  g_store.request_save(g_settings, millis());

  print_settings("updated");
  Serial.println("[04_storage] deferred save scheduled");
}

void handle_command(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }

  if (line == "show") {
    print_settings("show");
    return;
  }
  if (line == "flush") {
    if (g_store.save_now(g_settings, millis())) {
      Serial.println("[04_storage] saved immediately");
    } else {
      Serial.println("[04_storage] save failed");
    }
    return;
  }
  if (line == "erase") {
    const bool empty = g_store.erase();
    g_settings = thermostat::storage::default_device_settings();
    Serial.printf("[04_storage] erase %s\n", empty ? "ok" : "failed");
    print_settings("erase");
    return;
  }
  if (line == "reboot") {
    Serial.println("[04_storage] rebooting...");
    Serial.flush();
    delay(100);
    ESP.restart();
    return;
  }
  if (line.startsWith("set ")) {
    handle_set(line);
    return;
  }

  Serial.println(
      "[04_storage] unknown command "
      "(show|set <temp_c> <off|temp|repeat> <repeat_min>|flush|erase|reboot)");
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  if (!g_store.begin()) {
    Serial.println("[04_storage] NVS begin failed");
  }

  const DecodeStatus status = g_store.load(&g_settings);
  Serial.printf("[04_storage] load status=%s\n", decode_status_name(status));
  if (status == DecodeStatus::kOk) {
    Serial.println("[04_storage] NVS 설정을 복원했습니다");
  } else {
    Serial.println(
        "[04_storage] 저장된 설정이 없거나 손상되어 기본값을 사용합니다");
  }
  print_settings("boot");

  Serial.println(
      "[04_storage] commands: show | set <temp_c> <off|temp|repeat> "
      "<repeat_min> | flush | erase | reboot");
}

void loop() {
  if (g_store.tick(millis())) {
    Serial.println("[04_storage] NVS flushed");
  }

  static bool overflow = false;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (g_line.length() > 0) {
        handle_command(g_line);
      } else if (overflow) {
        Serial.println("[04_storage] command too long");
      }
      g_line = "";
      overflow = false;
    } else if (g_line.length() < kMaxCommandLen) {
      g_line += c;
    } else {
      overflow = true;
    }
  }

  delay(10);
}
