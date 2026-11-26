// SCRUM-242 - integrated SmartThings thermostat firmware.
//
// One heating-only Matter Thermostat endpoint drives the SmartThings routine
// that switches the smart plug. The firmware decides when heating is needed:
//
//   SHT40 / home-server sensors -> thermostat::core -> Matter RunningState
//                                                   -> NVS settings
//                                                   -> LVGL touch UI
//
// Threading rules (see docs/superpowers/plans/2026-11-25-...-wave5):
//   * Matter callbacks run on the CHIP task and LVGL callbacks run on the LVGL
//     task. Both only record a pending event under a portMUX_TYPE critical
//     section and return immediately; they never take the other stack's lock.
//   * loop() (single task) drains the events, advances ThermostatApp and
//     applies the resulting actions to NVS, Matter and the UI.
//
// I2C note: <Wire.h> must never be included here. ESP32_Display_Panel 1.0.0
// installs the ESP-IDF legacy I2C driver for the touch panel / IO expander;
// Arduino's Wire would link the new i2c_master driver into the same binary and
// ESP-IDF panics on boot with "CONFLICT! driver_ng is not allowed to be used
// with the legacy driver". SHT40 and RTC use <driver/i2c.h> on the same port.
//
// Wi-Fi note: with CONFIG_ENABLE_CHIPOBLE=1 the Matter stack owns the STA
// interface using the credentials stored during commissioning. Never call
// WiFi.begin() in that build; only the !CONFIG_ENABLE_CHIPOBLE fallback joins
// Wi-Fi from secrets.h.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_display_panel.hpp>
#include <esp_sntp.h>
#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config/pins.h"
#include "thermostat_core.h"
#include "sensor_logic.h"
#include "sht40_driver.h"
#include "thermostat_net.h"
#include "thermostat_storage.h"
#include "lvgl_port.h"
#include "thermostat_ui.h"
#include "thermostat_app.h"
// Keep Matter headers last so arduino-cli discovers all other libraries before
// paying the heavy esp_matter preprocessor cost once.
#include <Matter.h>
#include "thermostat_matter.h"

// Real credentials live in src/config/secrets.h (gitignored). Fall back to the
// committed placeholder template so the sketch always compiles.
#if __has_include("config/secrets.h")
#include "config/secrets.h"
#else
#include "config/secrets.h.example"
#endif

// GPIO0 is the BOOT button on the ESP32-S3 boards; keep a fallback for cores
// that do not define BOOT_PIN.
#ifndef BOOT_PIN
#define BOOT_PIN 0
#endif

