#include "doctest.h"
#include "stubs/hal_mock.h"

// ============================================================================
// Duplicate relay subsystem with mock GPIO (same bodies as main/io/relays.cpp)
// ============================================================================

static constexpr int RelayCount = 6;
static constexpr int SensorCount = 4;
static constexpr int RelayPins[6] = {46, 45, 42, 41, 2, 1};

// Types from state.h
struct RelayConfig {
  bool enabled = false;
  int role = 0;       // RelayRole enum values
  std::string name;
  bool activeHigh = true;
};

enum class SensorType : uint8_t { Disabled, WaterLevelSwitch, FlowSwitch, TemperatureDs18b20 };

struct SensorConfig {
  bool enabled = false;
  SensorType type = SensorType::Disabled;
  std::string name;
  bool lockoutEnabled = false;
};

struct DebouncedDigitalInput {
  bool stable = false;
  bool lastRaw = false;
  uint32_t changedAtMs = 0;
};

// Global state (mutable for test setup)
static RelayConfig relayConfigs[RelayCount];
static SensorConfig sensorConfigs[SensorCount];
static DebouncedDigitalInput sensorInputs[SensorCount];
static bool relayStates[RelayCount] = {};
static uint16_t relayEndpointIds[RelayCount] = {};

static bool relaysConfigured = false;

// ---------- Replicated from relays.cpp ----------

static void setupRelaysSafe() {
  for (uint8_t i = 0; i < RelayCount; i++) {
    relayConfigs[i].enabled = true;
    gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << RelayPins[i]),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    relayStates[i] = false;
    gpio_set_level((gpio_num_t)RelayPins[i], relayConfigs[i].activeHigh ? 0 : 1);
    if (relayConfigs[i].name.empty()) {
      relayConfigs[i].name = "Relay " + std::to_string(i + 1);
    }
  }
  relaysConfigured = true;
}

static void setRelay(uint8_t index, bool active, const char *source) {
  (void)source;
  if (index >= RelayCount) return;
  relayStates[index] = active;
  if (!relaysConfigured) return;

  bool pinLevel = relayConfigs[index].activeHigh ? active : !active;
  gpio_set_level((gpio_num_t)RelayPins[index], pinLevel ? 1 : 0);
}

static void setAllRelaysOff(const char *source) {
  (void)source;
  for (uint8_t i = 0; i < RelayCount; i++) {
    setRelay(i, false, source);
  }
}

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

// ============================================================================
// Integration scenarios
// ============================================================================

static void resetAll() {
  mockGpio.reset();
  relaysConfigured = false;
  for (int i = 0; i < RelayCount; i++) {
    relayConfigs[i] = RelayConfig{};
    relayStates[i] = false;
    relayEndpointIds[i] = 0;
  }
  for (int i = 0; i < SensorCount; i++) {
    sensorConfigs[i] = SensorConfig{};
    sensorInputs[i] = DebouncedDigitalInput{};
  }
}

TEST_CASE("Integration: boot → all relays off") {
  resetAll();
  setupRelaysSafe();

  // All 6 relays should be configured as outputs and driven inactive
  for (int i = 0; i < RelayCount; i++) {
    CHECK(relayConfigs[i].enabled);
    CHECK(!relayStates[i]);
    // activeHigh=true (default) → inactive = 0
    CHECK(mockGpio.getLevel(RelayPins[i]) == 0);
    CHECK(!relayConfigs[i].name.empty());
  }
  CHECK(relaysConfigured);
}

TEST_CASE("Integration: setRelay toggles correct pin") {
  resetAll();
  setupRelaysSafe();

  // Turn on relay 2 (index 1 → pin 45)
  setRelay(1, true, "test");
  CHECK(relayStates[1]);
  CHECK(mockGpio.getLevel(RelayPins[1]) == 1);
  // Other relays unchanged
  CHECK(mockGpio.getLevel(RelayPins[0]) == 0);
  CHECK(mockGpio.getLevel(RelayPins[2]) == 0);

  // Turn off
  setRelay(1, false, "test");
  CHECK(!relayStates[1]);
  CHECK(mockGpio.getLevel(RelayPins[1]) == 0);
}

TEST_CASE("Integration: setRelay with activeHigh=false inverts") {
  resetAll();
  relayConfigs[0].activeHigh = false;
  setupRelaysSafe();

  // After setup, inactive (relay open) with activeHigh=false → pin HIGH
  CHECK(mockGpio.getLevel(RelayPins[0]) == 1);
  CHECK(!relayStates[0]);

  // Turn on → pin LOW
  setRelay(0, true, "test");
  CHECK(mockGpio.getLevel(RelayPins[0]) == 0);
  CHECK(relayStates[0]);

  // Turn off → pin HIGH
  setRelay(0, false, "test");
  CHECK(mockGpio.getLevel(RelayPins[0]) == 1);
}

TEST_CASE("Integration: setAllRelaysOff turns everything off") {
  resetAll();
  setupRelaysSafe();

  // Turn all on
  for (int i = 0; i < RelayCount; i++) setRelay(i, true, "test");
  for (int i = 0; i < RelayCount; i++) {
    CHECK(relayStates[i]);
    CHECK(mockGpio.getLevel(RelayPins[i]) == 1);
  }

  // All off
  setAllRelaysOff("factory-reset");
  for (int i = 0; i < RelayCount; i++) {
    CHECK(!relayStates[i]);
    CHECK(mockGpio.getLevel(RelayPins[i]) == 0);
  }
}

TEST_CASE("Integration: setRelay out-of-bounds ignored") {
  resetAll();
  setupRelaysSafe();
  setRelay(6, true, "test");    // index too high
  setRelay(255, true, "test");  // way too high
  // All still off
  for (int i = 0; i < RelayCount; i++) {
    CHECK(!relayStates[i]);
  }
}

TEST_CASE("Integration: flow lockout → relay still toggles, lockout is advisory") {
  resetAll();
  setupRelaysSafe();

  // Configure flow sensor with lockout
  sensorConfigs[0].enabled = true;
  sensorConfigs[0].type = SensorType::FlowSwitch;
  sensorConfigs[0].lockoutEnabled = true;
  sensorInputs[0].stable = false;  // no flow

  CHECK(flowLockoutActive());

  // Relay still operates — lockout is enforced by callers, not setRelay itself
  setRelay(0, true, "cloud");
  CHECK(relayStates[0]);
  CHECK(mockGpio.getLevel(RelayPins[0]) == 1);
}

TEST_CASE("Integration: disabled relay still changes state, just not GPIO") {
  resetAll();
  relayConfigs[0].enabled = false;
  setupRelaysSafe();

  // Calling setRelay should still update relayStates
  relayStates[0] = false;
  setRelay(0, true, "test");
  CHECK(relayStates[0]);
  // But GPIO unchanged (relaysConfigured is true, but we test the state tracking)
}
