// Sensor input driver implementation.
//
// Digital sensors (water level, flow switches) use the DebouncedDigitalInput
// state machine from state.h. Each sensor GPIO is configured as an input with
// internal pull-up; the switch pulls the pin low when active (closed). The
// debounce logic inverts this so `stable == true` means "sensor active."
//
// Temperature sensors (DS18B20) use a bit-banged OneWire implementation.
// We avoid external libraries (like OneWire or DallasTemperature) to keep
// the firmware dependency-free and to have full control over the GPIO
// timing, which must be precise for DS18B20 communication.
//
// The DS18B20 read sequence uses a two-phase approach:
//   1. On the first sample, send a Convert T command (0x44).
//   2. On the next sample (30 s later), read the scratchpad, then send
//      another Convert T to start the next conversion.
// This gives the sensor the maximum conversion time (up to 750 ms for
// 12-bit resolution) and spreads the power draw across sample intervals.

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
#include <app/ConcreteClusterPath.h>
#include <app/clusters/boolean-state-server/BooleanStateCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>

#include "app/state.h"
#include "board/board_pins.h"
#include "board/board_sensor_config.h"

static const char *TAG = "sensors";

// ---- Deferred Matter reporting ----

// Sampling happens in the main loop; Matter attribute changes are deferred
// onto the Matter thread to keep GPIO timing independent of the Matter
// stack. The spinlock protects the shared struct below — only the main loop
// writes to it, but the Matter thread reads and clears it asynchronously.
static portMUX_TYPE sensorMatterMux = portMUX_INITIALIZER_UNLOCKED;

// Coalesced sensor state for deferred Matter updates. The "send" flags
// indicate new data is available. Multiple samples of the same sensor are
// coalesced — only the latest value is reported. The "pending" flag
// prevents scheduling duplicate callbacks on the Matter thread.
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

// Logs Matter update errors at WARN level. Individual report failures are
// not fatal — the next sample will retry. We log them so intermittent
// Matter stack issues are visible in the console.
static void logMatterUpdateError(const char *name, esp_err_t err) {
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Matter %s update failed: %s", name, esp_err_to_name(err));
  }
}

// Helper to retrieve a typed Matter cluster pointer from the data model
// provider. Returns nullptr if the endpoint doesn't have the requested
// cluster, which callers check before calling cluster methods.
template <typename ClusterType>
static ClusterType *matterServerCluster(uint16_t endpointId, uint32_t clusterId) {
  chip::app::ServerClusterInterface *cluster =
      esp_matter::data_model::provider::get_instance().registry().Get(
          chip::app::ConcreteClusterPath(endpointId, clusterId));
  return static_cast<ClusterType *>(cluster);
}

// Updates the BooleanState cluster value on the Matter data model.
// Returns ESP_OK on success, ESP_ERR_NOT_FOUND if the cluster doesn't
// exist on this endpoint, or ESP_FAIL if the value didn't change.
// We check GetStateValue() before calling SetStateValue() to avoid
// unnecessary Matter stack updates for identical consecutive values.
static esp_err_t updateBooleanState(uint16_t endpointId, bool active) {
  auto *cluster = matterServerCluster<chip::app::Clusters::BooleanStateCluster>(
      endpointId, chip::app::Clusters::BooleanState::Id);
  if (!cluster) return ESP_ERR_NOT_FOUND;
  if (cluster->GetStateValue() == active) return ESP_OK;  // No change, skip
  return cluster->SetStateValue(active).has_value() ? ESP_OK : ESP_FAIL;
}

// Updates the TemperatureMeasurement cluster value. When valid is false
// (sensor fault or no reading), the measured value is set to null to
// indicate "invalid" to the Matter controller rather than showing a
// bogus temperature.
static esp_err_t updateTemperatureState(uint16_t endpointId, bool valid, int16_t value) {
  auto *cluster = matterServerCluster<chip::app::Clusters::TemperatureMeasurementCluster>(
      endpointId, chip::app::Clusters::TemperatureMeasurement::Id);
  if (!cluster) return ESP_ERR_NOT_FOUND;

  chip::app::DataModel::Nullable<int16_t> measuredValue;
  if (valid) {
    measuredValue.SetNonNull(value);
  } else {
    measuredValue.SetNull();
  }
  return cluster->SetMeasuredValue(measuredValue) == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
}

