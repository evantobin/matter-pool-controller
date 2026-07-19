// Shared runtime state — definitions for all globals declared in state.h.
//
// This file holds the single-instance mutable state that every subsystem
// accesses. Configuration belongs in board/; this file owns only runtime
// state shared by the hardware I/O and Matter endpoint modules.
//
// The debounce logic lives here because it's tightly coupled to the timing
// constants defined in state.h. The NVS erase helper and the unit-conversion
// functions are here because they operate on state.h types and constants.
//
// String converters (relayRoleFromString, etc.) are used by the cloud config
// parser to translate JSON string values into enum constants. They're kept
// here rather than in the cloud module to avoid a circular include when
// other modules (like the schedule) need to reference relay roles.

#include "app/state.h"

#include <cmath>
#include <string.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <nvs_flash.h>

// ---- global definitions ----

// Config arrays are default-initialized with the struct defaults (disabled,
// no name). The sensor setup function in sensors.cpp overwrites these with
// values from board_sensor_config.h. The cloud shadow delta can further
// override them at runtime.
RelayConfig relayConfigs[BoardPins::RelayCount];
SensorConfig sensorConfigs[BoardPins::SensorCount];
DebouncedDigitalInput sensorInputs[BoardPins::SensorCount];

// Initialize all temperatures to NaN so the first sample always triggers
// a DS18B20 conversion request. Fault flags start false — no sensor is
// faulty until proven otherwise.
float temperatureC[BoardPins::SensorCount] = {NAN, NAN, NAN, NAN};
bool temperatureFault[BoardPins::SensorCount] = {false, false, false, false};

// All relays start off at boot for safety — a power-cycling pool pump
// should not restart pumps, heaters, or chemical feeders without explicit
// command.
bool relayStates[BoardPins::RelayCount] = {false, false, false, false, false, false};

// Endpoint IDs are zero until Matter endpoint creation assigns them.
// Callers check for zero to avoid dereferencing invalid endpoint IDs.
uint16_t relayEndpointIds[BoardPins::RelayCount] = {0};
uint16_t pumpEndpointId = 0;
uint16_t waterLevelSensorEndpointId = 0;
uint16_t flowSensorEndpointId = 0;
uint16_t temperatureSensorEndpointId = 0;

// Pump status starts invalid. The first valid status frame marks it valid
// and from that point forward it always reflects the latest known state.
PupStatus pumpStatus;

// Timers start at zero so the first loop iteration always samples
// (now - 0 >= sample_interval is always true).
uint32_t lastSensorSampleMs = 0;
uint32_t lastTemperatureSampleMs = 0;
uint32_t lastPumpStatusMs = 0;
uint32_t lastLedUpdateMs = 0;

// Boot button state. 0 means "not pressed."
uint32_t bootButtonPressedAtMs = 0;
uint32_t lastResetWarningMs = 0;
bool printedCommissioningInfo = false;
bool factoryResetTriggered = false;

// Start with the compile-time maximum; the user can lower this via Matter
// or cloud config.
uint16_t maxRpm = DEFAULT_MAX_RPM;

// Device ID buffer. Switched from a static const string to a mutable
// buffer so the cloud module can derive shortened IDs (e.g., for MQTT
// client IDs that have length limits).
char deviceId[64] = {0};

// Cloud connectivity state. Set by main loop every iteration based on
// whether the WiFi station interface has an IP address.
bool cloudConnected = false;

// The single observer callback. Only one handler is supported — the cloud
// service module registers itself at init. This is intentionally simple:
// there's only one consumer of state-change notifications.
static ControllerStateChangedHandler controllerStateChangedHandler = nullptr;

// ---- observer pattern ----

void setControllerStateChangedHandler(ControllerStateChangedHandler handler) {
  controllerStateChangedHandler = handler;
}

// notifyControllerStateChanged is called by relays, pump control, and sensor
// modules whenever their state changes. The handler runs inline on the
// caller's thread, so it must be non-blocking (the cloud service handler
// just sets a "dirty" flag for the next poll).
void notifyControllerStateChanged(bool urgent) {
  if (controllerStateChangedHandler) controllerStateChangedHandler(urgent);
}

// ---- DebouncedDigitalInput ----

// begin() is called once per sensor at boot. It configures the GPIO as an
// input with internal pull-up (sensors are wired active-low: switch closure
// pulls the pin to ground). The initial level is read immediately and set as
// "stable" so we don't get a false transition event on the first update().
void DebouncedDigitalInput::begin(int pin) {
  gpio_num_t gpio = (gpio_num_t)pin;
  gpio_config_t cfg = {
    .pin_bit_mask = (1ULL << gpio),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  // Active-low: gpio_get_level returns 0 when the switch is closed (sensor active).
  // We invert here so that `stable == true` means "sensor is active."
  bool raw = gpio_get_level(gpio) == 0;
  stable = raw;
  lastRaw = raw;
  changedAtMs = (uint32_t)(esp_timer_get_time() / 1000);
}

// update() is called from the main loop at the SENSOR_SAMPLE_MS rate.
// It advances the debounce state machine:
//   1. Read the raw GPIO level.
//   2. If the raw level differs from lastRaw, record the change timestamp.
//   3. If the raw level has been different from `stable` for at least
//      DIGITAL_DEBOUNCE_MS, accept it as the new stable value.
// This filters out contact bounce and brief electrical noise.
void DebouncedDigitalInput::update(int pin) {
  gpio_num_t gpio = (gpio_num_t)pin;
  // Active-low: invert so true = switch closed / sensor active.
  bool raw = gpio_get_level(gpio) == 0;
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAtMs = now;  // Reset the debounce timer on every level change
  }
  if (raw != stable && now - changedAtMs >= DIGITAL_DEBOUNCE_MS) {
    stable = raw;  // Level held long enough — accept it
  }
}

