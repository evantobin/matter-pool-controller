// Shared runtime state and compile-time constants.
//
// This header is the single source of truth for cross-module state. Every
// subsystem (Matter endpoints, relays, sensors, pump control, schedule, cloud)
// reads and writes through these globals instead of maintaining its own
// shadow copies. This eliminates synchronization bugs between, e.g., a Matter
// OnOff command and a schedule-driven relay toggle.
//
// Persistent configuration (GPIO pin maps, sensor wiring, device identity)
// lives in board/ and is not duplicated here. Only genuinely shared mutable
// state belongs in this file — keep module-local state in the module's own
// .cpp file.
//
// Value-range constants (MIN_RPM, MAX_GPM, etc.) are "the hardware can't
// exceed this" limits. Defaults are "what we start with before the user or
// cloud overrides it." Both are compile-time to keep startup fast and
// predictable.

#pragma once

#include <stdint.h>
#include <string>

#include <esp_matter.h>
#include <esp_matter_endpoint.h>

#include "board/board_identity.h"
#include "board/board_pins.h"
#include "pump/pump_protocol.h"

// ---- compile-time constants ----

// Firmware version reported over Matter and MQTT telemetry.
// The GIT_COMMIT macro is injected by the build system via a generated header;
// if missing (e.g. IDE build), fall back to "unknown".
static constexpr const char *FIRMWARE_VERSION = "0.2.0";
#ifdef POOLCONDUCTOR_GIT_COMMIT
static constexpr const char *GIT_COMMIT = POOLCONDUCTOR_GIT_COMMIT;
#else
static constexpr const char *GIT_COMMIT = "unknown";
#endif

// Device identity comes from board/board_identity.h and is burned at
// provisioning time. Different physical boards share the same firmware binary
// but carry a unique ID in their compile-time board config.
static constexpr const char *DEVICE_ID = BOARD_DEVICE_ID;

// Pump protocol is a compile-time switch. The protocol abstraction layer
// (pump_protocol.h) resolves to the correct RS-485 implementation at link
// time based on the board config. Address 96 is the Pentair factory default;
// Jandy/Hayward implementations remap it internally.
static constexpr PumpProtocolType PUMP_PROTOCOL = PumpProtocolType::Pentair;
static constexpr uint8_t PUMP_ADDRESS = 96;

// ---- timing constants (milliseconds) ----

// How often we poll the pump for a fresh status frame. Faster than this and
// the RS-485 bus gets congested; slower risks stale state during fault
// conditions.
static constexpr uint32_t STATUS_POLL_MS = 5000;

// How often we re-send the current pump target even when nothing changed.
// Pentair pumps have no watchdog, but if an RS-485 frame was lost or the pump
// was power-cycled, re-sending the target within this window keeps it running
// at the desired speed.
static constexpr uint32_t PUMP_KEEPALIVE_MS = 5000;

// Debounce and sampling rates. Digital inputs are sampled at 100 ms (10 Hz)
// which is fast enough to feel responsive but slow enough to filter contact
// bounce. Temperature reads happen every 30 s because DS18B20 conversions
// take up to 750 ms and temperature changes slowly in a pool.
static constexpr uint32_t SENSOR_SAMPLE_MS = 100;
static constexpr uint32_t TEMPERATURE_SAMPLE_MS = 30000;

// Minimum time a digital input must hold its new level before we consider it
// "stable." Chosen to be well above typical relay/switch bounce (5–50 ms).
static constexpr uint32_t DIGITAL_DEBOUNCE_MS = 250;

// Holding the boot button for this long triggers a factory reset. Must be
// held continuously — any release before this threshold is ignored.
// Coincides with the Matter "factory reset" long-press convention so users
// get a single reset mechanism regardless of commissioning state.
static constexpr uint32_t FACTORY_RESET_HOLD_MS = 10000;

// ---- pump operating limits ----

