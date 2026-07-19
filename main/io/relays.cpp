// Relay GPIO driver implementation.
//
// Each relay channel is an independent GPIO output with configurable active-
// high or active-low polarity. The polarity is stored in RelayConfig and
// applied transparently by setRelay() — callers always pass "active" meaning
// "relay coil energized," and the pin level is computed internally.
//
// Matter state reporting uses a deferred pattern: the GPIO write happens
// immediately on the caller's thread (usually the main loop), but the Matter
// attribute report is scheduled onto the Matter task via PlatformMgr().
// This avoids holding a mutex across a Matter SDK call that might block.
//
// A per-relay "dirty mask" coalesces multiple rapid changes (e.g., a
// schedule block toggling two relays simultaneously) into a single deferred
// callback on the Matter thread.

#include "io/relays.h"

#include <cstring>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>

#include "app/state.h"
#include "board/board_pins.h"

static const char *TAG = "relays";

// Guards against setupRelaysSafe() being called before GPIOs are ready.
// After setup, this is always true and is checked by setRelay() to avoid
// writing to unconfigured GPIOs in early boot paths.
static bool relaysConfigured = false;

// Protects the shared relayMatterReport struct between the main loop
// (which calls setRelay) and the Matter task (which runs the deferred
// report callback). A spinlock is appropriate because the critical
// section is just a few struct field assignments.
static portMUX_TYPE relayMatterMux = portMUX_INITIALIZER_UNLOCKED;

// Deferred Matter report state. The "pending" flag prevents scheduling
// duplicate callbacks. The "dirtyMask" is a bitmask of relay indices that
// have changed since the last report. Multiple changes to the same relay
// are coalesced — only the most recent state is reported.
static struct {
  bool pending = false;
  uint8_t dirtyMask = 0;
  bool states[BoardPins::RelayCount] = {};
} relayMatterReport;

// ---- Deferred Matter reporting ----

// Runs on the Matter thread (via PlatformMgr().ScheduleWork). Copies the
// dirty state under the spinlock, clears the dirty mask and pending flag,
// then reports each changed relay to Matter. If a relay has no endpoint
// (endpoint ID == 0), it's skipped — not all relay channels are exposed
// to Matter.
static void reportRelaysToMatter(intptr_t) {
  uint8_t dirtyMask;
  bool states[BoardPins::RelayCount];

  // Snapshot the dirty state under the lock, then release it so the
  // main loop can continue queuing new changes while we report.
  taskENTER_CRITICAL(&relayMatterMux);
  dirtyMask = relayMatterReport.dirtyMask;
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    states[i] = relayMatterReport.states[i];
  }
  relayMatterReport.dirtyMask = 0;
  relayMatterReport.pending = false;
  taskEXIT_CRITICAL(&relayMatterMux);

  // Report each dirty relay to its Matter OnOff cluster.
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    if ((dirtyMask & (1U << i)) == 0 || relayEndpointIds[i] == 0) continue;

    esp_matter_attr_val_t val = esp_matter_bool(states[i]);
    esp_err_t err = esp_matter::attribute::report(relayEndpointIds[i],
        chip::app::Clusters::OnOff::Id,
        chip::app::Clusters::OnOff::Attributes::OnOff::Id, &val);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Matter relay %u report failed: %s", i + 1, esp_err_to_name(err));
    }
  }
}

