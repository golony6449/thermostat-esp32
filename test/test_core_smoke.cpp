// LINK: src/core/*.cpp
#include "core_version.h"
#include "test_harness.h"

TEST_CASE(version_string_is_expected) {
  EXPECT_TRUE(thermostat::core::version_string() != nullptr);
  EXPECT_STREQ("0.1.0", thermostat::core::version_string());
}

TEST_CASE(clamp_keeps_setpoints_inside_range) {
  EXPECT_EQ(500, thermostat::core::clamp_setpoint_centi(500));
  EXPECT_EQ(1000, thermostat::core::clamp_setpoint_centi(1000));
  EXPECT_EQ(2000, thermostat::core::clamp_setpoint_centi(2000));
  EXPECT_EQ(2500, thermostat::core::clamp_setpoint_centi(2500));
  EXPECT_EQ(3000, thermostat::core::clamp_setpoint_centi(3000));
}

TEST_CASE(clamp_applies_default_lower_and_upper_bounds) {
  EXPECT_EQ(500, thermostat::core::clamp_setpoint_centi(0));
  EXPECT_EQ(500, thermostat::core::clamp_setpoint_centi(499));
  EXPECT_EQ(3000, thermostat::core::clamp_setpoint_centi(3001));
  EXPECT_EQ(3000, thermostat::core::clamp_setpoint_centi(100000));
}

TEST_CASE(clamp_accepts_custom_bounds) {
  EXPECT_EQ(1000, thermostat::core::clamp_setpoint_centi(999, 1000, 2000));
  EXPECT_EQ(1500, thermostat::core::clamp_setpoint_centi(1500, 1000, 2000));
  EXPECT_EQ(2000, thermostat::core::clamp_setpoint_centi(2500, 1000, 2000));
  EXPECT_EQ(-500, thermostat::core::clamp_setpoint_centi(-500, -1000, 0));
}

TEST_CASE(default_minimum_temperature_is_ten_degrees) {
  EXPECT_EQ(1000, thermostat::core::kDefaultMinTempCenti);
  EXPECT_NEAR(10.0, thermostat::core::kDefaultMinTempCenti / 100.0, 0.0001);
}

THERMOSTAT_TEST_MAIN()
