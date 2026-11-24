#include "core_version.h"

namespace thermostat::core {

const char* version_string() { return "0.1.0"; }

int clamp_setpoint_centi(int setpoint_centi, int min_centi, int max_centi) {
  if (setpoint_centi < min_centi) {
    return min_centi;
  }
  if (setpoint_centi > max_centi) {
    return max_centi;
  }
  return setpoint_centi;
}

}  // namespace thermostat::core
