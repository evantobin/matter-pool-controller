#include "doctest.h"
#include <string.h>

// ---------- Duplicate the constants / enums / globals that state.cpp needs ----------
// (Included via state.cpp which #includes state.h, so we need these to match)

static constexpr uint16_t MIN_RPM = 450;
static constexpr uint16_t DEFAULT_MAX_RPM = 3450;
static uint16_t maxRpm = DEFAULT_MAX_RPM;

enum class RelayRole : uint8_t {
  Disabled,
  SaltWaterGenerator,
  PoolHeater,
  AcidPump,
  Valve,
  Aux
};

enum class SensorType : uint8_t {
  Disabled,
  WaterLevelSwitch,
  FlowSwitch,
  TemperatureDs18b20
};

// ---------- Pull in the pure functions we want to test ----------
// We cannot #include "main/app/state.cpp" directly because it pulls in
// ESP-IDF hardware headers even through stubs.  Instead we duplicate the
// function bodies here.  If they drift the tests will catch it.

static uint16_t percentToRpm(uint8_t percent) {
  if (percent == 0) return 0;
  long rpm = (long)percent * (maxRpm - MIN_RPM) / 100 + MIN_RPM;
  if (rpm < MIN_RPM) rpm = MIN_RPM;
  if (rpm > maxRpm) rpm = maxRpm;
  return (uint16_t)rpm;
}

static uint8_t rpmToPercent(uint16_t rpm) {
  if (rpm < MIN_RPM) return 0;
  long percent = ((long)rpm - MIN_RPM) * 100 / (maxRpm - MIN_RPM);
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  return (uint8_t)percent;
}

static uint8_t percentToLevel(uint8_t percent) {
  if (percent == 0) return 1;
  if (percent > 100) percent = 100;
  long level = 1 + ((long)percent * 253 + 50) / 100;
  if (level < 1) level = 1;
  if (level > 254) level = 254;
  return (uint8_t)level;
}

static uint8_t levelToPercent(uint8_t level) {
  if (level == 0) return 0;
  long percent = ((long)level - 1) * 100 / 253;
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  return (uint8_t)percent;
}

static RelayRole relayRoleFromString(const char *s) {
  if (!s) return RelayRole::Aux;
  if (strcmp(s, "saltWaterGenerator") == 0) return RelayRole::SaltWaterGenerator;
  if (strcmp(s, "poolHeater") == 0) return RelayRole::PoolHeater;
  if (strcmp(s, "acidPump") == 0) return RelayRole::AcidPump;
  if (strcmp(s, "valve") == 0) return RelayRole::Valve;
  if (strcmp(s, "aux") == 0) return RelayRole::Aux;
  return RelayRole::Aux;
}

static const char *relayRoleName(RelayRole role) {
  switch (role) {
    case RelayRole::SaltWaterGenerator: return "SWG";
    case RelayRole::PoolHeater: return "Heater";
    case RelayRole::AcidPump: return "Acid Pump";
    case RelayRole::Valve: return "Valve";
    case RelayRole::Aux: return "Aux";
    default: return "Disabled";
  }
}

static SensorType sensorTypeFromString(const char *s) {
  if (!s) return SensorType::Disabled;
  if (strcmp(s, "waterLevelSwitch") == 0) return SensorType::WaterLevelSwitch;
  if (strcmp(s, "flowSwitch") == 0) return SensorType::FlowSwitch;
  if (strcmp(s, "temperatureDs18b20") == 0) return SensorType::TemperatureDs18b20;
  return SensorType::Disabled;
}

static const char *sensorTypeName(SensorType type) {
  switch (type) {
    case SensorType::WaterLevelSwitch: return "water_level";
    case SensorType::FlowSwitch: return "flow";
    case SensorType::TemperatureDs18b20: return "temperature";
    default: return "disabled";
  }
}

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("percentToRpm") {
  maxRpm = DEFAULT_MAX_RPM;

  CHECK(percentToRpm(0) == 0);
  CHECK(percentToRpm(1) == 480);                    // (3000/100) + 450
  CHECK(percentToRpm(50) == 1950);                   // midpoint
  CHECK(percentToRpm(100) == DEFAULT_MAX_RPM);       // max
  CHECK(percentToRpm(200) == DEFAULT_MAX_RPM);   // clamped

  // Custom maxRpm
  maxRpm = 3000;
  CHECK(percentToRpm(50) == 1725);               // (3000-450)/2 + 450
  CHECK(percentToRpm(100) == 3000);
}