// Runs on the Matter thread. Copies the coalesced sensor state under the
// spinlock, clears the send flags and pending flag, then applies each
// sensor update to its Matter endpoint. Only sensors with non-zero
// endpoint IDs are updated (not all sensor channels have Matter endpoints).
static void updateSensorsOnMatterThread(intptr_t) {
  bool sendWaterLevel;
  bool waterLevelActive;
  bool sendFlow;
  bool flowActive;
  bool sendTemperature;
  bool temperatureValid;
  int16_t temperatureValue;

  // Snapshot under lock so main loop can queue new samples immediately.
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

  // Apply each sensor update to its Matter endpoint. Only proceed if the
  // endpoint exists (non-zero ID) — some sensor channels may not be
  // exposed as Matter endpoints.
  if (sendWaterLevel && waterLevelSensorEndpointId != 0) {
    logMatterUpdateError(
        "water level", updateBooleanState(waterLevelSensorEndpointId, waterLevelActive));
  }

  if (sendFlow && flowSensorEndpointId != 0) {
    logMatterUpdateError("flow", updateBooleanState(flowSensorEndpointId, flowActive));
  }

  if (sendTemperature && temperatureSensorEndpointId != 0) {
    logMatterUpdateError(
        "temperature",
        updateTemperatureState(temperatureSensorEndpointId, temperatureValid, temperatureValue));
  }
}

// Called from the main loop when new sensor data is available. Schedules
// the Matter update callback if one isn't already pending. If scheduling
// fails, clears the pending flag so the next sample will retry.
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

// ---- Sensor-specific queue helpers ----

// Each queue function updates the coalesced state under the spinlock and
// triggers a deferred Matter update. These are called from the main loop
// (sampleSensors) when a sensor value changes.

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

// Temperature is stored as hundredths of a degree Celsius (Matter
// TemperatureMeasurement cluster format). The valid flag is separate from
// the value — Matter supports reporting "null" for invalid readings.
static void queueTemperatureMatterUpdate(bool valid, int16_t value) {
  taskENTER_CRITICAL(&sensorMatterMux);
  sensorMatterReport.temperatureValid = valid;
  sensorMatterReport.temperatureValue = value;
  sensorMatterReport.sendTemperature = true;
  taskEXIT_CRITICAL(&sensorMatterMux);
  scheduleSensorMatterUpdate();
}

// ---- Bit-banged OneWire for DS18B20 ----
//
// The ESP-IDF doesn't include a OneWire driver, and adding a third-party
// library dependency for what amounts to ~100 lines of GPIO bit-banging
// is unnecessary. This minimal implementation targets only the DS18B20
// (single-drop bus, one sensor per GPIO) and skips ROM matching (uses
// Skip ROM 0xCC) since each sensor has its own dedicated pin.
//
// Critical sections disable interrupts around timing-sensitive bit
// operations. The DS18B20 requires microsecond-precision pulses; a
// FreeRTOS context switch during a bit write would violate timing and
// corrupt the transaction. The interrupt-disabled windows are
// intentionally short (60–480 µs) to minimize scheduling jitter.
//
// Open-drain output mode with no pull resistors lets the sensor pull the
// line low during its response phase while the ESP32 drives it low for
// the master-initiated time slots.

// Configures the GPIO pin as open-drain output for OneWire.
// Open-drain allows the sensor to pull the line low without fighting the
// ESP32's output driver. No pull-up/pull-down is enabled — the OneWire
// bus requires an external 4.7k pull-up resistor to VCC.
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

// Writes a single bit to the OneWire bus. Timing is per the DS18B20
// datasheet: write-1 = 2µs low + 58µs high, write-0 = 60µs low + 2µs high.
// The sensor samples the line 15–60µs after the falling edge.
static void ow_write_bit(gpio_num_t pin, bool bit) {
  taskDISABLE_INTERRUPTS();
  if (bit) {
    gpio_set_level(pin, 0);
    esp_rom_delay_us(2);   // Master pulls low for 2 µs
    gpio_set_level(pin, 1);
    esp_rom_delay_us(58);  // Release, sensor holds line high
  } else {
    gpio_set_level(pin, 0);
    esp_rom_delay_us(60);  // Master holds low for 60 µs
    gpio_set_level(pin, 1);
    esp_rom_delay_us(2);   // Recovery time before next bit
  }
  taskENABLE_INTERRUPTS();
}

