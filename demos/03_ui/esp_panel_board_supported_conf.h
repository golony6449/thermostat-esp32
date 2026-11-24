/*
 * ESP32_Display_Panel board selection for the 03_ui demo.
 *
 * This file lives in the sketch folder, which the Arduino build puts on the
 * include path, so ESP32_Display_Panel 1.0.0 picks it up instead of its own
 * default (no board selected).  800x480 RGB, GT911 touch, CH422G IO expander
 * and CH422G-driven backlight are all configured by the library's official
 * BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B header.
 */
#pragma once

#define ESP_PANEL_BOARD_DEFAULT_USE_SUPPORTED (1)

#if ESP_PANEL_BOARD_DEFAULT_USE_SUPPORTED
#define BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_4_3_B
#endif

/* Version of the ESP32_Display_Panel configuration file format (1.0.0). */
#define ESP_PANEL_BOARD_SUPPORTED_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_SUPPORTED_FILE_VERSION_MINOR 0
#define ESP_PANEL_BOARD_SUPPORTED_FILE_VERSION_PATCH 0