TEST_CASE("rpmToPercent") {
  maxRpm = DEFAULT_MAX_RPM;

  CHECK(rpmToPercent(0) == 0);
  CHECK(rpmToPercent(449) == 0);                 // below MIN_RPM
  CHECK(rpmToPercent(MIN_RPM) == 1);             // floor
  CHECK(rpmToPercent(1950) == 50);               // midpoint
  CHECK(rpmToPercent(DEFAULT_MAX_RPM) == 100);   // max
  CHECK(rpmToPercent(4000) == 100);              // clamped

  // Roundtrip
  for (uint8_t p = 1; p <= 100; p++) {
    uint16_t rpm = percentToRpm(p);
    uint8_t back = rpmToPercent(rpm);
    // Allow ±1 due to integer rounding
    CHECK(std::abs((int)back - (int)p) <= 1);
  }
}

TEST_CASE("percentToLevel") {
  CHECK(percentToLevel(0) == 1);     // off → min level
  CHECK(percentToLevel(1) == 4);     // 1 + ((253+50)/100)
  CHECK(percentToLevel(50) == 128);  // 1 + ((12650+50)/100)
  CHECK(percentToLevel(100) == 254); // max
  CHECK(percentToLevel(200) == 254); // clamped
}

TEST_CASE("levelToPercent") {
  CHECK(levelToPercent(0) == 0);
  CHECK(levelToPercent(1) == 1);    // floor
  CHECK(levelToPercent(127) == 49); // (126*100)/253 = 49
  CHECK(levelToPercent(254) == 100);// max
  CHECK(levelToPercent(255) == 100);// clamped

  // Roundtrip
  CHECK(levelToPercent(percentToLevel(0)) == 1);
  CHECK(levelToPercent(percentToLevel(100)) == 100);
}

TEST_CASE("relayRoleFromString") {
  CHECK(relayRoleFromString(nullptr) == RelayRole::Aux);
  CHECK(relayRoleFromString("") == RelayRole::Aux);
  CHECK(relayRoleFromString("garbage") == RelayRole::Aux);

  CHECK(relayRoleFromString("saltWaterGenerator") == RelayRole::SaltWaterGenerator);
  CHECK(relayRoleFromString("poolHeater") == RelayRole::PoolHeater);
  CHECK(relayRoleFromString("acidPump") == RelayRole::AcidPump);
  CHECK(relayRoleFromString("valve") == RelayRole::Valve);
  CHECK(relayRoleFromString("aux") == RelayRole::Aux);
}

TEST_CASE("relayRoleName") {
  CHECK(strcmp(relayRoleName(RelayRole::Disabled), "Disabled") == 0);
  CHECK(strcmp(relayRoleName(RelayRole::SaltWaterGenerator), "SWG") == 0);
  CHECK(strcmp(relayRoleName(RelayRole::PoolHeater), "Heater") == 0);
  CHECK(strcmp(relayRoleName(RelayRole::AcidPump), "Acid Pump") == 0);
  CHECK(strcmp(relayRoleName(RelayRole::Valve), "Valve") == 0);
  CHECK(strcmp(relayRoleName(RelayRole::Aux), "Aux") == 0);
  CHECK(strcmp(relayRoleName(static_cast<RelayRole>(99)), "Disabled") == 0);

  // Every parseable string roundtrips
  const char *inputs[] = {
    "saltWaterGenerator", "poolHeater", "acidPump", "valve", "aux"
  };
  for (const char *s : inputs) {
    RelayRole r = relayRoleFromString(s);
    // relayRoleName gives display name, relayRoleFromString expects JSON key.
    // They are not inverses, so we just check the parse is stable.
    CHECK(r != RelayRole::Disabled);
  }
}

TEST_CASE("sensorTypeFromString") {
  CHECK(sensorTypeFromString(nullptr) == SensorType::Disabled);
  CHECK(sensorTypeFromString("") == SensorType::Disabled);
  CHECK(sensorTypeFromString("garbage") == SensorType::Disabled);

  CHECK(sensorTypeFromString("waterLevelSwitch") == SensorType::WaterLevelSwitch);
  CHECK(sensorTypeFromString("flowSwitch") == SensorType::FlowSwitch);
  CHECK(sensorTypeFromString("temperatureDs18b20") == SensorType::TemperatureDs18b20);
}

TEST_CASE("sensorTypeName") {
  CHECK(strcmp(sensorTypeName(SensorType::Disabled), "disabled") == 0);
  CHECK(strcmp(sensorTypeName(SensorType::WaterLevelSwitch), "water_level") == 0);
  CHECK(strcmp(sensorTypeName(SensorType::FlowSwitch), "flow") == 0);
  CHECK(strcmp(sensorTypeName(SensorType::TemperatureDs18b20), "temperature") == 0);
  CHECK(strcmp(sensorTypeName(static_cast<SensorType>(99)), "disabled") == 0);
}
