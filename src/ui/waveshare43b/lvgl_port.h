#pragma once

// LVGL 8.4 port for the Waveshare ESP32-S3-Touch-LCD-4.3B.
//
// The board itself is brought up with ESP32_Display_Panel 1.0.0
// (esp_panel::board::Board): init() creates the LCD/touch/backlight objects,
// begin() starts them.  To avoid tearing, the RGB panel gets two frame
// buffers and LVGL renders straight into them (direct mode); a buffer is
// shown only from the next panel frame (vsync), never while it is drawn.
//
// Call order:
//   board.init();
//   lvgl_port_prepare_lcd(board.getLCD());   // before begin()
//   board.begin();
//   lvgl_port_init(board.getLCD(), board.getTouch());

#include <stdint.h>

#include <esp_display_panel.hpp>
#include <lvgl.h>

namespace thermostat::ui {

// Configures the LCD for the port (two RGB frame buffers).
// Call after board.init() and before board.begin().
bool lvgl_port_prepare_lcd(esp_panel::drivers::LCD* lcd);

// Registers the LVGL display and input drivers and starts the LVGL task.
// Call after board.begin().  Fails if lvgl_port_prepare_lcd() was skipped.
bool lvgl_port_init(esp_panel::drivers::LCD* lcd,
                    esp_panel::drivers::Touch* touch);

// Number of flushes that gave up waiting for the panel vsync (diagnostics).
uint32_t lvgl_port_vsync_timeouts();

// Recursive mutex protecting every LVGL API call made outside the LVGL task.
// timeout_ms < 0 waits forever.
bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock();

}  // namespace thermostat::ui
