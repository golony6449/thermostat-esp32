#include "homeserver_client.h"

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef ARDUINO

namespace thermostat::net {
namespace {

constexpr const char* kSensorsPath = "/api/thermostat/sensors";
constexpr const char* kApiKeyHeader = "X-API-Key";

}  // namespace

void HomeServerClient::configure(const char* base_url, const char* api_key,
                                 uint16_t timeout_ms) {
  base_url_ = (base_url == nullptr) ? std::string() : std::string(base_url);
  while (!base_url_.empty() && base_url_.back() == '/') {
    base_url_.pop_back();
  }
  api_key_ = (api_key == nullptr) ? std::string() : std::string(api_key);
  timeout_ms_ = timeout_ms;
}

HomeServerClient::FetchResult HomeServerClient::fetch_sensors() {
  FetchResult result;
  if (base_url_.empty()) {
    result.error = "home server URL is not configured";
    return result;
  }
  if (WiFi.status() != WL_CONNECTED) {
    result.error = "Wi-Fi is not connected";
    return result;
  }

  const std::string url = base_url_ + kSensorsPath;
  HTTPClient http;
  http.setTimeout(timeout_ms_);
  if (!http.begin(String(url.c_str()))) {
    result.error = "HTTP begin failed";
    return result;
  }
  if (!api_key_.empty() && !is_placeholder_value(api_key_.c_str())) {
    http.addHeader(String(kApiKeyHeader), String(api_key_.c_str()));
  }

  result.http_status = http.GET();
  if (result.http_status == HTTP_CODE_OK) {
    const String payload = http.getString();
    result.body.assign(payload.c_str(),
                       static_cast<size_t>(payload.length()));
    result.ok = true;
  } else if (result.http_status > 0) {
    result.error = "HTTP status " + std::to_string(result.http_status);
  } else {
    result.error =
        "HTTP request failed (" + std::to_string(result.http_status) + ")";
  }
  http.end();
  return result;
}

bool HomeServerClient::should_poll(uint64_t now_ms,
                                   uint64_t interval_ms) const {
  if (!has_polled_) {
    return true;
  }
  return now_ms - last_poll_ms_ >= interval_ms;
}

void HomeServerClient::mark_polled(uint64_t now_ms) {
  last_poll_ms_ = now_ms;
  has_polled_ = true;
}

}  // namespace thermostat::net

#endif  // ARDUINO