namespace {

using thermostat::app::SyncActions;
using thermostat::app::ThermostatApp;
using thermostat::core::ZoneOutput;
using thermostat::matter::MatterThermostatCallbacks;
using thermostat::matter::ThermostatEndpoint;
using thermostat::net::ClockRtc;
using thermostat::net::HomeServerClient;
using thermostat::net::HomeTimePolicyInput;
using thermostat::net::should_fetch_home_time;
using thermostat::sensor::Sht40Driver;
using thermostat::storage::DeviceSettings;
using thermostat::storage::NvsSettingsStore;
using thermostat::ui::ThermostatUi;
using thermostat::ui::UiCallbacks;
using thermostat::ui::UiMode;
using thermostat::ui::UiViewModel;

constexpr uint32_t kBootWaitMs = 3000;
constexpr uint32_t kLoopDelayMs = 20;
constexpr uint32_t kSensorPollIntervalMs = 5000;
constexpr uint32_t kStepIntervalMs = 1000;
constexpr uint64_t kHomeServerPollIntervalMs = 60000;
constexpr uint64_t kRtcSyncIntervalMs = 6ULL * 3600ULL * 1000ULL;
constexpr int64_t kSaneEpochSec = 1700000000;
constexpr int16_t kMatterInitialTempCenti = 2100;  // shown until the first sample

esp_panel::board::Board g_board;
ThermostatUi g_ui;
ThermostatApp g_app;
ThermostatEndpoint g_matter;
NvsSettingsStore g_store;
Sht40Driver g_sht40;
ClockRtc g_clock;
HomeServerClient g_home_server;
MatterButton g_boot_button;

bool g_ui_ready = false;
bool g_sht40_ok = false;
bool g_rtc_ok = false;

// ---------------------------------------------------------------------------
// Time-source state (loop task only). SNTP reports COMPLETED exactly once
// after a successful sync and then falls back to RESET, so the loop latches
// the completion; the home-server time policy reads these flags and must never
// override an NTP-synchronised clock.
// ---------------------------------------------------------------------------

bool g_ntp_started = false;
uint64_t g_ntp_started_ms = 0;  // millis() when configTime() was called
bool g_ntp_synced = false;      // latched SNTP_SYNC_STATUS_COMPLETED
bool g_home_time_applied = false;
bool g_home_time_attempted = false;
uint64_t g_home_time_last_attempt_ms = 0;

// ---------------------------------------------------------------------------
// Cross-task pending events. The structs are only written by the CHIP/LVGL
// callbacks under their own spinlock and only read by loop().
// ---------------------------------------------------------------------------

struct PendingMatterEvents {
  bool mode_pending = false;
  bool heat_mode = false;
  bool setpoint_pending = false;
  int16_t setpoint_centi = 0;
};

struct PendingUiEvents {
  bool target_pending = false;
  int16_t target_centi = 0;
  bool mode_pending = false;
  UiMode mode = UiMode::kOff;
  bool repeat_pending = false;
  uint8_t repeat_minutes = 0;
  bool freeze_pending = false;
  int16_t freeze_centi = 0;
};

portMUX_TYPE g_matter_event_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_ui_event_mux = portMUX_INITIALIZER_UNLOCKED;
PendingMatterEvents g_pending_matter;
PendingUiEvents g_pending_ui;

// ---------------------------------------------------------------------------
// Matter callbacks (CHIP task). Record only; loop() applies the change.
// Returning true accepts the controller's request.
// ---------------------------------------------------------------------------

bool on_matter_mode_change(bool heat_mode) {
  portENTER_CRITICAL(&g_matter_event_mux);
  g_pending_matter.mode_pending = true;
  g_pending_matter.heat_mode = heat_mode;
  portEXIT_CRITICAL(&g_matter_event_mux);
  return true;
}

bool on_matter_setpoint_change(int16_t setpoint_centi) {
  portENTER_CRITICAL(&g_matter_event_mux);
  g_pending_matter.setpoint_pending = true;
  g_pending_matter.setpoint_centi = setpoint_centi;
  portEXIT_CRITICAL(&g_matter_event_mux);
  return true;
}

// ---------------------------------------------------------------------------
// LVGL callbacks (LVGL task). Record only; never call Matter from here.
// ---------------------------------------------------------------------------

void on_ui_target_temp_change(int16_t new_target_centi, void*) {
  portENTER_CRITICAL(&g_ui_event_mux);
  g_pending_ui.target_pending = true;
  g_pending_ui.target_centi = new_target_centi;
  portEXIT_CRITICAL(&g_ui_event_mux);
}

void on_ui_mode_change(UiMode new_mode, void*) {
  portENTER_CRITICAL(&g_ui_event_mux);
  g_pending_ui.mode_pending = true;
  g_pending_ui.mode = new_mode;
  portEXIT_CRITICAL(&g_ui_event_mux);
}

void on_ui_repeat_minutes_change(uint8_t new_minutes, void*) {
  portENTER_CRITICAL(&g_ui_event_mux);
  g_pending_ui.repeat_pending = true;
  g_pending_ui.repeat_minutes = new_minutes;
  portEXIT_CRITICAL(&g_ui_event_mux);
}

void on_ui_freeze_protect_change(int16_t new_freeze_centi, void*) {
  portENTER_CRITICAL(&g_ui_event_mux);
  g_pending_ui.freeze_pending = true;
  g_pending_ui.freeze_centi = new_freeze_centi;
  portEXIT_CRITICAL(&g_ui_event_mux);
}

// Returns true when at least one event was applied, so the caller can step the
// controller immediately instead of waiting for the 1 s tick.
bool drain_pending_events() {
  PendingMatterEvents matter_events;
  portENTER_CRITICAL(&g_matter_event_mux);
  matter_events = g_pending_matter;
  g_pending_matter = PendingMatterEvents{};
  portEXIT_CRITICAL(&g_matter_event_mux);

  PendingUiEvents ui_events;
  portENTER_CRITICAL(&g_ui_event_mux);
  ui_events = g_pending_ui;
  g_pending_ui = PendingUiEvents{};
  portEXIT_CRITICAL(&g_ui_event_mux);

  if (matter_events.mode_pending) {
    g_app.on_matter_mode(matter_events.heat_mode);
  }
  if (matter_events.setpoint_pending) {
    g_app.on_matter_setpoint(matter_events.setpoint_centi);
  }
  if (ui_events.target_pending) {
    g_app.on_ui_target_temp(ui_events.target_centi);
  }
  if (ui_events.mode_pending) {
    g_app.on_ui_mode(ui_events.mode);
  }
  if (ui_events.repeat_pending) {
    g_app.on_ui_repeat_minutes(ui_events.repeat_minutes);
  }
  if (ui_events.freeze_pending) {
    g_app.on_ui_freeze_protect(ui_events.freeze_centi);
  }

  return matter_events.mode_pending || matter_events.setpoint_pending ||
         ui_events.target_pending || ui_events.mode_pending ||
         ui_events.repeat_pending || ui_events.freeze_pending;
}

// ---------------------------------------------------------------------------
// Action application (loop task only)
// ---------------------------------------------------------------------------

void apply_sync_actions(uint64_t now_ms) {
  const SyncActions actions = g_app.take_actions();

  if (actions.settings_dirty) {
    g_store.request_save(g_app.settings(), now_ms);
  }
  if (actions.push_matter_mode) {
    g_matter.set_heat_mode(actions.matter_heat_mode);
  }
  if (actions.push_matter_setpoint) {
    g_matter.set_heating_setpoint_centi(actions.matter_setpoint_centi);
  }
  if (actions.push_matter_temp) {
    g_matter.set_local_temperature_centi(actions.matter_temp_centi);
  }
  if (actions.push_matter_heating) {
    g_matter.set_heating_active(actions.matter_heating);
  }
  if (actions.ui_dirty && g_ui_ready) {
    // A busy LVGL task must never block the control loop; the next dirty tick
    // redraws with the latest view model anyway.
    if (thermostat::ui::lvgl_port_lock(50)) {
      g_ui.update(g_app.build_view_model(g_clock.now_epoch_sec()));
      thermostat::ui::lvgl_port_unlock();
    }
  }
}

// ---------------------------------------------------------------------------
// Network, time, sensors
// ---------------------------------------------------------------------------

const char* homeserver_api_key() {
  return thermostat::net::is_placeholder_value(HOMESERVER_API_KEY)
             ? ""
             : HOMESERVER_API_KEY;
}

void configure_home_server() {
  g_home_server.configure(g_app.settings().homeserver_url, homeserver_api_key());
}

void poll_network_and_time(uint64_t now_ms) {
  static bool rtc_synced = false;
  static uint64_t last_rtc_sync_ms = 0;

  if (!g_ntp_started) {
    // Non-blocking: SNTP keeps running in the background.
    configTime(9L * 3600L, 0, "pool.ntp.org");
    g_ntp_started = true;
    g_ntp_started_ms = now_ms;
    Serial.println("[main] NTP sync started (KST)");
  }

  // sntp_get_sync_status() returns COMPLETED once and then RESET again, so a
  // poll that skips this window would never see the sync. Latch it here; the
  // home-time policy stops asking the home server as soon as this is true.
  if (!g_ntp_synced &&
      esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    g_ntp_synced = true;
    Serial.println("[main] NTP sync completed");
  }

  const int64_t epoch_sec = g_clock.now_epoch_sec();
  if (epoch_sec < kSaneEpochSec) {
    return;
  }
  if (rtc_synced && now_ms - last_rtc_sync_ms < kRtcSyncIntervalMs) {
    return;
  }

  rtc_synced = true;
  last_rtc_sync_ms = now_ms;
  if (g_rtc_ok && g_clock.write_rtc_epoch(epoch_sec)) {
    Serial.println("[main] RTC saved from system time");
  }
}

void poll_home_server(uint64_t now_ms) {
  if (!g_home_server.configured()) {
    return;
  }
  if (!g_home_server.should_poll(now_ms, kHomeServerPollIntervalMs)) {
    return;
  }

  const HomeServerClient::FetchResult result = g_home_server.fetch_sensors();
  g_home_server.mark_polled(now_ms);
  if (!result.ok) {
    g_app.on_remote_fetch_failed();
    Serial.printf("[main] home server fetch failed: %s\n", result.error.c_str());
    return;
  }
  if (!g_app.on_remote_sensors_json(result.body, g_clock.now_epoch_sec())) {
    Serial.printf("[main] home server JSON parse failed (%u bytes)\n",
                  static_cast<unsigned>(result.body.size()));
  }
}

void poll_home_time(uint64_t now_ms) {
  if (!g_home_server.configured()) {
    return;
  }

  HomeTimePolicyInput policy;
  policy.ntp_synced = g_ntp_synced;
  policy.has_home_time = g_home_time_applied;
  policy.now_ms = now_ms;
  policy.ntp_started_ms = g_ntp_started_ms;
  policy.has_attempted = g_home_time_attempted;
  policy.last_attempt_ms = g_home_time_last_attempt_ms;
  if (!should_fetch_home_time(policy)) {
    return;
  }

  // Record the attempt before the request so a failing server is retried on
  // the policy cadence, not on every loop iteration.
  g_home_time_attempted = true;
  g_home_time_last_attempt_ms = now_ms;

  const HomeServerClient::FetchResult result = g_home_server.fetch_time();
  if (!result.ok) {
    Serial.printf("[main] home time fetch failed: %s\n", result.error.c_str());
    return;
  }
  int64_t epoch_sec = 0;
  if (!thermostat::sensor::parse_homeserver_time_json(result.body, &epoch_sec)) {
    Serial.printf("[main] home time fetch failed: invalid response (%u bytes)\n",
                  static_cast<unsigned>(result.body.size()));
    return;
  }
  if (epoch_sec < kSaneEpochSec) {
    Serial.printf("[main] home time fetch failed: implausible epoch=%lld\n",
                  static_cast<long long>(epoch_sec));
    return;
  }
  const int64_t diff_sec = epoch_sec - g_clock.now_epoch_sec();
  if (!g_clock.set_system_epoch(epoch_sec)) {
    Serial.println("[main] home time fetch failed: settimeofday failed");
    return;
  }
  g_home_time_applied = true;
  Serial.printf("[main] time set from home server epoch=%lld (diff=%llds)\n",
                static_cast<long long>(epoch_sec),
                static_cast<long long>(diff_sec));
  // Keep the battery-backed RTC in step so a reboot before the next network
  // connection still restores a sane clock.
  if (g_rtc_ok) {
    g_clock.write_rtc_epoch(epoch_sec);
  }
}

void poll_sht40(uint64_t now_ms) {
  static bool has_polled = false;
  static uint32_t last_poll_ms = 0;

  const uint32_t now_ms32 = static_cast<uint32_t>(now_ms);
  if (has_polled && now_ms32 - last_poll_ms < kSensorPollIntervalMs) {
    return;
  }
  has_polled = true;
  last_poll_ms = now_ms32;

  if (!g_sht40_ok) {
    return;
  }
  int16_t temp_centi = 0;
  uint16_t rh_centi = 0;
  if (g_sht40.read(&temp_centi, &rh_centi)) {
    g_app.on_board_temp_sample(temp_centi, g_clock.now_epoch_sec());
  } else {
    Serial.println("[main] SHT40 read failed");
  }
}

void run_control_step(uint64_t now_ms, bool events_drained) {
  static bool has_stepped = false;
  static uint32_t last_step_ms = 0;

  const uint32_t now_ms32 = static_cast<uint32_t>(now_ms);
  if (!events_drained && has_stepped &&
      now_ms32 - last_step_ms < kStepIntervalMs) {
    return;
  }
  has_stepped = true;
  last_step_ms = now_ms32;
  g_app.step(now_ms, g_clock.now_epoch_sec());
}

// ---------------------------------------------------------------------------
// Serial diagnostics (never used for control paths)
// ---------------------------------------------------------------------------

char g_line[128];
size_t g_line_len = 0;

bool parse_float_arg(const char* text, float* out) {
  if (text == nullptr || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const float value = strtof(text, &end);
  if (end == text || end == nullptr || *end != '\0' || !isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

bool celsius_arg_to_centi(const char* text, int16_t* out_centi) {
  float celsius = 0.0f;
  if (!parse_float_arg(text, &celsius) || celsius < -100.0f ||
      celsius > 100.0f) {
    return false;
  }
  *out_centi = static_cast<int16_t>(lroundf(celsius * 100.0f));
  return true;
}

void print_help() {
  Serial.println("[main] commands:");
  Serial.println("  status                 현재 상태 출력");
  Serial.println("  qr                     Matter 페어링 코드/QR 출력");
  Serial.println("  sim <celsius>          센서 대신 시뮬레이션 온도 사용 (예: sim 18.5)");
  Serial.println("  sim off                시뮬레이션 해제");
  Serial.println("  homeserver <url>       홈서버 주소 변경 (예: homeserver http://192.168.0.10)");
  Serial.println("  flush                  NVS 설정 즉시 저장");
  Serial.println("  reboot                 재부팅");
  Serial.println("  decommission           Matter 커미셔닝 해제");
  Serial.println("  help                   이 도움말");
}

void print_status() {
  const DeviceSettings& settings = g_app.settings();
  const ZoneOutput& output = g_app.last_output();
  const thermostat::sensor::ResolvedTemperature& temp = g_app.last_temp();
  const int64_t epoch_sec = g_clock.now_epoch_sec();

  Serial.printf(
      "[main] status mode=%u target=%.2fC repeat=%umin freeze=%.2fC "
      "temp_valid=%d temp=%.2fC source=%s heating=%d matter_heating=%d "
      "freeze_protect=%d commissioned=%d wifi=%d rtc=%d sht40=%d "
      "ntp_synced=%d home_time=%d "
      "epoch=%lld nvs_dirty=%d heap=%lu\n",
      static_cast<unsigned>(settings.zones[0].control.mode),
      static_cast<double>(settings.zones[0].control.target_temp_centi) / 100.0,
      static_cast<unsigned>(settings.zones[0].control.repeat_on_minutes),
      static_cast<double>(settings.zones[0].control.freeze_protect_centi) / 100.0,
      temp.valid ? 1 : 0,
      static_cast<double>(temp.temp_centi) / 100.0, temp.source_label.c_str(),
      output.logical_heating ? 1 : 0, output.matter_heating ? 1 : 0,
      output.freeze_protect_active ? 1 : 0, Matter.isDeviceCommissioned() ? 1 : 0,
      static_cast<int>(WiFi.status()), g_rtc_ok ? 1 : 0, g_sht40_ok ? 1 : 0,
      g_ntp_synced ? 1 : 0, g_home_time_applied ? 1 : 0,
      static_cast<long long>(epoch_sec), g_store.is_dirty() ? 1 : 0,
      static_cast<unsigned long>(ESP.getFreeHeap()));
}

void print_pairing_codes() {
  if (Matter.isDeviceCommissioned()) {
    Serial.println("[main] already commissioned");
    return;
  }
  Serial.printf("[main] manual pairing code: %s\n",
                Matter.getManualPairingCode().c_str());
  // Let USB CDC drain between long lines so the code is not truncated.
  delay(20);
  Serial.printf("[main] onboarding QR code: %s\n",
                Matter.getOnboardingQRCodeUrl().c_str());
  delay(20);
}

void handle_command(char* line) {
  const char* command = strtok(line, " \t");
  if (command == nullptr) {
    return;
  }

  if (strcmp(command, "status") == 0) {
    print_status();
    return;
  }
  if (strcmp(command, "qr") == 0) {
    print_pairing_codes();
    return;
  }
  if (strcmp(command, "sim") == 0) {
    const char* argument = strtok(nullptr, " \t");
    if (argument == nullptr) {
      Serial.println("[main] usage: sim <celsius> | sim off");
      return;
    }
    if (strcmp(argument, "off") == 0) {
      g_app.clear_simulated_temp();
      Serial.println("[main] simulation off");
      return;
    }
    int16_t temp_centi = 0;
    if (!celsius_arg_to_centi(argument, &temp_centi)) {
      Serial.println("[main] usage: sim <celsius> | sim off");
      return;
    }
    g_app.set_simulated_temp(temp_centi);
    Serial.printf("[main] simulated temp = %.2f C\n",
                  static_cast<double>(temp_centi) / 100.0);
    return;
  }
  if (strcmp(command, "homeserver") == 0) {
    const char* url = strtok(nullptr, " \t");
    if (url == nullptr) {
      Serial.println("[main] usage: homeserver <url>");
      return;
    }
    g_app.set_homeserver_url(url);
    configure_home_server();
    Serial.printf("[main] home server = %s\n",
                  g_app.settings().homeserver_url);
    return;
  }
  if (strcmp(command, "flush") == 0) {
    const bool ok = g_store.save_now(g_app.settings(), millis());
    Serial.println(ok ? "[main] NVS saved" : "[main] NVS save failed");
    return;
  }
  if (strcmp(command, "reboot") == 0) {
    Serial.println("[main] rebooting...");
    delay(50);
    ESP.restart();
    return;
  }
  if (strcmp(command, "decommission") == 0) {
    Serial.println("[main] decommissioning Matter node...");
    Matter.decommission();
    return;
  }
  if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
    print_help();
    return;
  }
  Serial.printf("[main] unknown command: %s (type 'help')\n", command);
}

void poll_serial() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) {
      return;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      g_line[g_line_len] = '\0';
      if (g_line_len > 0) {
        handle_command(g_line);
      }
      g_line_len = 0;
      continue;
    }
    if (g_line_len + 1 < sizeof(g_line)) {
      g_line[g_line_len++] = static_cast<char>(ch);
    }
  }
}

