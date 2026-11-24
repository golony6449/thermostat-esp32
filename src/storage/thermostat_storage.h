#pragma once

// Public entry header for the thermostat_storage Arduino library.
//
// Includes both the portable DeviceSettings codec and, in Arduino builds, the
// ESP32 NVS adapter.
#include "settings_codec.h"
#include "nvs_store.h"
