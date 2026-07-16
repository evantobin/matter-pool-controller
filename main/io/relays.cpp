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
static bool relaysConfigured = false;
static portMUX_TYPE relayMatterMux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bool pending = false;
  uint8_t dirtyMask = 0;
  bool states[BoardPins::RelayCount] = {};
} relayMatterReport;

static void reportRelaysToMatter(intptr_t) {
  uint8_t dirtyMask;
  bool states[BoardPins::RelayCount];

  taskENTER_CRITICAL(&relayMatterMux);
  dirtyMask = relayMatterReport.dirtyMask;
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    states[i] = relayMatterReport.states[i];
  }
  relayMatterReport.dirtyMask = 0;
  relayMatterReport.pending = false;
  taskEXIT_CRITICAL(&relayMatterMux);

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
      taskENTER_CRITICAL(&relayMatterMux);
      relayMatterReport.pending = false;
      taskEXIT_CRITICAL(&relayMatterMux);
      ESP_LOGW(TAG, "Scheduling Matter relay report failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
  }
}

void setupRelaysSafe() {
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    // Public firmware exposes all relay channels by default. Each output is
    // driven to its inactive level before Matter can receive a command.
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
    gpio_set_level((gpio_num_t)BoardPins::RelayPins[i], relayConfigs[i].activeHigh ? 0 : 1);
    if (relayConfigs[i].name.empty()) {
      relayConfigs[i].name = "Relay " + std::to_string(i + 1);
    }
  }
  relaysConfigured = true;
}

void setAllRelaysOff(const char *source) {
  for (uint8_t i = 0; i < BoardPins::RelayCount; i++) {
    setRelay(i, false, source);
  }
}

void setRelay(uint8_t index, bool active, const char *source) {
  if (index >= BoardPins::RelayCount) return;
  relayStates[index] = active;
  if (!relaysConfigured) return;

  bool pinLevel = relayConfigs[index].activeHigh ? active : !active;
  gpio_set_level((gpio_num_t)BoardPins::RelayPins[index], pinLevel ? 1 : 0);

  // Report to Matter (only if NOT triggered by Matter itself)
  if (relayEndpointIds[index] != 0 && strcmp(source, "matter") != 0) {
    scheduleRelayMatterReport(index, active);
  }

  ESP_LOGI(TAG, "Relay %u %s from %s", index + 1, active ? "ON" : "OFF", source);
}

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