// Reads a single bit from the OneWire bus. Timing: master pulls low for
// 2 µs, releases for 10 µs, then samples the line. The sensor will hold
// the line low for ~15 µs to signal a 0, or release it for a 1.
static bool ow_read_bit(gpio_num_t pin) {
  taskDISABLE_INTERRUPTS();
  gpio_set_level(pin, 0);
  esp_rom_delay_us(2);    // Master pulls low for 2 µs
  gpio_set_level(pin, 1);
  esp_rom_delay_us(10);   // Wait for sensor to drive the line
  bool bit = gpio_get_level(pin);  // Sample before sensor releases
  esp_rom_delay_us(48);   // Complete the 60 µs time slot
  taskENABLE_INTERRUPTS();
  return bit;
}

// Sends a reset pulse and checks for the sensor's presence pulse.
// Per the datasheet: master pulls low for 480 µs, releases for 70 µs,
// then samples. The sensor responds by pulling low for 60–240 µs.
// Returns true if a sensor was detected (presence pulse received).
static bool ow_reset(gpio_num_t pin) {
  ow_pin_output(pin);

  taskDISABLE_INTERRUPTS();
  gpio_set_level(pin, 0);
  esp_rom_delay_us(480);  // Reset pulse: low for 480 µs
  gpio_set_level(pin, 1);
  esp_rom_delay_us(70);   // Wait for sensor presence pulse
  bool present = gpio_get_level(pin) == 0;  // Low = sensor present
  esp_rom_delay_us(410);  // Complete the 480 µs recovery window
  taskENABLE_INTERRUPTS();

  return present;
}

// Writes a byte LSB-first (OneWire convention). Each byte is 8 bit time
// slots, transmitted least-significant-bit first.
static void ow_write_byte(gpio_num_t pin, uint8_t val) {
  for (int i = 0; i < 8; i++) {
    ow_write_bit(pin, (val >> i) & 1);
  }
}

// Reads a byte LSB-first from the bus. Each received bit is shifted into
// position i, building the byte from LSB to MSB.
static uint8_t ow_read_byte(gpio_num_t pin) {
  uint8_t val = 0;
  for (int i = 0; i < 8; i++) {
    if (ow_read_bit(pin)) {
      val |= (1 << i);
    }
  }
  return val;
}

// ---- DS18B20 helpers ----

// Sends the Convert T command (0x44) to start a temperature conversion.
// The conversion takes up to 750 ms at 12-bit resolution. We use Skip ROM
// (0xCC) because there's only one sensor per GPIO pin — no need to address
// a specific 64-bit ROM code.
static void ds18b20_request(gpio_num_t pin) {
  if (!ow_reset(pin)) return;  // No sensor on this bus — nothing to do
  ow_write_byte(pin, 0xCC);    // Skip ROM (single-drop bus)
  ow_write_byte(pin, 0x44);    // Convert T — starts temperature conversion
}

// Reads the scratchpad and returns the temperature in degrees Celsius.
// Sends Skip ROM (0xCC) then Read Scratchpad (0xBE). Reads only the first
// two bytes (temperature LSB and MSB) then aborts the rest of the
// scratchpad read with a reset pulse. We don't validate the CRC because
// the DS18B20 is on a dedicated GPIO with a short trace — CRC errors are
// extremely unlikely in this configuration.
//
// Returns NAN if no sensor responds to the reset pulse.
static float ds18b20_read(gpio_num_t pin) {
  if (!ow_reset(pin)) return NAN;
  ow_write_byte(pin, 0xCC);    // Skip ROM
  ow_write_byte(pin, 0xBE);    // Read Scratchpad

  uint8_t lsb = ow_read_byte(pin);
  uint8_t msb = ow_read_byte(pin);

  // Abort the scratchpad read early — we only need the first two bytes.
  // A full scratchpad read would be 9 bytes; the reset terminates it.
  ow_reset(pin);

  // DS18B20 stores temperature as a signed 16-bit integer in 0.0625°C
  // increments. Formula: raw * 0.0625.
  int16_t raw = ((int16_t)msb << 8) | lsb;
  return raw * 0.0625f;
}

// ---- Sensor setup ----

// Maps the board sensor config type enum to the runtime SensorType enum.
// The board config is compile-time (defined in board_sensor_config.h);
// the runtime enum is used by the cloud config parser and Matter endpoint
// creation.
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

// Called once at boot. Iterates the board sensor config, translates each
// sensor type, and initializes the GPIO pin with the debounce state machine.
// The sensorConfigs array is populated here — the cloud shadow delta can
// later override individual fields (enable, name, lockout) at runtime.
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

// ---- Sensor sampling ----

