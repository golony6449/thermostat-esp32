#pragma once

// LVGL 8.4 touch UI for the Waveshare ESP32-S3-Touch-LCD-4.3B thermostat panel.
//
// The widgets only render the pure C++ UiViewModel and forward user actions
// through UiCallbacks.  All LVGL calls must be made while the LVGL port mutex
// (thermostat::ui::lvgl_port_lock) is held.

#include <lvgl.h>

#include "ui_view_model.h"

namespace thermostat::ui {

class ThermostatUi {
 public:
  ThermostatUi() = default;

  // Creates the widget tree on the active screen.  Returns false when called
  // twice without a successful first call.  Call with the LVGL lock held.
  bool begin(const UiCallbacks& callbacks);

  // Copies the view model and refreshes every label, badge, and button color.
  void update(const UiViewModel& vm);

  void show_settings(bool visible);
  bool settings_visible() const { return settings_visible_; }

  const UiViewModel& view_model() const { return vm_; }

 private:
  enum class Action : uint8_t {
    kNone = 0,
    kTargetMinus,
    kTargetPlus,
    kModeOff,
    kModeTemperature,
    kModeRepeat,
    kOpenSettings,
    kCloseSettings,
    kRepeatMinus,
    kRepeatPlus,
    kFreezeMinus,
    kFreezePlus,
  };

  static void on_clicked(lv_event_t* event);
  void handle_action(Action action);

  lv_obj_t* create_card(lv_obj_t* parent, int x, int y, int width, int height);
  lv_obj_t* create_label(lv_obj_t* parent, const char* text,
                         const lv_font_t* font, uint32_t color);
  lv_obj_t* create_button(lv_obj_t* parent, const char* text, Action action,
                          int x, int y, int width, int height, bool circle,
                          const lv_font_t* font);
  void apply_button_style(lv_obj_t* button, bool selected);

  void build_status_column(lv_obj_t* parent);
  void build_control_column(lv_obj_t* parent);
  void build_settings_overlay(lv_obj_t* parent);
  void refresh_all();
  void refresh_mode_buttons();
  void refresh_source_badge();
  void refresh_settings_values();

  UiCallbacks callbacks_{};
  UiViewModel vm_{};
  bool started_ = false;
  bool settings_visible_ = false;

  lv_obj_t* current_temp_label_ = nullptr;
  lv_obj_t* heating_badge_ = nullptr;
  lv_obj_t* heating_label_ = nullptr;
  lv_obj_t* source_badge_ = nullptr;
  lv_obj_t* source_label_ = nullptr;
  lv_obj_t* time_label_ = nullptr;
  lv_obj_t* target_temp_label_ = nullptr;
  lv_obj_t* mode_buttons_[3] = {nullptr, nullptr, nullptr};
  lv_obj_t* repeat_value_label_ = nullptr;
  lv_obj_t* freeze_value_label_ = nullptr;
  lv_obj_t* settings_overlay_ = nullptr;
};

}  // namespace thermostat::ui
