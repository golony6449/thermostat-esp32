// LINK: src/core/core_version.cpp
//
// Host-side tests for the pure C++ UI view model.  The header has no Arduino
// or LVGL dependency, so these tests exercise the same stepping and formatting
// rules the touch UI uses on the ESP32.

#include "test_harness.h"
#include "ui/waveshare43b/ui_view_model.h"

using thermostat::ui::format_temp_celsius;
using thermostat::ui::UiMode;
using thermostat::ui::UiViewModel;
using thermostat::ui::SourceSeverity;
using thermostat::ui::step_freeze_protect;
using thermostat::ui::step_repeat_minutes;
using thermostat::ui::step_target_temp;

TEST_CASE(step_target_temp_moves_in_half_degree_steps) {
  EXPECT_EQ(2300, step_target_temp(2250, 50));
  EXPECT_EQ(2200, step_target_temp(2250, -50));
  EXPECT_EQ(2250, step_target_temp(2250, 0));
  EXPECT_EQ(2150, step_target_temp(2100, 50));
  EXPECT_EQ(2100, step_target_temp(2150, -50));
}

TEST_CASE(step_target_temp_clamps_to_plan_range) {
  EXPECT_EQ(500, step_target_temp(500, -50));
  EXPECT_EQ(500, step_target_temp(520, -50));
  EXPECT_EQ(500, step_target_temp(-100, -50));
  EXPECT_EQ(3000, step_target_temp(3000, 50));
  EXPECT_EQ(3000, step_target_temp(2980, 50));
  EXPECT_EQ(3000, step_target_temp(5000, 50));
  EXPECT_EQ(3000, step_target_temp(2950, 1000));
}

TEST_CASE(step_repeat_minutes_moves_in_five_minute_steps) {
  EXPECT_EQ(20, step_repeat_minutes(15, 5));
  EXPECT_EQ(10, step_repeat_minutes(15, -5));
  EXPECT_EQ(0, step_repeat_minutes(0, -5));
  EXPECT_EQ(60, step_repeat_minutes(60, 5));
  EXPECT_EQ(60, step_repeat_minutes(55, 10));
  EXPECT_EQ(0, step_repeat_minutes(5, -10));
  EXPECT_EQ(15, step_repeat_minutes(15, 0));
}

TEST_CASE(step_freeze_protect_clamps_to_five_and_fifteen_degrees) {
  EXPECT_EQ(1050, step_freeze_protect(1000, 50));
  EXPECT_EQ(950, step_freeze_protect(1000, -50));
  EXPECT_EQ(500, step_freeze_protect(500, -50));
  EXPECT_EQ(500, step_freeze_protect(520, -50));
  EXPECT_EQ(1500, step_freeze_protect(1500, 50));
  EXPECT_EQ(1500, step_freeze_protect(1480, 50));
  EXPECT_EQ(1000, step_freeze_protect(1000, 0));
}

TEST_CASE(format_temp_celsius_renders_one_decimal) {
  char buffer[16];
  format_temp_celsius(2140, true, buffer, sizeof(buffer));
  EXPECT_STREQ("21.4℃", buffer);

  format_temp_celsius(2250, true, buffer, sizeof(buffer));
  EXPECT_STREQ("22.5℃", buffer);

  format_temp_celsius(2000, true, buffer, sizeof(buffer));
  EXPECT_STREQ("20.0℃", buffer);

  format_temp_celsius(0, true, buffer, sizeof(buffer));
  EXPECT_STREQ("0.0℃", buffer);

  format_temp_celsius(-550, true, buffer, sizeof(buffer));
  EXPECT_STREQ("-5.5℃", buffer);
}

TEST_CASE(format_temp_celsius_rounds_to_nearest_tenth) {
  char buffer[16];
  format_temp_celsius(2144, true, buffer, sizeof(buffer));
  EXPECT_STREQ("21.4℃", buffer);

  format_temp_celsius(2145, true, buffer, sizeof(buffer));
  EXPECT_STREQ("21.5℃", buffer);

  format_temp_celsius(2999, true, buffer, sizeof(buffer));
  EXPECT_STREQ("30.0℃", buffer);
}

TEST_CASE(format_temp_celsius_invalid_shows_placeholder) {
  char buffer[16];
  format_temp_celsius(2140, false, buffer, sizeof(buffer));
  EXPECT_STREQ("--.-℃", buffer);

  format_temp_celsius(-32768, false, buffer, sizeof(buffer));
  EXPECT_STREQ("--.-℃", buffer);
}

TEST_CASE(format_temp_celsius_handles_small_buffers) {
  char exact_fit[8];
  format_temp_celsius(2140, true, exact_fit, sizeof(exact_fit));
  EXPECT_STREQ("21.4℃", exact_fit);

  char truncated[4];
  format_temp_celsius(2140, true, truncated, sizeof(truncated));
  EXPECT_STREQ("21.", truncated);

  format_temp_celsius(2140, true, nullptr, 0);  // Must not crash.
}

TEST_CASE(view_model_defaults_match_plan) {
  UiViewModel vm;
  EXPECT_TRUE(vm.temp_valid);
  EXPECT_EQ(static_cast<int16_t>(2140), vm.current_temp_centi);
  EXPECT_EQ(static_cast<int16_t>(2250), vm.target_temp_centi);
  EXPECT_TRUE(vm.mode == UiMode::kTemperature);
  EXPECT_FALSE(vm.heating_active);
  EXPECT_EQ(static_cast<uint8_t>(15), vm.repeat_on_minutes);
  EXPECT_EQ(static_cast<int16_t>(1000), vm.freeze_protect_centi);
  EXPECT_TRUE(vm.source_severity == SourceSeverity::kNormal);
  EXPECT_STREQ("거실 센서 · 3분 전", vm.source_text);
  EXPECT_STREQ("21:15", vm.time_text);
}

TEST_CASE(ui_mode_values_match_matter_ordinals) {
  EXPECT_EQ(static_cast<uint8_t>(0), static_cast<uint8_t>(UiMode::kOff));
  EXPECT_EQ(static_cast<uint8_t>(1),
            static_cast<uint8_t>(UiMode::kTemperature));
  EXPECT_EQ(static_cast<uint8_t>(2), static_cast<uint8_t>(UiMode::kRepeat));
  EXPECT_EQ(static_cast<uint8_t>(0),
            static_cast<uint8_t>(SourceSeverity::kNormal));
  EXPECT_EQ(static_cast<uint8_t>(1),
            static_cast<uint8_t>(SourceSeverity::kWarning));
  EXPECT_EQ(static_cast<uint8_t>(2),
            static_cast<uint8_t>(SourceSeverity::kError));
}

THERMOSTAT_TEST_MAIN()