// Called from setRelay() on the main loop thread. Updates the deferred
// report state under the spinlock and, if no report is already pending,
// schedules the callback on the Matter thread. If scheduling fails
// (unlikely — would mean the Matter task queue is full), we clear the
// pending flag so the next change will retry.
static void scheduleRelayMatterReport(uint8_t index, bool active) {
  bool shouldSchedule = false;
  taskENTER_CRITICAL(&relayMatterMux);
  relayMatterReport.states[index] = active;
  relayMatterReport.dirtyMask |= (1U << index);
  if (!relayMatterReport.pending) {
    relayMatterReport.pending = true;
    shouldSchedule = true;
  }
  taskEXIT_CRITICAL(&relayMatterMux);

  if (shouldSchedule) {
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(reportRelaysToMatter, 0);
    if (err != CHIP_NO_ERROR) {
      // Scheduling failed — reset pending so we retry on the next change.
      taskENTER_CRITICAL(&relayMatterMux);
      relayMatterReport.pending = false;
      taskEXIT_CRITICAL(&relayMatterMux);
      ESP_LOGW(TAG, "Scheduling Matter relay report failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
  }
}

// ---- Public API ----

// Configures all relay GPIO pins as outputs and drives them to their
// inactive (safe) levels. Relay configs are default-enabled for all
// channels — the public firmware exposes every relay to Matter. Custom
// builds can modify this in the board config.
//
// The inactive level is computed from activeHigh: if activeHigh is true,
// the relay is active when GPIO is HIGH, so the inactive level is LOW.
// If activeHigh is false (active-low wiring), the inactive level is HIGH.
//
// Default relay names are set to "Relay 1", "Relay 2", etc. if no name
// was provided by the board config.
void setupRelaysSafe() {
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    // Enable all relays by default in the public firmware. The cloud
    // shadow delta can override individual relay configs at runtime.
    relayConfigs[i].enabled = true;
    gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << BoardPins::RelayPins[i]),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    relayStates[i] = false;
    // Drive to inactive level immediately — don't wait for the first command.
    gpio_set_level((gpio_num_t)BoardPins::RelayPins[i], relayConfigs[i].activeHigh ? 0 : 1);
    if (relayConfigs[i].name.empty()) {
      relayConfigs[i].name = "Relay " + std::to_string(i + 1);
    }
  }
  relaysConfigured = true;
}

// Iterates all relays and sets each to inactive. Used for boot-time safety
// and flow-lockout shutdown. The "source" parameter is logged to make
// forensics easier — if a relay was turned off unexpectedly, the log shows
// whether it was the schedule, cloud, flow lockout, or user action.
void setAllRelaysOff(const char *source) {
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    setRelay(i, false, source);
  }
}

// Sets a single relay's physical output and updates the shared state.
//
// Safety checks in order:
//   1. Bounds check the index.
//   2. Track whether the state actually changed (for observer notification).
//   3. Early return if GPIOs aren't configured yet (boot-early safety).
//   4. Apply activeHigh polarity to compute the pin level.
//   5. Re-report to Matter unless the change came from Matter itself.
//   6. Notify the controller state observer if state changed.
void setRelay(uint8_t index, bool active, const char *source) {
  if (index >= BoardPins::RelayCount) return;
  const bool changed = relayStates[index] != active;
  relayStates[index] = active;
  if (!relaysConfigured) return;

  // Apply activeHigh polarity. activeHigh=true: HIGH=on, LOW=off.
  // activeHigh=false: LOW=on, HIGH=off (active-low relay modules).
  bool pinLevel = relayConfigs[index].activeHigh ? active : !active;
  gpio_set_level((gpio_num_t)BoardPins::RelayPins[index], pinLevel ? 1 : 0);

  // Matter initiated this change — don't report back to avoid a loop.
  // All other sources (schedule, cloud, flow lockout, user) report.
  if (relayEndpointIds[index] != 0 && strcmp(source, "matter") != 0) {
    scheduleRelayMatterReport(index, active);
  }

  if (changed) notifyControllerStateChanged();

  ESP_LOGI(TAG, "Relay %u %s from %s", index + 1, active ? "ON" : "OFF", source);
}

// ---- Flow lockout detection ----

// Scans all enabled flow-switch sensors that have lockout enabled. If any
// of them reports no flow (stable == false), return true. The caller
// (Matter module, main loop) then forces all relays off and stops the pump.
//
// This is a safety interlock: if water stops flowing through the plumbing
// (e.g., a closed valve, blocked skimmer, or pump failure), the heater and
// SWG must be shut down immediately to prevent damage.
bool flowLockoutActive() {
  for (uint8_t i = 0; i < BoardPins::SensorCount; i++) {
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
