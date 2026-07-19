#include "doctest.h"
#include <cstdint>
#include <string>

// ---------- Replicated types needed by flowLockoutActive ----------

static constexpr int RelayCount = 6;
static constexpr int SensorCount = 4;

enum class RelayRole : uint8_t {
  Disabled, SaltWaterGenerator, PoolHeater, AcidPump, Valve, Aux
};

enum class SensorType : uint8_t {
  Disabled, WaterLevelSwitch, FlowSwitch, TemperatureDs18b20
};

struct DebouncedDigitalInput {
  bool stable = false;
  bool lastRaw = false;
  uint32_t changedAtMs = 0;
};

struct SensorConfig {
  bool enabled = false;
  SensorType type = SensorType::Disabled;
  std::string name;
  bool lockoutEnabled = false;
};

// ---------- Global arrays (mutable for test setup) ----------

static SensorConfig sensorConfigs[SensorCount];
static DebouncedDigitalInput sensorInputs[SensorCount];

// ---------- Replicated from relays.cpp ----------

static bool flowLockoutActive() {
  for (uint8_t i = 0; i < SensorCount; i++) {
    if (
      sensorConfigs[i].enabled &&
      sensorConfigs[i].type == SensorType::FlowSwitch &&
      sensorConfigs[i].lockoutEnabled &&
      !sensorInputs[i].stable
    ) {
      return true;
    }
  }
  return false;
}

// =============================================================================
// Tests
// =============================================================================

static void resetState() {
  for (int i = 0; i < SensorCount; i++) {
    sensorConfigs[i] = SensorConfig{};
    sensorInputs[i] = DebouncedDigitalInput{};
  }
}

TEST_CASE("flowLockoutActive — no sensors configured") {
  resetState();
  CHECK(!flowLockoutActive());
}

TEST_CASE("flowLockoutActive — flow switch enabled, no lockout, flow present") {
  resetState();
  sensorConfigs[0].enabled = true;
  sensorConfigs[0].type = SensorType::FlowSwitch;
  sensorConfigs[0].lockoutEnabled = false;
  sensorInputs[0].stable = true;
  CHECK(!flowLockoutActive());
}

TEST_CASE("flowLockoutActive — flow switch enabled, lockout active, flow present") {
  resetState();
  sensorConfigs[1].enabled = true;
  sensorConfigs[1].type = SensorType::FlowSwitch;
  sensorConfigs[1].lockoutEnabled = true;
  sensorInputs[1].stable = true;   // flow is OK
  CHECK(!flowLockoutActive());
}

TEST_CASE("flowLockoutActive — flow switch enabled, lockout active, NO flow") {
  resetState();
  sensorConfigs[1].enabled = true;
  sensorConfigs[1].type = SensorType::FlowSwitch;
  sensorConfigs[1].lockoutEnabled = true;
  sensorInputs[1].stable = false;  // no flow → lockout
  CHECK(flowLockoutActive());
}

TEST_CASE("flowLockoutActive — multiple sensors, only one triggers") {
  resetState();
  // Port 0: water level (shouldn't matter)
  sensorConfigs[0].enabled = true;
  sensorConfigs[0].type = SensorType::WaterLevelSwitch;
  sensorInputs[0].stable = false;
  // Port 2: temperature (shouldn't matter)
  sensorConfigs[2].enabled = true;
  sensorConfigs[2].type = SensorType::TemperatureDs18b20;
  // Port 3: flow with lockout, flow OK
  sensorConfigs[3].enabled = true;
  sensorConfigs[3].type = SensorType::FlowSwitch;
  sensorConfigs[3].lockoutEnabled = true;
  sensorInputs[3].stable = true;

  CHECK(!flowLockoutActive());

  // Now break flow on port 3
  sensorInputs[3].stable = false;
  CHECK(flowLockoutActive());
}

TEST_CASE("flowLockoutActive — flow switch without lockout flag, no flow") {
  resetState();
  sensorConfigs[0].enabled = true;
  sensorConfigs[0].type = SensorType::FlowSwitch;
  sensorConfigs[0].lockoutEnabled = false;  // lockout OFF
  sensorInputs[0].stable = false;           // flow absent
  CHECK(!flowLockoutActive());              // lockout flag not set → ignored
}

TEST_CASE("flowLockoutActive — disabled sensor with lockout") {
  resetState();
  sensorConfigs[0].enabled = false;         // disabled
  sensorConfigs[0].type = SensorType::FlowSwitch;
  sensorConfigs[0].lockoutEnabled = true;
  sensorInputs[0].stable = false;
  CHECK(!flowLockoutActive());              // disabled → ignored
}
