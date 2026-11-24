#include <Arduino.h>

#include "config/pins.h"
// Use the library entry header: the ESP32 core ships its own
// cores/esp32/core_version.h, so including "core_version.h" directly would
// resolve to the core header and the thermostat_core library would not be
// linked.
#include "thermostat_core.h"

namespace {
uint32_t g_tick = 0;
}  // namespace

void setup() {
  Serial.begin(thermostat::pins::kSerialBaud);

  // The Waveshare 4.3B routes UART0 to RS485, so USB CDC must come up before
  // any log is visible. Wait at most 3 s so a missing host does not block boot.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  const unsigned long flash_mb =
      static_cast<unsigned long>(ESP.getFlashChipSize() / (1024UL * 1024UL));
  const unsigned long psram_bytes =
      static_cast<unsigned long>(ESP.getPsramSize());

  Serial.printf(
      "[00_smoke] thermostat-esp32 boot core=%s flash_mb=%lu psram_bytes=%lu\n",
      thermostat::core::version_string(), flash_mb, psram_bytes);
}

void loop() {
  const int clamp_test = thermostat::core::clamp_setpoint_centi(3500);

  Serial.printf("[00_smoke] tick=%lu uptime_ms=%lu clamp_test=%d\n",
                static_cast<unsigned long>(g_tick),
                static_cast<unsigned long>(millis()), clamp_test);
  ++g_tick;
  delay(1000);
}