void poll_boot_button() {
  matterButtonEvent_t event = MATTER_BUTTON_NONE;
  while ((event = g_boot_button.poll()) != MATTER_BUTTON_NONE) {
    if (event == MATTER_BUTTON_LONG_HOLD) {
      Serial.println("[main] BOOT long hold -> decommission");
      Matter.decommission();
    }
  }
}

void init_matter() {
  // Wi-Fi credentials are owned by the Matter stack; never call WiFi.begin()
  // in this build. selectNetwork just declares the transport.
  Matter.selectNetwork(MATTER_NETWORK_WIFI);

#if !CONFIG_ENABLE_CHIPOBLE
  // Prebuilds without CHIPoBLE cannot receive credentials from the hub.
  if (Matter.isWiFiStationEnabled() &&
      !thermostat::net::is_placeholder_value(WIFI_SSID) &&
      !thermostat::net::is_placeholder_value(WIFI_PASSWORD)) {
    Serial.println("[main] CHIPoBLE disabled: joining Wi-Fi from secrets.h");
    matterConnectWiFi(WIFI_SSID, WIFI_PASSWORD);
  }
#endif

  MatterThermostatCallbacks callbacks;
  callbacks.on_mode_change = on_matter_mode_change;
  callbacks.on_setpoint_change = on_matter_setpoint_change;

  // SCRUM-233 verified the stock ThermostatRunningState works in SmartThings,
  // so the auxiliary contact endpoint is not needed.
  if (!g_matter.begin(callbacks, false)) {
    Serial.println("[main] FATAL: ThermostatEndpoint::begin() failed");
    return;
  }

  Matter.begin();
  g_matter.after_matter_begin(
      kMatterInitialTempCenti,
      g_app.settings().zones[0].control.target_temp_centi,
      g_app.is_matter_heat_mode());
  print_pairing_codes();
}

