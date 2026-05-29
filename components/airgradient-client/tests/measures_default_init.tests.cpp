/**
 * AirGradient — measures default-initialisation contract tests
 *
 * Asserts that value-initialised measure aggregates have every field at the
 * matching `MeasuresInvalid::*` sentinel, so the serializer in
 * payload_serializer.cpp omits every measure field unless the caller wrote
 * a real value into it.  See the "Measures Initialisation Contract" notes
 * (now removed) in components/airgradient-client/README.md.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "measures_types.h"

TEST_CASE("Default-constructed CO2Data fails is_valid", "[measures_default_init]") {
  CO2Data d{};
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed TempHumData fails every validation", "[measures_default_init]") {
  TempHumData d{};
  REQUIRE_FALSE(d.is_temp_valid());
  REQUIRE_FALSE(d.is_hum_valid());
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed PMData fails every validation", "[measures_default_init]") {
  PMData d{};
  REQUIRE_FALSE(d.is_pm_01_valid());
  REQUIRE_FALSE(d.is_pm_25_valid());
  REQUIRE_FALSE(d.is_pm_10_valid());
  REQUIRE_FALSE(d.is_pm_01_sp_valid());
  REQUIRE_FALSE(d.is_pm_25_sp_valid());
  REQUIRE_FALSE(d.is_pm_10_sp_valid());
  REQUIRE_FALSE(d.is_pm_03_pc_valid());
  REQUIRE_FALSE(d.is_pm_05_pc_valid());
  REQUIRE_FALSE(d.is_pm_01_pc_valid());
  REQUIRE_FALSE(d.is_pm_25_pc_valid());
  REQUIRE_FALSE(d.is_pm_5_pc_valid());
  REQUIRE_FALSE(d.is_pm_10_pc_valid());
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed TVOCNOxData fails every validation", "[measures_default_init]") {
  TVOCNOxData d{};
  REQUIRE_FALSE(d.is_tvoc_index_valid());
  REQUIRE_FALSE(d.is_tvoc_raw_valid());
  REQUIRE_FALSE(d.is_nox_index_valid());
  REQUIRE_FALSE(d.is_nox_raw_valid());
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed O3No2Data fails every validation", "[measures_default_init]") {
  O3No2Data d{};
  REQUIRE_FALSE(d.is_o3_working_valid());
  REQUIRE_FALSE(d.is_o3_auxiliary_valid());
  REQUIRE_FALSE(d.is_no2_working_valid());
  REQUIRE_FALSE(d.is_no2_auxiliary_valid());
  REQUIRE_FALSE(d.is_afe_temp_valid());
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed MeasuresPower fails every validation", "[measures_default_init]") {
  MeasuresPower d{};
  REQUIRE_FALSE(d.is_battery_voltage_valid());
  REQUIRE_FALSE(d.is_charging_voltage_valid());
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed PressureData fails every validation", "[measures_default_init]") {
  PressureData d{};
  REQUIRE_FALSE(d.is_pressure_valid());
  REQUIRE_FALSE(d.is_altitude_valid());
  REQUIRE_FALSE(d.is_valid());
}

TEST_CASE("Default-constructed Measures{} aggregates fail every substruct validation",
          "[measures_default_init]") {
  Measures m{};
  REQUIRE_FALSE(m.temp_hum_a.is_valid());
  REQUIRE_FALSE(m.temp_hum_b.is_valid());
  REQUIRE_FALSE(m.pm_a.is_valid());
  REQUIRE_FALSE(m.pm_b.is_valid());
  REQUIRE_FALSE(m.co2.is_valid());
  REQUIRE_FALSE(m.tvoc_nox.is_valid());
  REQUIRE_FALSE(m.power.is_valid());
  REQUIRE_FALSE(m.electrode.is_valid());
  REQUIRE_FALSE(m.pressure.is_valid());
}

TEST_CASE("Default-constructed MeasuresBasic{} fails every substruct validation",
          "[measures_default_init]") {
  MeasuresBasic m{};
  REQUIRE_FALSE(m.temp_hum_a.is_valid());
  REQUIRE_FALSE(m.pm_a.is_valid());
  REQUIRE_FALSE(m.co2.is_valid());
  REQUIRE_FALSE(m.tvoc_nox.is_valid());
}

TEST_CASE("Default-constructed MeasuresAGo{} fails every substruct validation",
          "[measures_default_init]") {
  MeasuresAGo m{};
  REQUIRE_FALSE(m.temp_hum_a.is_valid());
  REQUIRE_FALSE(m.pm_a.is_valid());
  REQUIRE_FALSE(m.co2.is_valid());
  REQUIRE_FALSE(m.tvoc_nox.is_valid());
  REQUIRE_FALSE(m.power.is_valid());
  REQUIRE_FALSE(m.pressure.is_valid());
}
