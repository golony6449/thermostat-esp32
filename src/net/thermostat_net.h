#pragma once

// Entry header for the thermostat_net Arduino library: board clock (NTP with
// PCF85063A RTC backup) and the home-server HTTP client.
//
// Sketches should include this header so arduino-cli detects the module and
// links it; including the individual headers directly also works.

#include "clock_rtc.h"
#include "homeserver_client.h"