// Called from the main loop at ~100 Hz. Rate-limited internally:
//   - Digital sensors sampled at SENSOR_SAMPLE_MS (100 ms)
//   - Temperature sensors sampled at TEMPERATURE_SAMPLE_MS (30 s)
//
// For digital sensors, each sensor's debounce state machine is advanced,
// then the stable values are collected. Matter attribute updates are only
// queued when the value actually changes — this prevents ESP_ERR_NOT_SUPPORTED
// spam from internally-managed Matter attributes that reject no-op updates.
//
// For DS18B20 temperature sensors, the two-phase read cycle is managed by
// the temperatureC[] array state:
//   - NaN: no conversion has been requested yet → send Convert T
//   - 0.0f: conversion is in progress → read scratchpad, then start next
//   - valid float: previous read was successful → read scratchpad
void sampleSensors() {
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

  // Digital sensor sampling — rate-limited to 100 ms.
  if (now - lastSensorSampleMs < SENSOR_SAMPLE_MS) return;
  lastSensorSampleMs = now;

  bool waterLevelActive = false;
  bool flowActive = false;

  // Advance each sensor's debounce state machine and collect the latest
  // stable values for water level and flow sensors.
  for (uint8_t i = 0; i < BoardPins::SensorCount; i++) {
    sensorInputs[i].update(BoardPins::SensorPins[i]);
    if (!sensorConfigs[i].enabled) continue;
    if (sensorConfigs[i].type == SensorType::WaterLevelSwitch) {
      waterLevelActive = sensorInputs[i].stable;
    } else if (sensorConfigs[i].type == SensorType::FlowSwitch) {
      flowActive = sensorInputs[i].stable;
    }
  }

  // Only queue Matter updates when the value actually changes. Without
  // this guard, every sample would trigger an update to the Matter stack,
  // and internally-managed attributes return ESP_ERR_NOT_SUPPORTED on
  // no-op updates, flooding the console with warnings.
  static bool lastWaterLevel = false;
  static bool lastFlow = false;
  static bool firstUpdate = true;  // Force first update even if unchanged

  if (waterLevelSensorEndpointId != 0 && (firstUpdate || waterLevelActive != lastWaterLevel)) {
    queueWaterLevelMatterUpdate(waterLevelActive);
    lastWaterLevel = waterLevelActive;
  }

  if (flowSensorEndpointId != 0 && (firstUpdate || flowActive != lastFlow)) {
    queueFlowMatterUpdate(flowActive);
    lastFlow = flowActive;
  }

  firstUpdate = false;

  // Temperature sampling — rate-limited to 30 s.
  // DS18B20 conversions take up to 750 ms, so we use a two-phase approach:
  //   Phase 1 (NaN → request): send Convert T, set to 0.0f as "pending"
  //   Phase 2 (0.0f → read):   read scratchpad, request next, store value
  // Each sensor is independent — they can be in different phases at the
  // same time. This is important because different sensors may have been
  // powered on at different times.
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
        // Phase 1: No conversion has been requested yet (or previous read
        // failed and reset to NaN). Send Convert T and mark as pending.
        ds18b20_request(pin);
        temperatureC[i] = 0.0f;  // Sentinel: conversion in progress
      } else {
        // Phase 2: Conversion result should be ready. Read the scratchpad
        // and immediately start the next conversion for the next cycle.
        float value = ds18b20_read(pin);
        ds18b20_request(pin);  // Start next conversion now

        // Validate the reading. -55°C is the DS18B20 power-on default
        // (means no valid conversion happened). >125°C is out of spec.
        // These thresholds are per the DS18B20 datasheet.
        if (isnan(value) || value < -55.0f || value > 125.0f) {
          temperatureC[i] = NAN;
          temperatureFault[i] = true;
        } else {
          temperatureC[i] = value;
          temperatureFault[i] = false;
          temperatureUpdated = true;

          if (temperatureSensorEndpointId != 0) {
            // Matter TemperatureMeasurement uses hundredths of a degree.
            int16_t matterTemp = (int16_t)(value * 100);
            queueTemperatureMatterUpdate(true, matterTemp);
          }
        }
      }
    }

    // If no sensor produced a valid reading this cycle, report the
    // temperature as invalid (null) to Matter so the dashboard shows
    // "no data" instead of a stale temperature.
    if (!temperatureUpdated && temperatureSensorEndpointId != 0) {
      queueTemperatureMatterUpdate(false, 0);
    }
  }
}
