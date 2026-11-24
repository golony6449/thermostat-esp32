// SCRUM-233 - Matter spike for the DIY SmartThings thermostat.
//
// Creates one heating-only Thermostat endpoint plus an auxiliary contact
// sensor endpoint, then exposes serial commands so the SmartThings routine
// conditions (ThermostatRunningState, 70-minute dead-man timer, keep-alive
// pulse length) can be measured on a real hub.
//
// Build: python tools/build.py demos/01_matter
// Never upload/monitor from automated checks (COM9 is user-owned).

#include <Arduino.h>
#include <Matter.h>

// Real credentials live in src/config/secrets.h (gitignored). Fall back to
// the committed placeholder template so the sketch always compiles.
#if __has_include("config/secrets.h")
#include "config/secrets.h"
#else
#include "config/secrets.h.example"
#endif

#include "thermostat_matter.h"

// The ESP32 Arduino core defines BOOT_PIN for boards that have a BOOT button
// (GPIO0 on ESP32-S3). Keep a local fallback in case it is not defined.
#ifndef BOOT_PIN
#define BOOT_PIN 0
#endif

namespace {

using thermostat::matter::ThermostatEndpoint;

constexpr int16_t kInitialTempCenti = 2100;      // 21.00 C
constexpr int16_t kInitialSetpointCenti = 2100;  // 21.00 C

ThermostatEndpoint g_endpoint;
MatterButton g_boot_button;

int16_t g_temp_centi = kInitialTempCenti;
int16_t g_setpoint_centi = kInitialSetpointCenti;
bool g_heat_mode = false;
bool g_heating_active = false;

bool g_pulse_active = false;
uint32_t g_pulse_end_ms = 0;

char g_line[64];
size_t g_line_len = 0;

#if !CONFIG_ENABLE_CHIPOBLE
bool wifi_credentials_configured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 &&
         strcmp(WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") != 0;
}
#endif

const char* on_off(bool value) {
  return value ? "ON" : "OFF";
}

const char* ok_text(bool ok) {
  return ok ? "ok" : "failed";
}

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

bool parse_uint_arg(const char* text, uint32_t* out, uint32_t max_value) {
  if (text == nullptr || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = strtoul(text, &end, 10);
  if (end == text || end == nullptr || *end != '\0' || value > max_value) {
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

// Converts a user-supplied Celsius value for the fake temperature/setpoint.
// Rejects input outside a sane demo range instead of overflowing int16_t.
bool celsius_arg_to_centi(const char* text, int16_t* out_centi) {
  float celsius = 0.0f;
  if (!parse_float_arg(text, &celsius) || celsius < -100.0f || celsius > 100.0f) {
    return false;
  }
  *out_centi = static_cast<int16_t>(lroundf(celsius * 100.0f));
  return true;
}

int16_t clamp_setpoint_centi(int16_t setpoint_centi) {
  if (setpoint_centi < thermostat::matter::kMinHeatSetpointCenti) {
    return thermostat::matter::kMinHeatSetpointCenti;
  }
  if (setpoint_centi > thermostat::matter::kMaxHeatSetpointCenti) {
    return thermostat::matter::kMaxHeatSetpointCenti;
  }
  return setpoint_centi;
}

bool on_mode_change(bool heat_mode) {
  g_heat_mode = heat_mode;
  Serial.printf("[01_matter] app changed mode -> %s\n", heat_mode ? "Heat" : "Off");
  return true;
}

bool on_setpoint_change(int16_t setpoint_centi) {
  g_setpoint_centi = setpoint_centi;
  Serial.printf("[01_matter] app changed setpoint -> %.2f C\n",
                static_cast<double>(setpoint_centi) / 100.0);
  return true;
}

void print_help() {
  Serial.println("[01_matter] commands:");
  Serial.println("  t <celsius>     가짜 현재 온도 (예: t 19.5)");
  Serial.println("  s <celsius>     목표 온도, 7.0~30.0 C (예: s 23.0)");
  Serial.println("  m off | m heat  모드 전환");
  Serial.println("  h 1 | h 0       난방 작동 상태 수동 ON/OFF (RunningState + 보조 센서)");
  Serial.println("  p <seconds>     유지 신호 펄스: 현재 ON이면 sec초 OFF 후 다시 ON");
  Serial.println("  status          현재 온도/목표/모드/난방/커미셔닝/메모리 출력");
  Serial.println("  decommission    Matter.decommission() (BOOT 5초 길게 눌러도 동작)");
}

void print_status() {
  Serial.printf(
      "[01_matter] status temp=%.2fC setpoint=%.2fC mode=%s heating=%s pulse=%s "
      "commissioned=%s running_state_supported=%s aux_contact=%s endpoint=%u "
      "heap_free=%lu psram_free=%lu\n",
      static_cast<double>(g_temp_centi) / 100.0,
      static_cast<double>(g_setpoint_centi) / 100.0, g_heat_mode ? "Heat" : "Off",
      on_off(g_heating_active), g_pulse_active ? "active" : "idle",
      Matter.isDeviceCommissioned() ? "yes" : "no",
      g_endpoint.is_running_state_supported() ? "yes" : "no",
      g_endpoint.is_aux_contact_enabled() ? "yes" : "no",
      static_cast<unsigned>(g_endpoint.endpoint_id()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getFreePsram()));
}

void handle_command(char* line) {
  const char* command = strtok(line, " \t");
  if (command == nullptr) {
    return;
  }

  if (strcmp(command, "t") == 0) {
    int16_t temp_centi = 0;
    if (!celsius_arg_to_centi(strtok(nullptr, " \t"), &temp_centi)) {
      Serial.println("[01_matter] usage: t <celsius>   (예: t 19.5)");
      return;
    }
    g_temp_centi = temp_centi;
    const bool ok = g_endpoint.set_local_temperature_centi(temp_centi);
    Serial.printf("[01_matter] local temperature -> %.2f C (%s)\n",
                  static_cast<double>(temp_centi) / 100.0, ok_text(ok));
    return;
  }

  if (strcmp(command, "s") == 0) {
    int16_t setpoint_centi = 0;
    if (!celsius_arg_to_centi(strtok(nullptr, " \t"), &setpoint_centi)) {
      Serial.println("[01_matter] usage: s <celsius>   (예: s 23.0)");
      return;
    }
    const int16_t clamped = clamp_setpoint_centi(setpoint_centi);
    g_setpoint_centi = clamped;
    const bool ok = g_endpoint.set_heating_setpoint_centi(clamped);
    Serial.printf("[01_matter] heating setpoint -> %.2f C (%s)\n",
                  static_cast<double>(clamped) / 100.0, ok_text(ok));
    return;
  }

  if (strcmp(command, "m") == 0) {
    const char* mode = strtok(nullptr, " \t");
    bool heat_mode = false;
    if (mode == nullptr || (strcmp(mode, "off") != 0 && strcmp(mode, "heat") != 0)) {
      Serial.println("[01_matter] usage: m off | m heat");
      return;
    }
    heat_mode = (strcmp(mode, "heat") == 0);
    g_heat_mode = heat_mode;
    const bool ok = g_endpoint.set_heat_mode(heat_mode);
    Serial.printf("[01_matter] system mode -> %s (%s)\n", heat_mode ? "Heat" : "Off",
                  ok_text(ok));
    return;
  }

  if (strcmp(command, "h") == 0) {
    const char* state = strtok(nullptr, " \t");
    if (state == nullptr || (strcmp(state, "1") != 0 && strcmp(state, "0") != 0)) {
      Serial.println("[01_matter] usage: h 1 | h 0");
      return;
    }
    const bool heating = (strcmp(state, "1") == 0);
    g_pulse_active = false;
    g_heating_active = heating;
    const bool ok = g_endpoint.set_heating_active(heating);
    Serial.printf("[01_matter] heating active -> %s (%s)\n", on_off(heating), ok_text(ok));
    return;
  }

  if (strcmp(command, "p") == 0) {
    uint32_t seconds = 0;
    if (!parse_uint_arg(strtok(nullptr, " \t"), &seconds, 600) || seconds == 0) {
      Serial.println("[01_matter] usage: p <seconds 1..600>");
      return;
    }
    if (!g_heating_active) {
      Serial.println("[01_matter] pulse ignored: heating is OFF (use 'h 1' first)");
      return;
    }
    if (!g_endpoint.set_heating_active(false)) {
      Serial.println("[01_matter] pulse failed: could not switch heating OFF");
      return;
    }
    g_heating_active = false;
    g_pulse_active = true;
    g_pulse_end_ms = millis() + seconds * 1000UL;
    Serial.printf(
        "[01_matter] keep-alive pulse: RunningState/contact OFF for %lu s, then ON\n",
        static_cast<unsigned long>(seconds));
    return;
  }

  if (strcmp(command, "status") == 0) {
    print_status();
    return;
  }

  if (strcmp(command, "decommission") == 0) {
    Serial.println("[01_matter] decommissioning Matter node...");
    Matter.decommission();
    return;
  }

  if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
    print_help();
    return;
  }

  Serial.printf("[01_matter] unknown command: %s (type 'help')\n", command);
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

void poll_pulse() {
  if (!g_pulse_active) {
    return;
  }
  if (static_cast<int32_t>(millis() - g_pulse_end_ms) < 0) {
    return;
  }
  g_pulse_active = false;
  g_heating_active = true;
  const bool ok = g_endpoint.set_heating_active(true);
  Serial.printf("[01_matter] keep-alive pulse finished -> heating ON (%s)\n", ok_text(ok));
}

void poll_boot_button() {
  matterButtonEvent_t event = MATTER_BUTTON_NONE;
  while ((event = g_boot_button.poll()) != MATTER_BUTTON_NONE) {
    if (event == MATTER_BUTTON_LONG_HOLD) {
      Serial.println("[01_matter] BOOT long hold -> decommission");
      Matter.decommission();
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB CDC must come up before the first log is visible (UART0 goes to RS485
  // on the Waveshare 4.3B). Wait at most 3 s so a missing host cannot block.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println("[01_matter] DIY SmartThings thermostat - Matter spike (SCRUM-233)");

  // BOOT button helper: active-low, 5 s long hold emits MATTER_BUTTON_LONG_HOLD.
  g_boot_button.begin(BOOT_PIN);

#if !CONFIG_ENABLE_CHIPOBLE
  // Prebuilds without CHIPoBLE cannot receive Wi-Fi credentials from the hub.
  if (Matter.isWiFiStationEnabled() && wifi_credentials_configured()) {
    Serial.println("[01_matter] no CHIPoBLE build: joining Wi-Fi from secrets.h");
    matterConnectWiFi(WIFI_SSID, WIFI_PASSWORD);
  }
#endif

  thermostat::matter::MatterThermostatCallbacks callbacks;
  callbacks.on_mode_change = on_mode_change;
  callbacks.on_setpoint_change = on_setpoint_change;

  if (!g_endpoint.begin(callbacks, true)) {
    Serial.println("[01_matter] FATAL: ThermostatEndpoint::begin() failed");
    return;
  }

  // Endpoints must exist before Matter.begin(); RunningState / contact sensor
  // setters run only after it.
  Matter.begin();
  g_endpoint.after_matter_begin(g_temp_centi, g_setpoint_centi, g_heat_mode);

  Serial.printf(
      "[01_matter] endpoint=%u running_state_supported=%s aux_contact=%s mode=%s\n",
      static_cast<unsigned>(g_endpoint.endpoint_id()),
      g_endpoint.is_running_state_supported() ? "yes" : "no",
      g_endpoint.is_aux_contact_enabled() ? "yes" : "no", "Off");

  if (!Matter.isDeviceCommissioned()) {
    Serial.printf("[01_matter] manual pairing code: %s\n",
                  Matter.getManualPairingCode().c_str());
    Serial.printf("[01_matter] onboarding QR code: %s\n",
                  Matter.getOnboardingQRCodeUrl().c_str());
  } else {
    Serial.println("[01_matter] already commissioned");
  }

  print_help();
}

void loop() {
  poll_serial();
  poll_pulse();
  poll_boot_button();
  delay(10);
}
