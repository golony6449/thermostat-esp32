#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#ifdef ARDUINO
#include <Arduino.h>
#include <HTTPClient.h>
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

// --- Home-server time policy ----------------------------------------------
//
// NTP is the preferred time source; the home server only fills in when SNTP
// has not completed yet. The firmware polls SNTP from its loop, so the policy
// takes plain inputs instead of calling Arduino/ESP-IDF functions and stays
// host-testable.

struct HomeTimePolicyInput {
  bool ntp_synced = false;      // SNTP completed at least once
  bool has_home_time = false;   // a home-server time was applied at least once
  uint64_t now_ms = 0;
  uint64_t ntp_started_ms = 0;  // when configTime() was called
  bool has_attempted = false;   // a home-time fetch was tried at least once
  uint64_t last_attempt_ms = 0;
};

constexpr uint64_t kHomeTimeNtpGraceMs = 30000;  // give NTP the first chance
constexpr uint64_t kHomeTimeRetryMs = 60000;     // retry gap before success
constexpr uint64_t kHomeTimeRefreshMs =
    6ULL * 3600ULL * 1000ULL;  // refresh gap after success

// True only while NTP has not completed: waits kHomeTimeNtpGraceMs after
// configTime(), then fetches once, retries every kHomeTimeRetryMs until the
// first success and every kHomeTimeRefreshMs afterwards. Once SNTP completes,
// the home-server time is never fetched again (NTP owns the clock).
inline bool should_fetch_home_time(const HomeTimePolicyInput& in) {
  if (in.ntp_synced) {
    return false;
  }
  const uint64_t since_ntp_start =
      in.now_ms >= in.ntp_started_ms ? in.now_ms - in.ntp_started_ms : 0;
  if (since_ntp_start < kHomeTimeNtpGraceMs) {
    return false;
  }
  if (!in.has_attempted) {
    return true;
  }
  const uint64_t since_attempt =
      in.now_ms >= in.last_attempt_ms ? in.now_ms - in.last_attempt_ms : 0;
  return since_attempt >=
         (in.has_home_time ? kHomeTimeRefreshMs : kHomeTimeRetryMs);
}

#ifdef ARDUINO

// Minimal HTTP client for the home-server APIs:
//
//   GET <base_url>/api/thermostat/sensors
//   GET <base_url>/api/thermostat/time
//
// The response body is handed to the pure sensor logic for JSON parsing; this
// class only owns transport concerns (timeout, API-key header, poll cadence).
//
// Connectivity is deliberately not checked here. In CONFIG_ENABLE_CHIPOBLE
// builds the Matter stack owns the STA interface and WiFi.status() can report
// something other than WL_CONNECTED while the interface is actually up, so the
// caller decides when the network is usable (loop() checks
// Matter.isWiFiConnected() || WiFi.status() == WL_CONNECTED). Without a
// network the HTTP request fails and surfaces through the normal error path.
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
  FetchResult fetch_time();

  // 1-minute polling cadence used by the firmware (CLAUDE.md section 10):
  // `should_poll()` is true before the first poll and then once per interval.
  bool should_poll(uint64_t now_ms, uint64_t interval_ms = 60000) const;
  void mark_polled(uint64_t now_ms);

  const std::string& base_url() const { return base_url_; }
  bool configured() const { return !base_url_.empty(); }

 private:
  FetchResult fetch_path(const char* path);

  std::string base_url_;
  std::string api_key_;
  uint16_t timeout_ms_ = 3000;
  bool has_polled_ = false;
  uint64_t last_poll_ms_ = 0;
};

#endif  // ARDUINO

}  // namespace thermostat::net
