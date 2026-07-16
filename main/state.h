#pragma once

#include <stdint.h>
#include <string>

#include <esp_matter.h>
#include <esp_matter_endpoint.h>

#include "board_identity.h"
#include "board_pins.h"

// ---------- constants ----------

static constexpr const char *FIRMWARE_VERSION = "0.2.0";
static constexpr const char *DEVICE_ID = BOARD_DEVICE_ID;
static constexpr uint32_t STATUS_POLL_MS = 5000;
static constexpr uint32_t PUMP_KEEPALIVE_MS = 5000;
static constexpr uint32_t SENSOR_SAMPLE_MS = 100;
static constexpr uint32_t TEMPERATURE_SAMPLE_MS = 30000;
static constexpr uint32_t DIGITAL_DEBOUNCE_MS = 250;
static constexpr uint32_t COMMISSIONING_LOG_MS = 5 * 60 * 1000UL;
static constexpr uint32_t FACTORY_RESET_HOLD_MS = 10000;
static constexpr uint16_t MIN_RPM = 450;
static constexpr uint16_t DEFAULT_RPM = 1800;
static constexpr uint16_t DEFAULT_MAX_RPM = 3450;
static constexpr uint8_t MIN_GPM = 20;
static constexpr uint8_t DEFAULT_GPM = 40;
static constexpr uint8_t MAX_GPM = 140;

using namespace chip::app::Clusters;

// ---------- enums ----------

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

enum class UserStatus : uint8_t {
  Booting,
  Commissioning,
  Online,
  FlowLockout
};

// ---------- config structs ----------

struct RelayConfig {
  bool enabled = false;
  RelayRole role = RelayRole::Disabled;
  std::string name;
  bool activeHigh = true;
};

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

  void begin(int pin);
  void update(int pin);
};

struct PupStatus {
  uint16_t rpm = 0;
  uint16_t watts = 0;
  uint8_t flow = 0;
  uint8_t mode = 0;
  uint8_t ppc = 0;
  uint8_t reserved = 0;
  uint8_t command = 0;
  uint8_t driveState = 0;
  uint8_t pumpError = 0;
  uint16_t statusWord = 0;
  uint8_t clockHour = 0;
  uint8_t clockMinute = 0;
  bool valid = false;
};

// ---------- Pentair pump ----------

// Forward-declared; full definition in pentair_pump.h
class PentairPump;

// ---------- globals ----------

extern RelayConfig relayConfigs[BoardPins::RelayCount];
extern SensorConfig sensorConfigs[BoardPins::SensorCount];
extern DebouncedDigitalInput sensorInputs[BoardPins::SensorCount];
extern float temperatureC[BoardPins::SensorCount];
extern bool temperatureFault[BoardPins::SensorCount];
extern bool relayStates[BoardPins::RelayCount];
extern uint16_t relayEndpointIds[BoardPins::RelayCount];
extern uint16_t pumpEndpointId;
extern uint16_t waterLevelSensorEndpointId;
extern uint16_t flowSensorEndpointId;
extern uint16_t temperatureSensorEndpointId;

extern PupStatus pumpStatus;
extern uint32_t lastSensorSampleMs;
extern uint32_t lastTemperatureSampleMs;
extern uint32_t lastPumpStatusMs;
extern uint32_t lastCommissioningLogMs;
extern uint32_t lastLedUpdateMs;
extern uint32_t bootButtonPressedAtMs;
extern uint32_t lastResetWarningMs;
extern bool printedCommissioningInfo;
extern bool factoryResetTriggered;
extern uint16_t maxRpm;
extern char deviceId[64];
// ---------- helpers ----------

void clearNvsConfig();
uint16_t percentToRpm(uint8_t percent);
uint8_t rpmToPercent(uint16_t rpm);
uint8_t percentToLevel(uint8_t percent);
uint8_t levelToPercent(uint8_t level);
