#pragma once

// Public entry header for the thermostat_core Arduino library.
//
// Do not include "core_version.h" directly from a sketch: the ESP32 Arduino
// core ships its own cores/esp32/core_version.h, and the unqualified include
// resolves to that header first. Arduino CLI then never detects this library
// as used and skips linking it. Including this header first makes the library
// detection work; the quoted include below resolves next to this file.
#include "core_version.h"
#include "thermostat_types.h"
#include "thermostat_controller.h"