void init_display() {
  if (!g_board.init()) {
    Serial.println("[main] board init failed (display stays off)");
    return;
  }
  if (!g_board.begin()) {
    Serial.println("[main] board begin failed (display stays off)");
    return;
  }
  if (!thermostat::ui::lvgl_port_init(g_board.getLCD(), g_board.getTouch())) {
    Serial.println("[main] lvgl port init failed");
    return;
  }

  UiCallbacks ui_callbacks;
  ui_callbacks.on_target_temp_change = on_ui_target_temp_change;
  ui_callbacks.on_mode_change = on_ui_mode_change;
  ui_callbacks.on_repeat_minutes_change = on_ui_repeat_minutes_change;
  ui_callbacks.on_freeze_protect_change = on_ui_freeze_protect_change;

  if (!thermostat::ui::lvgl_port_lock(-1)) {
    Serial.println("[main] lvgl lock failed");
    return;
  }
  g_ui_ready = g_ui.begin(ui_callbacks);
  if (g_ui_ready) {
    g_ui.update(g_app.build_view_model(g_clock.now_epoch_sec()));
  }
  thermostat::ui::lvgl_port_unlock();
  Serial.println(g_ui_ready ? "[main] UI ready" : "[main] UI begin failed");
}

void init_board_io() {
  // The display bring-up installed the legacy I2C driver on I2C_NUM_0, which
  // the SHT40 and the RTC share. Both drivers probe gracefully.
  g_sht40_ok = g_sht40.begin(I2C_NUM_0);
  Serial.println(g_sht40_ok ? "[main] SHT40 detected"
                            : "[main] SHT40 not detected (graceful skip)");

  g_rtc_ok = g_clock.begin(I2C_NUM_0);
  if (!g_rtc_ok) {
    Serial.println("[main] RTC PCF85063A not detected (graceful skip)");
  } else if (g_clock.restore_system_time_from_rtc()) {
    Serial.println("[main] system time restored from RTC");
  }
}

}  // namespace