// The pump physically cannot run below 450 RPM (Pentair IntelliFlo minimum).
// DEFAULT_RPM is a reasonable "medium speed" for energy efficiency —
// high enough to move water, low enough to save power.
// DEFAULT_MAX_RPM is the max before the user overrides it.
static constexpr uint16_t MIN_RPM = 450;
static constexpr uint16_t DEFAULT_RPM = 1800;
static constexpr uint16_t DEFAULT_MAX_RPM = 3450;

// GPM limits: 20 is the minimum detectable flow, 140 is the pump's practical
// maximum at typical head. 40 GPM is a good default for general circulation.
static constexpr uint8_t MIN_GPM = 20;
static constexpr uint8_t DEFAULT_GPM = 40;
static constexpr uint8_t MAX_GPM = 140;

using namespace chip::app::Clusters;

// ---- enums ----

// Each relay channel can be assigned one of these roles. The role determines
// how the relay interacts with safety logic (flow lockout) and the schedule
// (SWG relay is toggled by schedule blocks; other roles ignore the schedule).
// Disabled channels are not exposed as Matter endpoints.
enum class RelayRole : uint8_t {
  Disabled,
  SaltWaterGenerator,
  PoolHeater,
  AcidPump,
  Valve,
  Aux
};

// Each sensor input can be one of these types. The type determines the
// sampling strategy, Matter endpoint type, and safety interlocks.
// WaterLevelSwitch and FlowSwitch are debounced digital inputs.
// TemperatureDs18b20 uses bit-banged OneWire on the same GPIO pin.
enum class SensorType : uint8_t {
  Disabled,
  WaterLevelSwitch,
  FlowSwitch,
  TemperatureDs18b20
};

// The user-visible status is a derived value, not a separate state machine.
// Booting → Commissioning → Online is the normal flow. FlowLockout is entered
// when a flow-switch sensor with lockout enabled reports no flow; it forces
// all relays off and stops the pump for safety.
enum class UserStatus : uint8_t {
  Booting,
  Commissioning,
  Online,
  FlowLockout
};

// ---- configuration structs ----

// Per-relay configuration. Set at boot from board_sensor_config.h defaults,
// then overridden by cloud shadow delta at runtime. activeHigh = true means
// the relay coil is energized when the GPIO is high (most opto-isolated
// relay modules). Some boards are wired active-low.
struct RelayConfig {
  bool enabled = false;
  RelayRole role = RelayRole::Disabled;
  std::string name;
  bool activeHigh = true;
};

// Per-sensor configuration. lockoutEnabled is only meaningful for
// FlowSwitch sensors — when true, loss of flow triggers a safety shutdown
// of all relays and the pump.
struct SensorConfig {
  bool enabled = false;
  SensorType type = SensorType::Disabled;
  std::string name;
  bool lockoutEnabled = false;
};

// Software debounce state machine for a single digital input.
// begin() samples the initial level and considers it stable immediately
// (avoids a brief "no water" state at boot). update() advances the
// state machine: if the raw level differs from lastRaw, the change
// timestamp is recorded. If the raw level has been different from stable
// for at least DIGITAL_DEBOUNCE_MS, stable is updated.
struct DebouncedDigitalInput {
  bool stable = false;       // The debounced output — what callers read
  bool lastRaw = false;      // Most recent raw GPIO level
  uint32_t changedAtMs = 0;  // Timestamp of the last raw-level change

  void begin(int pin);
  void update(int pin);
};

// Snapshot of the pump status from the latest RS-485 status frame.
// valid = false until the first complete frame is received.
// pumpError is set to 1 on any fault bit in the status word.
// The Pentair status frame is 26 bytes; this struct holds the parsed fields.
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

// ---- forward declarations ----

class PentairPump;

// ---- globals (defined in state.cpp) ----

// Fixed-size arrays sized by the board pin map. Using BoardPins::RelayCount
// and BoardPins::SensorCount ensures the firmware compiles for any board
// variant without manual array resizing.

extern RelayConfig relayConfigs[BoardPins::RelayCount];
extern SensorConfig sensorConfigs[BoardPins::SensorCount];
extern DebouncedDigitalInput sensorInputs[BoardPins::SensorCount];

