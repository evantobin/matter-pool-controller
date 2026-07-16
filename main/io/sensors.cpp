#include "io/sensors.h"

#include <cmath>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <esp_matter_attribute_utils.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>

#include "app/state.h"
#include "board/board_pins.h"
#include "board/board_sensor_config.h"

static const char *TAG = "sensors";

// Sampling happens in the main loop; Matter attribute changes are deferred onto
// the Matter thread to keep GPIO timing independent of the Matter stack.
static portMUX_TYPE sensorMatterMux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bool pending = false;
  bool sendWaterLevel = false;
  bool waterLevelActive = false;
  bool sendFlow = false;
  bool flowActive = false;
  bool sendTemperature = false;
  bool temperatureValid = false;
  int16_t temperatureValue = 0;
} sensorMatterReport;

static void logMatterUpdateError(const char *name, esp_err_t err) {
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Matter %s update failed: %s", name, esp_err_to_name(err));
  }
}

static void updateSensorsOnMatterThread(intptr_t) {
  bool sendWaterLevel;
  bool waterLevelActive;
  bool sendFlow;
  bool flowActive;
  bool sendTemperature;
  bool temperatureValid;
  int16_t temperatureValue;

  taskENTER_CRITICAL(&sensorMatterMux);
  sendWaterLevel = sensorMatterReport.sendWaterLevel;
  waterLevelActive = sensorMatterReport.waterLevelActive;
  sendFlow = sensorMatterReport.sendFlow;
  flowActive = sensorMatterReport.flowActive;
  sendTemperature = sensorMatterReport.sendTemperature;
  temperatureValid = sensorMatterReport.temperatureValid;
  temperatureValue = sensorMatterReport.temperatureValue;
  sensorMatterReport.sendWaterLevel = false;
  sensorMatterReport.sendFlow = false;
  sensorMatterReport.sendTemperature = false;
  sensorMatterReport.pending = false;
  taskEXIT_CRITICAL(&sensorMatterMux);

  if (sendWaterLevel && waterLevelSensorEndpointId != 0) {
    esp_matter_attr_val_t val = esp_matter_bool(waterLevelActive);
    logMatterUpdateError("water level", esp_matter::attribute::update(waterLevelSensorEndpointId,
        chip::app::Clusters::BooleanState::Id,
        chip::app::Clusters::BooleanState::Attributes::StateValue::Id, &val));
  }

  if (sendFlow && flowSensorEndpointId != 0) {
    esp_matter_attr_val_t val = esp_matter_bool(flowActive);
    logMatterUpdateError("flow", esp_matter::attribute::update(flowSensorEndpointId,
        chip::app::Clusters::BooleanState::Id,
        chip::app::Clusters::BooleanState::Attributes::StateValue::Id, &val));
  }

  if (sendTemperature && temperatureSensorEndpointId != 0) {
    esp_matter_attr_val_t val = temperatureValid
        ? esp_matter_int16(temperatureValue)
        : esp_matter_nullable_int16(nullable<int16_t>());
    logMatterUpdateError("temperature", esp_matter::attribute::update(temperatureSensorEndpointId,
        chip::app::Clusters::TemperatureMeasurement::Id,
        chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id, &val));
  }
}

