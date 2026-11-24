#pragma once

// LVGL 8.4 port for the Waveshare ESP32-S3-Touch-LCD-4.3B.
//
// The board itself is brought up with ESP32_Display_Panel 1.0.0
// (esp_panel::board::Board): init() creates the LCD/touch/backlight objects,
// begin() starts them.  After that, lvgl_port_init() registers an RGB LCD
// display driver with PSRAM double buffers plus the GT911 touch input driver,
// and starts the LVGL task.

#include <esp_display_panel.hpp>
#include <lvgl.h>

namespace thermostat::ui {

// Registers the LVGL display and input drivers and starts the LVGL task.
// Call after board.init() and board.begin().
bool lvgl_port_init(esp_panel::drivers::LCD* lcd,
                    esp_panel::drivers::Touch* touch);

// Recursive mutex protecting every LVGL API call made outside the LVGL task.
// timeout_ms < 0 waits forever.
bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock();

}  // namespace thermostat::ui
