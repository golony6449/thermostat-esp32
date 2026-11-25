// Board I/O probe: SHT40 temperature/humidity, PCF85063A RTC + NTP, and one
// home-server HTTP fetch. Runs without a sensor attached (graceful skip) and
// without real credentials (secrets.h.example placeholders are detected).
//
// The I2C bus is driven with the ESP-IDF legacy master driver instead of
// <Wire.h>, because ESP32_Display_Panel 1.0.0 (used by the integrated firmware)
// installs the legacy driver and the two driver generations cannot coexist in
// one binary.

#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2c.h>

#include <cstdio>

#include "config/pins.h"

#if __has_include("config/secrets.h")
#include "config/secrets.h"
#else
#include "config/secrets.h.example"
#endif

#include "sht40_driver.h"
#include "thermostat_net.h"
#include "thermostat_sensor_hw.h"

namespace {

constexpr uint32_t kStatusIntervalMs = 10000;
constexpr uint32_t kWifiTimeoutMs = 10000;
constexpr int64_t kSaneEpochSec = 1700000000;  // 2023-11-14, i.e. SNTP synced
constexpr i2c_port_t kSensorI2cPort = I2C_NUM_0;

thermostat::sensor::Sht40Driver g_sht40;
thermostat::net::ClockRtc g_clock;
thermostat::net::HomeServerClient g_home_server;

bool g_sht40_ok = false;
bool g_rtc_ok = false;
uint32_t g_last_log_ms = 0;

// Bounded transfer timeout so a wedged bus cannot hang the probe.
TickType_t i2c_timeout_ticks() { return pdMS_TO_TICKS(50); }

// Installs the legacy I2C master driver shared with the board peripherals.
bool init_i2c() {
  i2c_config_t config = {};
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = thermostat::pins::kI2cSda;
  config.scl_io_num = thermostat::pins::kI2cScl;
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = thermostat::sensor::kSensorI2cClockHz;
  config.clk_flags = 0;

  esp_err_t err = i2c_param_config(kSensorI2cPort, &config);
  if (err != ESP_OK) {
    Serial.printf("[02_io] i2c_param_config failed: %d\n",
                  static_cast<int>(err));
    return false;
  }
  err = i2c_driver_install(kSensorI2cPort, I2C_MODE_MASTER, 0, 0, 0);
  if (err != ESP_OK) {
    Serial.printf("[02_io] i2c_driver_install failed: %d\n",
                  static_cast<int>(err));
    return false;
  }
  return true;
}

bool i2c_probe(uint8_t address) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) {
    return false;
  }
  i2c_master_start(cmd);
  i2c_master_write_byte(
      cmd, static_cast<uint8_t>((address << 1) | I2C_MASTER_WRITE), true);
  i2c_master_stop(cmd);
  const esp_err_t err =
      i2c_master_cmd_begin(kSensorI2cPort, cmd, i2c_timeout_ticks());
  i2c_cmd_link_delete(cmd);
  return err == ESP_OK;
}

void scan_i2c_bus() {
  Serial.println("[02_io] I2C scan:");
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    if (i2c_probe(address)) {
      Serial.printf("[02_io]   found 0x%02X\n", address);
      ++found;
    }
  }
  if (found == 0) {
    Serial.println("[02_io]   no I2C devices responded");
  }
}

void format_centi(char* buffer, size_t buffer_size, int16_t value_centi,
                  const char* unit) {
  if (buffer == nullptr || buffer_size == 0) {
    return;
  }
  const int32_t magnitude = value_centi < 0
                                ? -static_cast<int32_t>(value_centi)
                                : static_cast<int32_t>(value_centi);
  snprintf(buffer, buffer_size, "%s%ld.%02ld%s", value_centi < 0 ? "-" : "",
           static_cast<long>(magnitude / 100),
           static_cast<long>(magnitude % 100), unit);
}

void format_centi_unsigned(char* buffer, size_t buffer_size,
                           uint16_t value_centi, const char* unit) {
  if (buffer == nullptr || buffer_size == 0) {
    return;
  }
  snprintf(buffer, buffer_size, "%lu.%02lu%s",
           static_cast<unsigned long>(value_centi / 100U),
           static_cast<unsigned long>(value_centi % 100U), unit);
}

void format_epoch_utc(char* buffer, size_t buffer_size, int64_t epoch_sec) {
  if (buffer == nullptr || buffer_size == 0) {
    return;
  }
  thermostat::net::UtcDateTime dt;
  if (!thermostat::net::epoch_to_utc_datetime(epoch_sec, &dt)) {
    snprintf(buffer, buffer_size, "invalid(%lld)",
             static_cast<long long>(epoch_sec));
    return;
  }
  snprintf(buffer, buffer_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", dt.year,
           dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

void print_epoch_utc(const char* label, int64_t epoch_sec) {
  char text[40];
  format_epoch_utc(text, sizeof(text), epoch_sec);
  Serial.printf("[02_io] %s: %s (epoch=%lld)\n", label, text,
                static_cast<long long>(epoch_sec));
}

bool connect_wifi(uint32_t timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t started_ms = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started_ms < timeout_ms) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[02_io] Wi-Fi connect failed");
    return false;
  }
  Serial.print("[02_io] Wi-Fi connected: ");
  Serial.println(WiFi.localIP());
  return true;
}

