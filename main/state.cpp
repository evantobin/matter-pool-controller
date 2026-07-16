#include "state.h"

#include <cmath>
#include <string.h>

#include <driver/gpio.h>
#include <esp_timer.h>
#include <nvs_flash.h>

// ---------- static sensor buses ----------

// ---------- global definitions ----------

RelayConfig relayConfigs[BoardPins::RelayCount];
SensorConfig sensorConfigs[BoardPins::SensorCount];
DebouncedDigitalInput sensorInputs[BoardPins::SensorCount];

float temperatureC[BoardPins::SensorCount] = {NAN, NAN, NAN, NAN};
bool temperatureFault[BoardPins::SensorCount] = {false, false, false, false};
bool relayStates[BoardPins::RelayCount] = {false, false, false, false, false, false};
uint16_t relayEndpointIds[BoardPins::RelayCount] = {0};
uint16_t pumpEndpointId = 0;
uint16_t waterLevelSensorEndpointId = 0;
uint16_t flowSensorEndpointId = 0;
uint16_t temperatureSensorEndpointId = 0;

PupStatus pumpStatus;
uint32_t lastSensorSampleMs = 0;
uint32_t lastTemperatureSampleMs = 0;
uint32_t lastPumpStatusMs = 0;
uint32_t lastCommissioningLogMs = 0;
uint32_t lastLedUpdateMs = 0;
uint32_t bootButtonPressedAtMs = 0;
uint32_t lastResetWarningMs = 0;
bool printedCommissioningInfo = false;
bool factoryResetTriggered = false;
uint16_t maxRpm = DEFAULT_MAX_RPM;
char deviceId[64] = {0};
// ---------- DebouncedDigitalInput ----------

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
  bool raw = gpio_get_level(gpio) == 0;
  stable = raw;
  lastRaw = raw;
  changedAtMs = (uint32_t)(esp_timer_get_time() / 1000);
}

void DebouncedDigitalInput::update(int pin) {
  gpio_num_t gpio = (gpio_num_t)pin;
  bool raw = gpio_get_level(gpio) == 0;
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAtMs = now;
  }
  if (raw != stable && now - changedAtMs >= DIGITAL_DEBOUNCE_MS) {
    stable = raw;
  }
}

// ---------- NVS helpers ----------

void clearNvsConfig() {
  nvs_flash_erase();
  nvs_flash_init();
}

// ---------- pump helpers ----------

uint16_t percentToRpm(uint8_t percent) {
  if (percent == 0) return 0;
  long rpm = (long)percent * (maxRpm - MIN_RPM) / 100 + MIN_RPM;
  if (rpm < MIN_RPM) rpm = MIN_RPM;
  if (rpm > maxRpm) rpm = maxRpm;
  return (uint16_t)rpm;
}

uint8_t rpmToPercent(uint16_t rpm) {
  if (rpm < MIN_RPM) return 0;
  long percent = ((long)rpm - MIN_RPM) * 100 / (maxRpm - MIN_RPM);
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  return (uint8_t)percent;
}

uint8_t percentToLevel(uint8_t percent) {
  if (percent == 0) return 1;
  if (percent > 100) percent = 100;
  long level = 1 + ((long)percent * 253 + 50) / 100;
  if (level < 1) level = 1;
  if (level > 254) level = 254;
  return (uint8_t)level;
}

uint8_t levelToPercent(uint8_t level) {
  if (level == 0) return 0;
  long percent = ((long)level - 1) * 100 / 253;
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  return (uint8_t)percent;
}
