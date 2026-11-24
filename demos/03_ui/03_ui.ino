// 03_ui — LVGL touch UI demo with fake thermostat state.
//
// Verifies the Waveshare ESP32-S3-Touch-LCD-4.3B display/touch bring-up, the
// Korean fonts, the settings panel, the UiCallbacks wiring, and the memory
// headroom while Wi-Fi is active.  No thermostat control logic is connected:
// the view model is filled with fake values and the source badge cycles every
// 10 seconds through normal / warning / error states.

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>
#include <esp_display_panel.hpp>
#include <lvgl.h>

#include "config/pins.h"
#include "lvgl_port.h"
#include "thermostat_ui.h"
#include "ui_view_model.h"

#if __has_include("config/secrets.h")
#include "config/secrets.h"
#else
#include "config/secrets.h.example"
#endif

namespace {

using thermostat::ui::SourceSeverity;
using thermostat::ui::ThermostatUi;
using thermostat::ui::UiCallbacks;
using thermostat::ui::UiMode;
using thermostat::ui::UiViewModel;

constexpr uint32_t kMemoryLogIntervalMs = 5000;
constexpr uint32_t kSourceCycleIntervalMs = 10000;
constexpr uint32_t kUiRefreshIntervalMs = 1000;

struct FakeSourceBadge {
  const char* text;
  SourceSeverity severity;
};

// The three source states from CLAUDE.md section 11:
// normal, 5-minute-old warning, and board fallback error.
const FakeSourceBadge kFakeSources[] = {
    {"거실 센서 · 3분 전", SourceSeverity::kNormal},
    {"평균(거실, 안방) · 6분 전", SourceSeverity::kWarning},
    {"보드 센서 · 원격 센서 응답 없음", SourceSeverity::kError},
};
constexpr size_t kFakeSourceCount = sizeof(kFakeSources) / sizeof(kFakeSources[0]);

esp_panel::board::Board g_board;
ThermostatUi g_ui;
UiViewModel g_view_model;
bool g_ui_ready = false;
size_t g_source_index = 0;
uint32_t g_last_memory_log_ms = 0;
uint32_t g_last_source_cycle_ms = 0;
uint32_t g_last_ui_refresh_ms = 0;

bool wifi_credentials_configured() {
  return std::strncmp(WIFI_SSID, "YOUR_", 5) != 0;
}

void refresh_ui() {
  if (!g_ui_ready) {
    return;
  }
  if (thermostat::ui::lvgl_port_lock(-1)) {
    g_ui.update(g_view_model);
    thermostat::ui::lvgl_port_unlock();
  }
}

void apply_source_badge() {
  const FakeSourceBadge& source = kFakeSources[g_source_index];
  std::snprintf(g_view_model.source_text, sizeof(g_view_model.source_text), "%s",
                source.text);
  g_view_model.source_severity = source.severity;
}

void update_clock_text() {
  // Fake clock starting at 21:15 so the demo does not depend on NTP.
  const uint32_t total_seconds = 21UL * 3600UL + 15UL * 60UL + millis() / 1000UL;
  std::snprintf(g_view_model.time_text, sizeof(g_view_model.time_text),
                "%02u:%02u", static_cast<unsigned>((total_seconds / 3600UL) % 24UL),
                static_cast<unsigned>((total_seconds / 60UL) % 60UL));
}

void cycle_source() {
  g_source_index = (g_source_index + 1) % kFakeSourceCount;
  apply_source_badge();
  // Alternate the heating badge so both colors are visible in the demo.
  g_view_model.heating_active = !g_view_model.heating_active;
  Serial.printf("[03_ui] source=%s heating=%d\n", g_view_model.source_text,
                g_view_model.heating_active ? 1 : 0);
  refresh_ui();
}

void cycle_mode() {
  switch (g_view_model.mode) {
    case UiMode::kOff:
      g_view_model.mode = UiMode::kTemperature;
      break;
    case UiMode::kTemperature:
      g_view_model.mode = UiMode::kRepeat;
      break;
    case UiMode::kRepeat:
    default:
      g_view_model.mode = UiMode::kOff;
      break;
  }
  Serial.printf("[03_ui] mode=%u\n", static_cast<unsigned>(g_view_model.mode));
  refresh_ui();
}

void handle_serial_commands() {
  while (Serial.available() > 0) {
    const int command = Serial.read();
    switch (command) {
      case 's':
        cycle_source();
        break;
      case 'h':
        g_view_model.heating_active = !g_view_model.heating_active;
        Serial.printf("[03_ui] heating=%d\n",
                      g_view_model.heating_active ? 1 : 0);
        refresh_ui();
        break;
      case 'm':
        cycle_mode();
        break;
      case '+':
        g_view_model.target_temp_centi = thermostat::ui::step_target_temp(
            g_view_model.target_temp_centi, 50);
        Serial.printf("[03_ui] target=%d centi\n",
                      g_view_model.target_temp_centi);
        refresh_ui();
        break;
      case '-':
        g_view_model.target_temp_centi = thermostat::ui::step_target_temp(
            g_view_model.target_temp_centi, -50);
        Serial.printf("[03_ui] target=%d centi\n",
                      g_view_model.target_temp_centi);
        refresh_ui();
        break;
      case '?':
        Serial.println(
            "[03_ui] commands: s=next source h=toggle heating m=next mode "
            "+/-=target ?=help");
        break;
      default:
        break;  // Ignore CR/LF and unknown bytes.
    }
  }
}

void on_target_temp_change(int16_t new_target_centi, void*) {
  Serial.printf("[03_ui] cb target=%d centi\n", new_target_centi);
}

void on_mode_change(UiMode new_mode, void*) {
  Serial.printf("[03_ui] cb mode=%u\n", static_cast<unsigned>(new_mode));
}

void on_repeat_minutes_change(uint8_t new_minutes, void*) {
  Serial.printf("[03_ui] cb repeat=%u min\n", static_cast<unsigned>(new_minutes));
}

void on_freeze_protect_change(int16_t new_freeze_centi, void*) {
  Serial.printf("[03_ui] cb freeze=%d centi\n", new_freeze_centi);
}

void connect_wifi() {
  WiFi.mode(WIFI_STA);
  if (wifi_credentials_configured()) {
    Serial.println("[03_ui] WiFi connecting with config/secrets.h values");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    Serial.println(
        "[03_ui] WiFi credentials are placeholders; STA mode initialized only");
  }
}

void log_memory() {
  Serial.printf("[03_ui] heap_free=%u psram_free=%u\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getFreePsram()));
  if (wifi_credentials_configured()) {
    Serial.printf("[03_ui] wifi_status=%d rssi=%d\n",
                  static_cast<int>(WiFi.status()),
                  static_cast<int>(WiFi.RSSI()));
  }
}

}  // namespace

void setup() {
  Serial.begin(thermostat::pins::kSerialBaud);
  // The Waveshare 4.3B routes UART0 to RS485, so USB CDC must come up before
  // any log is visible.  Wait at most 3 s so a missing host does not block boot.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  Serial.println("[03_ui] boot");
  log_memory();
  connect_wifi();

  if (!g_board.init()) {
    Serial.println("[03_ui] board init failed (display stays off)");
    return;
  }
  if (!g_board.begin()) {
    Serial.println("[03_ui] board begin failed (display stays off)");
    return;
  }
  if (!thermostat::ui::lvgl_port_init(g_board.getLCD(), g_board.getTouch())) {
    Serial.println("[03_ui] lvgl port init failed");
    return;
  }

  UiCallbacks callbacks;
  callbacks.on_target_temp_change = &on_target_temp_change;
  callbacks.on_mode_change = &on_mode_change;
  callbacks.on_repeat_minutes_change = &on_repeat_minutes_change;
  callbacks.on_freeze_protect_change = &on_freeze_protect_change;
  callbacks.ctx = nullptr;

  if (!thermostat::ui::lvgl_port_lock(-1)) {
    Serial.println("[03_ui] lvgl lock failed");
    return;
  }
  g_ui.begin(callbacks);
  thermostat::ui::lvgl_port_unlock();
  g_ui_ready = true;

  apply_source_badge();
  update_clock_text();
  refresh_ui();

  Serial.println(
      "[03_ui] commands: s=next source h=toggle heating m=next mode "
      "+/-=target ?=help");
  Serial.printf(
      "[03_ui] ui ready lcd=%dx%d touch=%s\n", g_board.getLCD()->getFrameWidth(),
      g_board.getLCD()->getFrameHeight(),
      (g_board.getTouch() != nullptr) ? "yes" : "no");
}

void loop() {
  const uint32_t now = millis();

  if (now - g_last_source_cycle_ms >= kSourceCycleIntervalMs) {
    g_last_source_cycle_ms = now;
    cycle_source();
  }
  if (now - g_last_ui_refresh_ms >= kUiRefreshIntervalMs) {
    g_last_ui_refresh_ms = now;
    update_clock_text();
    refresh_ui();
  }
  if (now - g_last_memory_log_ms >= kMemoryLogIntervalMs) {
    g_last_memory_log_ms = now;
    log_memory();
  }

  handle_serial_commands();
  delay(10);
}