void fetch_homeserver_once() {
  if (thermostat::net::is_placeholder_value(HOMESERVER_BASE_URL)) {
    Serial.println(
        "[02_io] home server not configured (HOMESERVER_BASE_URL placeholder); "
        "skip HTTP");
    return;
  }

  g_home_server.configure(HOMESERVER_BASE_URL, HOMESERVER_API_KEY);
  Serial.printf("[02_io] GET %s/api/thermostat/sensors\n",
                g_home_server.base_url().c_str());
  const thermostat::net::HomeServerClient::FetchResult result =
      g_home_server.fetch_sensors();
  if (!result.ok) {
    Serial.printf("[02_io] home server fetch failed: http_status=%d error=%s\n",
                  result.http_status, result.error.c_str());
    return;
  }
  Serial.printf("[02_io] home server fetch ok: %u bytes\n",
                static_cast<unsigned>(result.body.size()));
  Serial.printf("[02_io] body: %s\n", result.body.c_str());
}

void log_status() {
  char temp_text[24] = "n/a";
  char rh_text[24] = "n/a";
  if (g_sht40_ok) {
    int16_t temp_centi = 0;
    uint16_t rh_centi = 0;
    if (g_sht40.read(&temp_centi, &rh_centi)) {
      format_centi(temp_text, sizeof(temp_text), temp_centi, " C");
      format_centi_unsigned(rh_text, sizeof(rh_text), rh_centi, " %");
    } else {
      snprintf(temp_text, sizeof(temp_text), "read error");
    }
  }

  const int64_t system_epoch = g_clock.now_epoch_sec();
  char time_text[40];
  if (system_epoch >= kSaneEpochSec) {
    format_epoch_utc(time_text, sizeof(time_text), system_epoch);
  } else {
    snprintf(time_text, sizeof(time_text), "unset");
  }

  int64_t rtc_epoch = 0;
  const bool rtc_time_ok = g_rtc_ok && g_clock.read_rtc_epoch(&rtc_epoch);

  Serial.printf(
      "[02_io] status uptime_ms=%lu temp=%s rh=%s system_time=%s "
      "rtc=%s wifi=%d sht40=%d\n",
      static_cast<unsigned long>(millis()), temp_text, rh_text, time_text,
      rtc_time_ok ? "ok" : "n/a", static_cast<int>(WiFi.status()),
      g_sht40_ok ? 1 : 0);
  if (rtc_time_ok) {
    print_epoch_utc("[02_io]   rtc time", rtc_epoch);
  }
}

}  // namespace

void setup() {
  Serial.begin(thermostat::pins::kSerialBaud);
  // USB CDC must come up before any log is visible; wait at most 3 s so a
  // missing host does not block boot.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  Serial.println(
      "[02_io] boot: SHT40 + PCF85063A clock + home-server probe");

  if (!init_i2c()) {
    Serial.println("[02_io] I2C init failed; skip sensor and RTC probes");
  } else {
    scan_i2c_bus();

    g_sht40_ok = g_sht40.begin(kSensorI2cPort);
    if (g_sht40_ok) {
      Serial.printf("[02_io] SHT40 detected at 0x%02X\n", g_sht40.address());
    } else {
      Serial.println("[02_io] SHT40 not detected (graceful skip)");
    }

    g_rtc_ok = g_clock.begin(kSensorI2cPort);
    if (!g_rtc_ok) {
      Serial.println("[02_io] RTC PCF85063A not detected (graceful skip)");
    } else {
      int64_t rtc_epoch = 0;
      if (g_clock.read_rtc_epoch(&rtc_epoch)) {
        print_epoch_utc("[02_io] RTC time", rtc_epoch);
        if (g_clock.restore_system_time_from_rtc()) {
          Serial.println("[02_io] system time restored from RTC");
        } else {
          Serial.println("[02_io] system time restore from RTC failed");
        }
      } else {
        Serial.println(
            "[02_io] RTC time invalid (OS bit set, corrupt registers or read "
            "error)");
      }
    }
  }

  const bool wifi_configured =
      !thermostat::net::is_placeholder_value(WIFI_SSID) &&
      !thermostat::net::is_placeholder_value(WIFI_PASSWORD);
  if (!wifi_configured) {
    Serial.println(
        "[02_io] Wi-Fi not configured (secrets.h placeholders); skip NTP and "
        "home-server probe");
  } else if (!connect_wifi(kWifiTimeoutMs)) {
    Serial.println("[02_io] skip NTP and home-server probe (no Wi-Fi)");
  } else {
    const bool rtc_saved = g_clock.sync_ntp_and_save();
    const int64_t system_epoch = g_clock.now_epoch_sec();
    if (system_epoch >= kSaneEpochSec) {
      print_epoch_utc("[02_io] NTP time", system_epoch);
      Serial.println(rtc_saved ? "[02_io] RTC updated from NTP"
                               : "[02_io] RTC update skipped or failed");
    } else {
      Serial.println("[02_io] NTP sync failed (no valid time received)");
    }
    fetch_homeserver_once();
  }

  g_last_log_ms = millis();
  log_status();
}

void loop() {
  const uint32_t now_ms = millis();
  if (now_ms - g_last_log_ms >= kStatusIntervalMs) {
    g_last_log_ms = now_ms;
    log_status();
  }
  delay(50);
}
