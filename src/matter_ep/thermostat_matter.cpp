#include "thermostat_matter.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>

#include <algorithm>
#include <cmath>

namespace thermostat::matter {
namespace {

// ThermostatRunningState is a bitmap16; bit 0 is the Heat state (Matter spec).
constexpr uint16_t kRunningStateHeatingBit = 0x0001;
constexpr uint16_t kRunningStateIdle = 0x0000;

int16_t clamp_setpoint_centi(int16_t setpoint_centi) {
  if (setpoint_centi < kMinHeatSetpointCenti) {
    return kMinHeatSetpointCenti;
  }
  if (setpoint_centi > kMaxHeatSetpointCenti) {
    return kMaxHeatSetpointCenti;
  }
  return setpoint_centi;
}

// Converts a Celsius value coming from a Matter controller into centi-degC.
// Returns false for non-finite or out-of-range input instead of wrapping.
bool celsius_to_centi(double celsius, int16_t* out_centi) {
  if (out_centi == nullptr || !std::isfinite(celsius)) {
    return false;
  }
  const double centi = celsius * 100.0;
  if (centi <= static_cast<double>(INT16_MIN) || centi > static_cast<double>(INT16_MAX)) {
    return false;
  }
  *out_centi = static_cast<int16_t>(std::lround(centi));
  return true;
}

}  // namespace

bool ThermostatEndpoint::begin(const MatterThermostatCallbacks& callbacks,
                               bool enable_aux_contact) {
  callbacks_ = callbacks;
  aux_contact_enabled_ = false;
  running_state_supported_ = false;
  matter_begun_ = false;
  heating_active_ = false;

  if (!thermostat_.begin(MatterThermostat::THERMOSTAT_SEQ_OP_HEATING,
                         MatterThermostat::THERMOSTAT_AUTO_MODE_DISABLED)) {
    return false;
  }

  // Controller -> firmware callbacks. These run on the Matter task once the
  // stack is started, so register them before Matter.begin().
  thermostat_.onChangeMode([this](MatterThermostat::ThermostatMode_t mode) -> bool {
    if (!callbacks_.on_mode_change) {
      return true;
    }
    return callbacks_.on_mode_change(mode == MatterThermostat::THERMOSTAT_MODE_HEAT);
  });
  thermostat_.onChangeHeatingSetpoint([this](double setpoint_celsius) -> bool {
    if (!callbacks_.on_setpoint_change) {
      return true;
    }
    int16_t setpoint_centi = 0;
    if (!celsius_to_centi(setpoint_celsius, &setpoint_centi)) {
      return false;
    }
    return callbacks_.on_setpoint_change(setpoint_centi);
  });

  // MatterThermostat has no ThermostatRunningState setter; add the standard
  // attribute to the Thermostat cluster when the server did not create it.
  const uint16_t ep_id = thermostat_.getEndPointId();
  esp_matter::endpoint_t* endpoint =
      esp_matter::endpoint::get(esp_matter::node::get(), ep_id);
  esp_matter::cluster_t* thermostat_cluster =
      endpoint == nullptr
          ? nullptr
          : esp_matter::cluster::get(endpoint, chip::app::Clusters::Thermostat::Id);
  if (thermostat_cluster != nullptr) {
    esp_matter::attribute_t* running_state = esp_matter::attribute::get(
        thermostat_cluster,
        chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id);
    if (running_state == nullptr) {
      running_state = esp_matter::cluster::thermostat::attribute::
          create_thermostat_running_state(thermostat_cluster, kRunningStateIdle);
    }
    running_state_supported_ = (running_state != nullptr);
  }

  if (enable_aux_contact) {
    // Auxiliary endpoint: contact = true means "heating requested".
    aux_contact_enabled_ = aux_heat_sensor_.begin();
  }
  return true;
}

void ThermostatEndpoint::after_matter_begin(int16_t initial_temp_centi,
                                            int16_t initial_setpoint_centi,
                                            bool heat_mode) {
  matter_begun_ = true;
  heating_active_ = false;
  set_local_temperature_centi(initial_temp_centi);
  set_heating_setpoint_centi(initial_setpoint_centi);
  set_heat_mode(heat_mode);
  // Auxiliary BooleanSensor / RunningState setters must only run after
  // Matter.begin(); publish idle/open once the stack is up.
  set_heating_active(false);
}

bool ThermostatEndpoint::set_local_temperature_centi(int16_t temp_centi) {
  if (temp_centi == kInvalidTempCenti) {
    return false;
  }
  return thermostat_.setLocalTemperature(static_cast<double>(temp_centi) / 100.0);
}

bool ThermostatEndpoint::set_heating_setpoint_centi(int16_t setpoint_centi) {
  const int16_t clamped = clamp_setpoint_centi(setpoint_centi);
  return thermostat_.setHeatingSetpoint(static_cast<double>(clamped) / 100.0);
}

bool ThermostatEndpoint::set_heat_mode(bool heat_mode) {
  return thermostat_.setMode(heat_mode ? MatterThermostat::THERMOSTAT_MODE_HEAT
                                       : MatterThermostat::THERMOSTAT_MODE_OFF);
}

bool ThermostatEndpoint::set_heating_active(bool heating) {
  heating_active_ = heating;
  if (!matter_begun_) {
    // RunningState / ContactSensor setters must not touch the Matter stack
    // before Matter.begin() has completed.
    return false;
  }

  bool ok = true;
  if (running_state_supported_) {
    // esp_matter_bitmap16() is declared at global scope in
    // esp_matter_attribute_utils.h (before the esp_matter namespace opens).
    esp_matter_attr_val_t value = esp_matter_bitmap16(
        heating ? kRunningStateHeatingBit : kRunningStateIdle);
    const esp_err_t err = esp_matter::attribute::update(
        thermostat_.getEndPointId(), chip::app::Clusters::Thermostat::Id,
        chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id, &value);
    ok = (err == ESP_OK);
  }
  if (aux_contact_enabled_) {
    const bool contact_ok = aux_heat_sensor_.setContact(heating);
    ok = ok && contact_ok;
  }
  return ok;
}

uint16_t ThermostatEndpoint::endpoint_id() const {
  // MatterEndPoint::getEndPointId() is not const-qualified but only reads a
  // member, so casting away constness here is safe.
  return const_cast<MatterThermostat&>(thermostat_).getEndPointId();
}

}  // namespace thermostat::matter