void setup() {
  Serial.begin(thermostat::pins::kSerialBaud);
  // USB CDC must come up before any log is visible; wait at most 3 s so a
  // missing host cannot block boot.
  while (!Serial && millis() < kBootWaitMs) {
    delay(10);
  }

  Serial.println();
  Serial.println("[main] SmartThings DIY thermostat - integrated firmware (SCRUM-242)");

  DeviceSettings loaded_settings;
  g_store.begin();
  const thermostat::storage::DecodeStatus load_status =
      g_store.load(&loaded_settings);
  Serial.printf("[main] NVS load status=%d\n", static_cast<int>(load_status));
  if (loaded_settings.homeserver_url[0] == '\0' &&
      !thermostat::net::is_placeholder_value(HOMESERVER_BASE_URL)) {
    std::snprintf(loaded_settings.homeserver_url,
                  sizeof(loaded_settings.homeserver_url), "%s",
                  HOMESERVER_BASE_URL);
  }
  g_app.init(loaded_settings, millis());

  init_matter();
  init_display();
  init_board_io();

  configure_home_server();
  if (g_home_server.configured()) {
    Serial.printf("[main] home server = %s\n",
                  g_app.settings().homeserver_url);
  } else {
    Serial.println("[main] home server not configured");
  }

  // BOOT button: 5 s long hold decommissions the Matter node.
  g_boot_button.begin(BOOT_PIN);

  print_help();
}

void loop() {
  const uint64_t now_ms = millis();

  const bool events_drained = drain_pending_events();

  // Matter owns the STA interface in CHIPoBLE builds; WiFi.status() covers the
  // !CONFIG_ENABLE_CHIPOBLE fallback where secrets.h joined the network.
  const bool wifi_connected =
      Matter.isWiFiConnected() || WiFi.status() == WL_CONNECTED;
  if (wifi_connected) {
    poll_network_and_time(now_ms);
    poll_home_server(now_ms);
    poll_home_time(now_ms);
  }

  poll_sht40(now_ms);
  run_control_step(now_ms, events_drained);
  apply_sync_actions(now_ms);

  if (g_store.tick(now_ms)) {
    Serial.println("[main] NVS flushed");
  }

  poll_serial();
  poll_boot_button();
  delay(kLoopDelayMs);
}
