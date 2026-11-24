#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#ifdef ARDUINO
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#endif

namespace thermostat::net {

// True when a secrets.h value is missing or still the `YOUR_...` placeholder
// from `config/secrets.h.example`. Shared by the demo and the HTTP client so a
// placeholder API key is never sent as a header.
inline bool is_placeholder_value(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return true;
  }
  return std::strncmp(value, "YOUR_", 5) == 0;
}

#ifdef ARDUINO

// Minimal HTTP client for the home-server remote sensor API:
//
//   GET <base_url>/api/thermostat/sensors
//
// The response body is handed to the pure sensor logic for JSON parsing; this
// class only owns transport concerns (timeout, API-key header, poll cadence).
class HomeServerClient {
 public:
  struct FetchResult {
    bool ok = false;
    int http_status = 0;
    std::string body;
    std::string error;
  };

  void configure(const char* base_url, const char* api_key = "",
                 uint16_t timeout_ms = 3000);

  FetchResult fetch_sensors();

  // 1-minute polling cadence used by the firmware (CLAUDE.md section 10):
  // `should_poll()` is true before the first poll and then once per interval.
  bool should_poll(uint64_t now_ms, uint64_t interval_ms = 60000) const;
  void mark_polled(uint64_t now_ms);

  const std::string& base_url() const { return base_url_; }
  bool configured() const { return !base_url_.empty(); }

 private:
  std::string base_url_;
  std::string api_key_;
  uint16_t timeout_ms_ = 3000;
  bool has_polled_ = false;
  uint64_t last_poll_ms_ = 0;
};

#endif  // ARDUINO

}  // namespace thermostat::net