static void scheduleSensorMatterUpdate() {
  bool shouldSchedule = false;
  taskENTER_CRITICAL(&sensorMatterMux);
  if (!sensorMatterReport.pending) {
    sensorMatterReport.pending = true;
    shouldSchedule = true;
  }
  taskEXIT_CRITICAL(&sensorMatterMux);

  if (shouldSchedule) {
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(updateSensorsOnMatterThread, 0);
    if (err != CHIP_NO_ERROR) {
      taskENTER_CRITICAL(&sensorMatterMux);
      sensorMatterReport.pending = false;
      taskEXIT_CRITICAL(&sensorMatterMux);
      ESP_LOGW(TAG, "Scheduling Matter sensor update failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
  }
}

static void queueWaterLevelMatterUpdate(bool active) {
  taskENTER_CRITICAL(&sensorMatterMux);
  sensorMatterReport.waterLevelActive = active;
  sensorMatterReport.sendWaterLevel = true;
  taskEXIT_CRITICAL(&sensorMatterMux);
  scheduleSensorMatterUpdate();
}

static void queueFlowMatterUpdate(bool active) {
  taskENTER_CRITICAL(&sensorMatterMux);
  sensorMatterReport.flowActive = active;
  sensorMatterReport.sendFlow = true;
  taskEXIT_CRITICAL(&sensorMatterMux);
  scheduleSensorMatterUpdate();
}

static void queueTemperatureMatterUpdate(bool valid, int16_t value) {
  taskENTER_CRITICAL(&sensorMatterMux);
  sensorMatterReport.temperatureValid = valid;
  sensorMatterReport.temperatureValue = value;
  sensorMatterReport.sendTemperature = true;
  taskEXIT_CRITICAL(&sensorMatterMux);
  scheduleSensorMatterUpdate();
}

// ---------- Minimal OneWire for DS18B20 (dedicated GPIO per sensor) ----------

static void ow_pin_output(gpio_num_t pin) {
  gpio_config_t cfg = {
    .pin_bit_mask = (1ULL << pin),
    .mode = GPIO_MODE_OUTPUT_OD,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
}

static void ow_write_bit(gpio_num_t pin, bool bit) {
  taskDISABLE_INTERRUPTS();
  if (bit) {
    gpio_set_level(pin, 0);
    esp_rom_delay_us(2);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(58);
  } else {
    gpio_set_level(pin, 0);
    esp_rom_delay_us(60);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(2);
  }
  taskENABLE_INTERRUPTS();
}

static bool ow_read_bit(gpio_num_t pin) {
  taskDISABLE_INTERRUPTS();
  gpio_set_level(pin, 0);
  esp_rom_delay_us(2);
  gpio_set_level(pin, 1);
  esp_rom_delay_us(10);
  bool bit = gpio_get_level(pin);
  esp_rom_delay_us(48);
  taskENABLE_INTERRUPTS();
  return bit;
}

static bool ow_reset(gpio_num_t pin) {
  ow_pin_output(pin);

  taskDISABLE_INTERRUPTS();
  gpio_set_level(pin, 0);
  esp_rom_delay_us(480);
  gpio_set_level(pin, 1);
  esp_rom_delay_us(70);
  bool present = gpio_get_level(pin) == 0;
  esp_rom_delay_us(410);
  taskENABLE_INTERRUPTS();

  return present;
}

static void ow_write_byte(gpio_num_t pin, uint8_t val) {
  for (int i = 0; i < 8; i++) {
    ow_write_bit(pin, (val >> i) & 1);
  }
}

static uint8_t ow_read_byte(gpio_num_t pin) {
  uint8_t val = 0;
  for (int i = 0; i < 8; i++) {
    if (ow_read_bit(pin)) {
      val |= (1 << i);
    }
  }
  return val;
}

// ---------- DS18B20 helpers ----------

static void ds18b20_request(gpio_num_t pin) {
  if (!ow_reset(pin)) return;
  ow_write_byte(pin, 0xCC); // Skip ROM
  ow_write_byte(pin, 0x44); // Convert T
}

static float ds18b20_read(gpio_num_t pin) {
  if (!ow_reset(pin)) return NAN;
  ow_write_byte(pin, 0xCC); // Skip ROM
  ow_write_byte(pin, 0xBE); // Read Scratchpad

  uint8_t lsb = ow_read_byte(pin);
  uint8_t msb = ow_read_byte(pin);

  // Reset to abort scratchpad read (don't need CRC for single sensor)
  ow_reset(pin);

  int16_t raw = ((int16_t)msb << 8) | lsb;
  return raw * 0.0625f;
}

// ---------- Sensor setup ----------

static SensorType toSensorType(BoardSensorConfig::Type type) {
  switch (type) {
    case BoardSensorConfig::Type::WaterLevelSwitch:
      return SensorType::WaterLevelSwitch;
    case BoardSensorConfig::Type::FlowSwitch:
      return SensorType::FlowSwitch;
    case BoardSensorConfig::Type::TemperatureDs18b20:
      return SensorType::TemperatureDs18b20;
    case BoardSensorConfig::Type::Disabled:
    default:
      return SensorType::Disabled;
  }
}

void setupSensors() {
  for (uint8_t i = 0; i < BoardPins::SensorCount; i++) {
    const BoardSensorConfig::Config &boardConfig = BoardSensorConfig::Sensors[i];
    sensorConfigs[i].type = toSensorType(boardConfig.type);
    sensorConfigs[i].enabled = sensorConfigs[i].type != SensorType::Disabled;
    sensorConfigs[i].name = boardConfig.name;
    sensorConfigs[i].lockoutEnabled = boardConfig.flowLockoutEnabled;
    sensorInputs[i].begin(BoardPins::SensorPins[i]);
  }
}

// ---------- Sensor sampling ----------

void sampleSensors() {
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if (now - lastSensorSampleMs < SENSOR_SAMPLE_MS) return;
  lastSensorSampleMs = now;

  bool waterLevelActive = false;
  bool flowActive = false;

  for (uint8_t i = 0; i < BoardPins::SensorCount; i++) {
    sensorInputs[i].update(BoardPins::SensorPins[i]);
    if (!sensorConfigs[i].enabled) continue;
    if (sensorConfigs[i].type == SensorType::WaterLevelSwitch) {
      waterLevelActive = sensorInputs[i].stable;
    } else if (sensorConfigs[i].type == SensorType::FlowSwitch) {
      flowActive = sensorInputs[i].stable;
    }
  }

  // Only update Matter attributes when the value actually changes
  // (avoids ESP_ERR_NOT_SUPPORTED spam on every sample for internally-managed attributes)
  static bool lastWaterLevel = false;
  static bool lastFlow = false;
  static bool firstUpdate = true;

  if (waterLevelSensorEndpointId != 0 && (firstUpdate || waterLevelActive != lastWaterLevel)) {
    queueWaterLevelMatterUpdate(waterLevelActive);
    lastWaterLevel = waterLevelActive;
  }

  if (flowSensorEndpointId != 0 && (firstUpdate || flowActive != lastFlow)) {
    queueFlowMatterUpdate(flowActive);
    lastFlow = flowActive;
  }

  firstUpdate = false;

  // Temperature sampling (less frequent)
  if (now - lastTemperatureSampleMs >= TEMPERATURE_SAMPLE_MS) {
    lastTemperatureSampleMs = now;
    bool temperatureUpdated = false;

    for (uint8_t i = 0; i < BoardPins::SensorCount; i++) {
      if (!sensorConfigs[i].enabled || sensorConfigs[i].type != SensorType::TemperatureDs18b20) {
        temperatureC[i] = NAN;
        temperatureFault[i] = false;
        continue;
      }

      gpio_num_t pin = (gpio_num_t)BoardPins::SensorPins[i];

      if (isnan(temperatureC[i])) {
        // First read: request conversion
        ds18b20_request(pin);
        temperatureC[i] = 0.0f; // Mark as pending (not NaN, not valid yet)
      } else {
        // Read result, then request next
        float value = ds18b20_read(pin);
        ds18b20_request(pin);

        if (isnan(value) || value < -55.0f || value > 125.0f) {
          temperatureC[i] = NAN;
          temperatureFault[i] = true;
        } else {
          temperatureC[i] = value;
          temperatureFault[i] = false;
          temperatureUpdated = true;

          if (temperatureSensorEndpointId != 0) {
            int16_t matterTemp = (int16_t)(value * 100); // Matter uses hundredths of a degree
            queueTemperatureMatterUpdate(true, matterTemp);
          }
        }
      }
    }

    if (!temperatureUpdated && temperatureSensorEndpointId != 0) {
      queueTemperatureMatterUpdate(false, 0);
    }
  }
}