// Temperature values in degrees Celsius. NaN means "no reading yet."
// temperatureFault[i] is set when the DS18B20 returns an out-of-range value
// or fails to respond after multiple attempts.
extern float temperatureC[BoardPins::SensorCount];
extern bool temperatureFault[BoardPins::SensorCount];

// Relay output state as last commanded. May differ from the physical GPIO
// if relays haven't been configured yet (boot-early safety).
extern bool relayStates[BoardPins::RelayCount];

// Matter endpoint IDs assigned during endpoint creation. Zero means "no
// endpoint exists for this channel" — callers must check before using.
extern uint16_t relayEndpointIds[BoardPins::RelayCount];
extern uint16_t pumpEndpointId;
extern uint16_t waterLevelSensorEndpointId;
extern uint16_t flowSensorEndpointId;
extern uint16_t temperatureSensorEndpointId;

// Pump status snapshot. Updated asynchronously by readPumpStatus().
extern PupStatus pumpStatus;

// Last-sample timestamps drive the rate-limiting in the main loop.
// Using esp_timer_get_time() / 1000 gives a monotonically increasing
// millisecond counter that survives light sleep but not deep sleep.
extern uint32_t lastSensorSampleMs;
extern uint32_t lastTemperatureSampleMs;
extern uint32_t lastPumpStatusMs;
extern uint32_t lastLedUpdateMs;

// Boot button state machine. pressedAtMs is set when the button goes down;
// if it's still down after FACTORY_RESET_HOLD_MS, factory reset is triggered.
extern uint32_t bootButtonPressedAtMs;
extern uint32_t lastResetWarningMs;

// Commissioning info is printed once to the UART console so the user can
// capture the QR code and manual pairing code.
extern bool printedCommissioningInfo;

// Set to true by the boot button state machine when the long-press threshold
// is reached. The flag is checked once per loop iteration so the reset
// happens at a defined point, not inside an ISR.
extern bool factoryResetTriggered;

// The user-adjustable maximum RPM. Clamped to MIN_RPM..3450.
extern uint16_t maxRpm;

// The board-specific device ID copied from DEVICE_ID at boot. 64 bytes is
// generous for a UUID or MAC-derived string.
extern char deviceId[64];

// Reflects whether the cloud MQTT connection is currently established.
// Used by the LED feedback and schedule gating.
extern bool cloudConnected;

// ---- observer pattern ----

// Optional callback invoked when any controller state changes (relay toggle,
// pump target change, sensor fault). The "urgent" flag is set for fault
// transitions so the cloud service can publish an immediate telemetry report
// instead of waiting for the next coalesced interval.
//
// This pattern avoids making the pump/relay/sensor modules depend on the
// cloud service module — the cloud module registers itself as an observer
// at init time.
using ControllerStateChangedHandler = void (*)(bool urgent);

void setControllerStateChangedHandler(ControllerStateChangedHandler handler);
void notifyControllerStateChanged(bool urgent = false);

// ---- helpers ----

// Wipes all NVS, including Matter fabric data and user configuration.
// Used by factory reset (boot button long-press) and on first-boot when
// NVS is corrupt or from an incompatible firmware version.
void clearNvsConfig();

// Convert between percent (0–100), RPM (MIN_RPM..maxRpm), and Matter
// LevelControl level (1–254). The percent↔level mapping reserves level 0
// as "off" so it's never produced by percentToLevel.
uint16_t percentToRpm(uint8_t percent);
uint8_t rpmToPercent(uint16_t rpm);
uint8_t percentToLevel(uint8_t percent);
uint8_t levelToPercent(uint8_t level);

// String parsers for cloud config JSON. If the string doesn't match any
// known value, a safe default is returned (Aux for relays, Disabled for
// sensors) so a malformed cloud config doesn't crash the device.
RelayRole relayRoleFromString(const char *s);
const char *relayRoleName(RelayRole role);
SensorType sensorTypeFromString(const char *s);
const char *sensorTypeName(SensorType type);