// ---- NVS helpers ----

// Erase-then-init is the simplest way to wipe all NVS namespaces including
// the Matter fabric data stored by the ESP-Matter SDK. A production device
// would want selective namespace erasure, but for a pool controller with
// a single user, full wipe is acceptable and predictable.
void clearNvsConfig() {
  nvs_flash_erase();
  nvs_flash_init();
}

// ---- pump helpers ----

// Maps percent (0–100) to RPM (0 or MIN_RPM..maxRpm).
// 0% always means "off" (0 RPM). Values 1–100% linearly map to
// MIN_RPM..maxRpm. This linear mapping is the convention for Matter
// LevelControl clusters — the "level" scales linearly from min to max.
uint16_t percentToRpm(uint8_t percent) {
  if (percent == 0) return 0;
  long rpm = (long)percent * (maxRpm - MIN_RPM) / 100 + MIN_RPM;
  if (rpm < MIN_RPM) rpm = MIN_RPM;
  if (rpm > maxRpm) rpm = maxRpm;
  return (uint16_t)rpm;
}

// Maps RPM back to percent. RPM below MIN_RPM is treated as "off" (0%).
// The floor of 1% ensures that any running pump shows at least 1% on the
// Matter controller — some controllers treat 0% as "off" and won't display
// a running pump correctly.
uint8_t rpmToPercent(uint16_t rpm) {
  if (rpm < MIN_RPM) return 0;
  long percent = ((long)rpm - MIN_RPM) * 100 / (maxRpm - MIN_RPM);
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  return (uint8_t)percent;
}

// Maps percent (0–100) to Matter LevelControl level (1–254).
// Level 0 is reserved for "off" in the Matter spec, so we shift the range
// to start at 1. 0% → 1, 100% → 254. The +50 in the numerator is for
// banker's rounding to minimize cumulative rounding error.
uint8_t percentToLevel(uint8_t percent) {
  if (percent == 0) return 1;
  if (percent > 100) percent = 100;
  long level = 1 + ((long)percent * 253 + 50) / 100;
  if (level < 1) level = 1;
  if (level > 254) level = 254;
  return (uint8_t)level;
}

// Maps Matter LevelControl level (0–254) back to percent (0–100).
// Level 0 → 0%, Level 1 → 1%, Level 254 → 100%.
uint8_t levelToPercent(uint8_t level) {
  if (level == 0) return 0;
  long percent = ((long)level - 1) * 100 / 253;
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  return (uint8_t)percent;
}

// ---- string converters (used by cloud config parser) ----

// These functions map JSON string values from the cloud shadow delta
// to the enum constants used internally. The cloud dashboard sends
// camelCase strings matching these key names.
//
// The default return values are safe: Aux for relay role (a generic
// relay that can be manually controlled), Disabled for sensor type
// (won't participate in safety interlocks). A malformed cloud config
// should not brick the device.

RelayRole relayRoleFromString(const char *s) {
  if (!s) return RelayRole::Aux;  // Null pointer from cJSON is possible
  if (strcmp(s, "saltWaterGenerator") == 0) return RelayRole::SaltWaterGenerator;
  if (strcmp(s, "poolHeater") == 0) return RelayRole::PoolHeater;
  if (strcmp(s, "acidPump") == 0) return RelayRole::AcidPump;
  if (strcmp(s, "valve") == 0) return RelayRole::Valve;
  if (strcmp(s, "aux") == 0) return RelayRole::Aux;
  return RelayRole::Aux;  // Unknown string → safe default
}

// Returns human-readable short names for log messages and the cloud
// dashboard. These differ from the JSON keys intentionally — the logs
// are for developers, the JSON is for the web UI.
const char *relayRoleName(RelayRole role) {
  switch (role) {
    case RelayRole::SaltWaterGenerator: return "SWG";
    case RelayRole::PoolHeater: return "Heater";
    case RelayRole::AcidPump: return "Acid Pump";
    case RelayRole::Valve: return "Valve";
    case RelayRole::Aux: return "Aux";
    default: return "Disabled";
  }
}

SensorType sensorTypeFromString(const char *s) {
  if (!s) return SensorType::Disabled;
  if (strcmp(s, "waterLevelSwitch") == 0) return SensorType::WaterLevelSwitch;
  if (strcmp(s, "flowSwitch") == 0) return SensorType::FlowSwitch;
  if (strcmp(s, "temperatureDs18b20") == 0) return SensorType::TemperatureDs18b20;
  return SensorType::Disabled;
}

// These names are used as keys in Matter endpoint metadata and cloud
// telemetry. They're stable API identifiers — changing them would break
// the cloud dashboard's sensor panel.
const char *sensorTypeName(SensorType type) {
  switch (type) {
    case SensorType::WaterLevelSwitch: return "water_level";
    case SensorType::FlowSwitch: return "flow";
    case SensorType::TemperatureDs18b20: return "temperature";
    default: return "disabled";
  }
}
