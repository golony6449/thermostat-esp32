#pragma once

// Pure C++17 view model for the Waveshare 4.3B thermostat UI.
//
// This header deliberately has no Arduino, LVGL, or Wi-Fi dependencies so the
// value stepping and formatting rules can be unit-tested on a PC host. The
// LVGL widgets in thermostat_ui.* only consume the structures defined here.

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace thermostat::ui {

enum class UiMode : uint8_t { kOff = 0, kTemperature = 1, kRepeat = 2 };

// Severity of the temperature-source badge. The UI paints normal sources in
// light gray, 5-minute-old sources in amber, and fallback/error sources in red.
enum class SourceSeverity : uint8_t { kNormal = 0, kWarning = 1, kError = 2 };

constexpr int16_t kMinTargetTempCenti = 500;   // 5.0 degC
constexpr int16_t kMaxTargetTempCenti = 3000;  // 30.0 degC
constexpr int16_t kMinFreezeProtectCenti = 500;
constexpr int16_t kMaxFreezeProtectCenti = 1500;
constexpr uint8_t kMinRepeatMinutes = 0;
constexpr uint8_t kMaxRepeatMinutes = 60;

struct UiViewModel {
  bool temp_valid = true;
  int16_t current_temp_centi = 2140;  // 21.40 degC
  int16_t target_temp_centi = 2250;   // 22.50 degC
  UiMode mode = UiMode::kTemperature;
  bool heating_active = false;
  uint8_t repeat_on_minutes = 15;      // 0..60
  int16_t freeze_protect_centi = 1000;  // 5.0..15.0 degC
  SourceSeverity source_severity = SourceSeverity::kNormal;
  char source_text[96] = "거실 센서 · 3분 전";
  char time_text[16] = "21:15";
};

// Widget events are forwarded to the firmware with plain function pointers so
// the UI stays independent from std::function and from the control layer.
struct UiCallbacks {
  void (*on_target_temp_change)(int16_t new_target_centi, void* ctx) = nullptr;
  void (*on_mode_change)(UiMode new_mode, void* ctx) = nullptr;
  void (*on_repeat_minutes_change)(uint8_t new_minutes, void* ctx) = nullptr;
  void (*on_freeze_protect_change)(int16_t new_freeze_centi, void* ctx) = nullptr;
  void* ctx = nullptr;
};

inline int16_t clamp_centi(int16_t value, int16_t low, int16_t high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

inline uint8_t clamp_minutes(uint8_t value, uint8_t low, uint8_t high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

// The + / - buttons pass +/-50 (0.5 degC) and the helper clamps to the
// thermostat target range. Values are kept as centi-degC so the display can
// still show 0.1 degC resolution.
inline int16_t step_target_temp(int16_t current_centi, int16_t delta_centi) {
  const int32_t next =
      static_cast<int32_t>(current_centi) + static_cast<int32_t>(delta_centi);
  if (next < kMinTargetTempCenti) {
    return kMinTargetTempCenti;
  }
  if (next > kMaxTargetTempCenti) {
    return kMaxTargetTempCenti;
  }
  return static_cast<int16_t>(next);
}

// The +/- buttons pass +/-5 minutes; the repeat cycle is 60 minutes long.
inline uint8_t step_repeat_minutes(uint8_t current_min, int delta_min) {
  const int32_t next =
      static_cast<int32_t>(current_min) + static_cast<int32_t>(delta_min);
  if (next < kMinRepeatMinutes) {
    return kMinRepeatMinutes;
  }
  if (next > kMaxRepeatMinutes) {
    return kMaxRepeatMinutes;
  }
  return static_cast<uint8_t>(next);
}

// The +/- buttons pass +/-50 (0.5 degC) and the helper clamps to 5.0..15.0.
inline int16_t step_freeze_protect(int16_t current_centi, int16_t delta_centi) {
  const int32_t next =
      static_cast<int32_t>(current_centi) + static_cast<int32_t>(delta_centi);
  if (next < kMinFreezeProtectCenti) {
    return kMinFreezeProtectCenti;
  }
  if (next > kMaxFreezeProtectCenti) {
    return kMaxFreezeProtectCenti;
  }
  return static_cast<int16_t>(next);
}

// Formats centi-degC as "21.4℃" (UTF-8). Invalid readings render as "--.-℃".
inline void format_temp_celsius(int16_t temp_centi, bool valid, char* buf,
                                size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return;
  }
  if (!valid) {
    std::snprintf(buf, buf_size, "--.-℃");
    return;
  }

  const bool negative = temp_centi < 0;
  int32_t magnitude = negative ? -static_cast<int32_t>(temp_centi)
                               : static_cast<int32_t>(temp_centi);
  // Round to the nearest 0.1 degC (half away from zero).
  const int32_t tenths = (magnitude + 5) / 10;
  const int32_t whole = tenths / 10;
  const int32_t fraction = tenths % 10;
  std::snprintf(buf, buf_size, "%s%ld.%ld℃", negative ? "-" : "",
                static_cast<long>(whole), static_cast<long>(fraction));
}

// Caption shown above the main value in the bottom control panel.  The main
// +/- buttons take on a different meaning per mode, so the caption must
// change with the mode instead of always saying "목표 온도".
inline const char* primary_caption_for_mode(UiMode mode) {
  switch (mode) {
    case UiMode::kTemperature:
      return "목표 온도";
    case UiMode::kRepeat:
      return "반복 시간";
    case UiMode::kOff:
    default:
      return "꺼짐";
  }
}

// Main value shown between the large +/- buttons: the target temperature in
// temperature mode, the repeat on-minutes in repeat mode, and a dash when the
// thermostat is off (nothing is adjustable).
inline void format_primary_value(const UiViewModel& vm, char* buf,
                                 size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return;
  }
  switch (vm.mode) {
    case UiMode::kTemperature:
      format_temp_celsius(vm.target_temp_centi, true, buf, buf_size);
      return;
    case UiMode::kRepeat:
      std::snprintf(buf, buf_size, "%u분",
                    static_cast<unsigned>(vm.repeat_on_minutes));
      return;
    case UiMode::kOff:
    default:
      std::snprintf(buf, buf_size, "-");
      return;
  }
}

// Off mode locks the main +/- buttons; temperature and repeat modes allow
// editing.  The LVGL layer uses this for the visual/event lock and the action
// handler checks it again so a stale event can never change a value.
inline bool can_adjust_primary(UiMode mode) { return mode != UiMode::kOff; }

}  // namespace thermostat::ui
